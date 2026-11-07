#include "game.h"
#include <jansson.h>

/**
 * @brief One editable field and its shared browsing and search popup.
 */
typedef struct {
    GtkWidget parent;
    GtkWidget* entry;
    GtkWidget* arrow;
    GtkPopover* popup;
    GtkWidget* list;
    GtkWidget* empty;
    GtkSingleSelection* selection;
    GtkEventController* dismiss;
    GListStore* matches;
    GListModel* browse;
    GtkStringList* names;
    gboolean game;
    gboolean updating;
    guint pending;
} NameField;

typedef GtkWidgetClass NameFieldClass;

/**
 * @brief The suggestions provided by a field.
 */
typedef enum {
    NAME_FIELD_GAME,
    NAME_FIELD_CATEGORY,
} NameFieldKind;

/**
 * @brief A matching name and its distance from the search text.
 */
typedef struct {
    const char* name;
    glong rank;
    guint position;
} NameMatch;

static const char* GAMES_URL = "https://www.speedrun.com/api/v1/games?_bulk=yes&max=1000";
static const char* CATEGORIES_URL = "https://www.speedrun.com/api/v1/games?";
static const char* CATEGORIES_URL_FULL = "https://www.speedrun.com/api/v1/games/%s/categories";
static const char* FALLBACK_CATEGORIES[] = { "Any%", "100%", NULL };

/**
 * @brief Uses the text child for editing.
 *
 * @param editable The game or category field.
 * @return GtkEditable* The field's text child, without a new reference.
 */
static GtkEditable* name_field_get_delegate(GtkEditable* editable)
{
    return GTK_EDITABLE(((NameField*)editable)->entry);
}

/**
 * @brief Provides the editable field's text interface.
 *
 * @param interface The editable interface to initialize.
 */
static void name_field_editable_init(GtkEditableInterface* interface)
{
    interface->get_delegate = name_field_get_delegate;
}

G_DEFINE_TYPE_WITH_CODE(NameField, name_field, GTK_TYPE_WIDGET,
    G_IMPLEMENT_INTERFACE(GTK_TYPE_EDITABLE, name_field_editable_init))

/**
 * @brief Game names and search aliases shared for this application session.
 */
static struct {
    GtkStringList* names;
    GListModel* sorted;
    json_t* games;
    json_t* refreshed;
    char* cache_path;
} catalogue;

/**
 * @brief The selected game and category request for one editor.
 */
typedef struct {
    GtkEditable* game;
    GtkEditable* category;
    char* selected_id;
    GCancellable* categories_request;
} GameFields;

/**
 * @brief Adds an abbreviation only once.
 *
 * @param aliases The list receiving the alias.
 * @param name The alias text; ownership is transferred.
 */
static void add_alias(GPtrArray* aliases, char* name)
{
    g_strstrip(name);
    if (!g_ptr_array_find_with_equal_func(aliases, name, (GEqualFunc)g_str_equal, NULL)) {
        g_ptr_array_add(aliases, name);
    } else {
        g_free(name);
    }
}

/**
 * @brief Builds title, subtitle and initial abbreviations.
 *
 * @param title The game title to abbreviate.
 * @return GPtrArray* The alias list, owned by the caller.
 */
static GPtrArray* abbreviations(const char* title)
{
    GPtrArray* aliases = g_ptr_array_new_with_free_func(g_free);
    char* name = g_strstrip(g_strdup(title));
    add_alias(aliases, g_strdup(name));
    char* open = strrchr(name, '(');
    char* close = open ? strchr(open, ')') : NULL;
    const char* separators[] = { ": ", " - ", " | " };
    const char* separator = NULL;
    char* split = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(separators); ++i) {
        split = strstr(name, separators[i]);
        if (split) {
            separator = separators[i];
            break;
        }
    }

    char* lower = g_ascii_strdown(name, -1);
    char* and = strstr(lower, " and ");
    if (close || (and && !split)) {
        char* replacement;
        if (close) {
            *open = '\0';
            replacement = g_strconcat(g_strstrip(name), " ", g_strstrip(close + 1), NULL);
        } else {
            name[and - lower] = '\0';
            replacement = g_strconcat(name, " & ", name + (and - lower) + 5, NULL);
        }

        GPtrArray* parts = abbreviations(replacement);
        g_ptr_array_extend_and_steal(aliases, parts);
        g_free(replacement);
    } else if (split) {
        *split = '\0';
        GPtrArray* series = abbreviations(name);
        GPtrArray* subtitle = abbreviations(split + strlen(separator));
        char* trimmed = g_strstrip(g_strdup(name));
        gsize length = strlen(trimmed);
        gsize roman = length;
        while (roman && strchr("IVX", trimmed[roman - 1])) {
            --roman;
        }

        gboolean numbered = roman < length && (!roman || trimmed[roman - 1] == ' ');
        if (length) {
            gunichar last = g_utf8_get_char(g_utf8_find_prev_char(trimmed, trimmed + length));
            numbered |= g_unichar_isdigit(last);
        }

        gboolean keep_both = strcmp(separator, " | ") == 0;
        for (guint i = 0; i < subtitle->len; ++i) {
            const char* sub = g_ptr_array_index(subtitle, i);
            if (!keep_both) {
                add_alias(aliases, g_strdup(sub));
            }

            for (guint j = 0; j < series->len; ++j) {
                const char* part = g_ptr_array_index(series, j);
                if (numbered && !keep_both) {
                    add_alias(aliases, g_strdup(part));
                }

                if (keep_both || numbered || series->len == 1 || strcmp(part, name)) {
                    add_alias(aliases, g_strconcat(part, separator, sub, NULL));
                }
            }
        }

        g_free(trimmed);
        g_ptr_array_unref(series);
        g_ptr_array_unref(subtitle);
    } else {
        if (g_str_has_prefix(lower, "the ") || g_str_has_prefix(lower, "a ")) {
            add_alias(aliases, g_strdup(name + (lower[0] == 't' ? 4 : 2)));
        }

        if (strchr(name, ' ')) {
            g_strdelimit(name, "&", 'a');
            char** words = g_strsplit_set(name, " -", -1);
            GString* initials = g_string_new(NULL);
            for (guint i = 0; words[i]; ++i) {
                const char* word = words[i];
                if (!*word) {
                    continue;
                }

                gboolean capitals = g_utf8_strlen(word, -1) <= 4;
                for (const char* p = word; capitals && *p; p = g_utf8_next_char(p)) {
                    gunichar c = g_utf8_get_char(p);
                    capitals = g_unichar_isupper(c) || g_unichar_isdigit(c);
                }

                if (g_unichar_isdigit(g_utf8_get_char(word))) {
                    g_string_append(initials, word);
                } else if (capitals) {
                    g_string_append_c(initials, ' ');
                    g_string_append(initials, word);
                } else {
                    g_string_append_unichar(initials, g_utf8_get_char(word));
                }
            }

            add_alias(aliases, g_string_free(initials, FALSE));
            g_strfreev(words);
        }
    }

    g_free(lower);
    g_free(name);
    return aliases;
}

/**
 * @brief Prepares case-insensitive search text once for each name.
 *
 * @param name The name to prepare for searching.
 * @param game TRUE to include game abbreviations.
 * @return char** The NULL-terminated aliases. free with g_strfreev.
 */
static char** search_aliases(const char* name, gboolean game)
{
    GPtrArray* aliases = game ? abbreviations(name) : g_ptr_array_new_with_free_func(g_free);
    if (!game) {
        g_ptr_array_add(aliases, g_strdup(name));
    }

    for (guint i = 0; i < aliases->len; ++i) {
        char* original = g_ptr_array_index(aliases, i);
        g_ptr_array_index(aliases, i) = g_utf8_casefold(original, -1);
        g_free(original);
    }

    g_ptr_array_add(aliases, NULL);
    return (char**)g_ptr_array_free(aliases, FALSE);
}

/**
 * @brief Caches search aliases alongside a set of names.
 *
 * @param choices The NULL-terminated names, or NULL for an empty list.
 * @param game TRUE to include game abbreviations in searches.
 * @return GtkStringList* A new reference to the names and their search aliases.
 */
static GtkStringList* new_names(const char* const* choices, gboolean game)
{
    GtkStringList* names = gtk_string_list_new(NULL);
    GHashTable* aliases = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_strfreev);
    for (guint i = 0; choices && choices[i]; ++i) {
        if (!g_hash_table_contains(aliases, choices[i])) {
            gtk_string_list_append(names, choices[i]);
            g_hash_table_insert(aliases, g_strdup(choices[i]), search_aliases(choices[i], game));
        }
    }

    g_object_set_data_full(G_OBJECT(names), "aliases", aliases, (GDestroyNotify)g_hash_table_unref);
    return names;
}

/**
 * @brief Ranks matching aliases, then shorter full names.
 *
 * @param a The first NameMatch.
 * @param b The second NameMatch.
 * @return int Negative, zero, or positive for the match order.
 */
static int compare_matches(gconstpointer a, gconstpointer b)
{
    const NameMatch* left = a;
    const NameMatch* right = b;
    if (left->rank != right->rank) {
        return left->rank > right->rank ? 1 : -1;
    }

    glong length = g_utf8_strlen(left->name, -1) - g_utf8_strlen(right->name, -1);
    return length ? (length > 0 ? 1 : -1) : strcmp(left->name, right->name);
}

/**
 * @brief Reuses GTK's alphabetical view of a catalogue.
 *
 * @param names The names to sort.
 * @return GListModel* A new reference to the sorted view.
 */
static GListModel* sorted_names(GtkStringList* names)
{
    GtkStringSorter* sorter = gtk_string_sorter_new(gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string"));
    gtk_string_sorter_set_ignore_case(sorter, FALSE);
    GtkSortListModel* sorted = gtk_sort_list_model_new(G_LIST_MODEL(g_object_ref(names)), GTK_SORTER(sorter));
    gtk_sort_list_model_set_incremental(sorted, TRUE);
    return G_LIST_MODEL(sorted);
}

/**
 * @brief Filters suggestions while keeping custom text and the highlighted row.
 * Substring matches rank by their extra characters.
 *
 * @param data The name field whose suggestions should be updated.
 * @return gboolean Always G_SOURCE_REMOVE.
 */
static gboolean update_choices(gpointer data)
{
    NameField* field = data;
    field->pending = 0;
    const char* text = gtk_editable_get_text(GTK_EDITABLE(field->entry));
    GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(field->entry));
    GtkWidget* focus = root ? gtk_root_get_focus(root) : NULL;
    gboolean navigating = focus && (focus == field->list || gtk_widget_is_ancestor(focus, field->list));
    GtkStringObject* highlighted = gtk_single_selection_get_selected_item(field->selection);
    char* previous = highlighted ? g_strdup(gtk_string_object_get_string(highlighted)) : NULL;
    if (!*text) {
        gtk_single_selection_set_model(field->selection, field->browse);
    } else {
        char* query = g_utf8_casefold(text, -1);
        glong query_length = g_utf8_strlen(query, -1);
        GHashTable* aliases = g_object_get_data(G_OBJECT(field->names), "aliases");
        GArray* matches = g_array_new(FALSE, FALSE, sizeof(NameMatch));
        for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(field->names)); ++i) {
            const char* name = gtk_string_list_get_string(field->names, i);
            char** alternatives = g_hash_table_lookup(aliases, name);
            glong rank = G_MAXLONG;
            for (guint j = 0; alternatives[j]; ++j) {
                if (strstr(alternatives[j], query)) {
                    rank = MIN(rank, g_utf8_strlen(alternatives[j], -1) - query_length);
                }
            }

            if (strcmp(name, text) == 0) {
                rank = -1;
            }

            if (rank != G_MAXLONG) {
                NameMatch match = { name, rank, i };
                g_array_append_val(matches, match);
            }
        }

        if (field->game) {
            g_array_sort(matches, compare_matches);
        }

        GPtrArray* items = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = 0; i < matches->len; ++i) {
            NameMatch* match = &g_array_index(matches, NameMatch, i);
            g_ptr_array_add(items, g_list_model_get_item(G_LIST_MODEL(field->names), match->position));
        }

        g_list_store_splice(field->matches, 0, g_list_model_get_n_items(G_LIST_MODEL(field->matches)), items->pdata, items->len);
        gtk_single_selection_set_model(field->selection, G_LIST_MODEL(field->matches));
        g_ptr_array_unref(items);
        g_array_unref(matches);
        g_free(query);
    }

    GListModel* model = gtk_single_selection_get_model(field->selection);
    guint count = g_list_model_get_n_items(model);
    guint selected = gtk_single_selection_get_selected(field->selection);
    if (*text) {
        selected = GTK_INVALID_LIST_POSITION;
        for (guint i = 0; i < count; ++i) {
            GtkStringObject* item = g_list_model_get_item(model, i);
            const char* name = gtk_string_object_get_string(item);
            if (!strcmp(name, text) || g_strcmp0(name, previous) == 0) {
                selected = i;
            }

            g_object_unref(item);
        }
    }

    gtk_single_selection_set_selected(field->selection, selected);
    if (navigating && selected != GTK_INVALID_LIST_POSITION) {
        gtk_list_view_scroll_to(GTK_LIST_VIEW(field->list), selected, GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT, NULL);
    }

    gtk_widget_set_visible(field->empty, count == 0);
    g_free(previous);
    return G_SOURCE_REMOVE;
}

/**
 * @brief Queues one update for the latest suggestions.
 *
 * @param field The name field to update.
 */
static void queue_choices_update(NameField* field)
{
    if (!field->pending) {
        field->pending = g_idle_add(update_choices, field);
    }
}

/**
 * @brief Queues filtering and opens the same popup used by the arrow.
 *
 * @param entry The edited text child.
 * @param field The name field containing the entry.
 */
static void text_changed(GtkEditable* entry, NameField* field)
{
    if (field->updating) {
        return;
    }

    gtk_single_selection_set_selected(field->selection, GTK_INVALID_LIST_POSITION);
    queue_choices_update(field);

    GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(entry));
    GtkWidget* focus = root ? gtk_root_get_focus(root) : NULL;
    if (focus && (focus == GTK_WIDGET(entry) || gtk_widget_is_ancestor(focus, GTK_WIDGET(entry)))) {
        gtk_popover_popup(field->popup);
        gtk_text_grab_focus_without_selecting(GTK_TEXT(field->entry));
    }
}

/**
 * @brief Refreshes an entry when its available names change.
 *
 * @param names The changed name list (unused).
 * @param position The first changed position (unused).
 * @param removed The number of removed names (unused).
 * @param added The number of added names (unused).
 * @param field The name field to refresh.
 */
static void names_changed(GListModel* names, guint position, guint removed, guint added, NameField* field)
{
    queue_choices_update(field);
}

/**
 * @brief Creates the visible label for a suggestion.
 *
 * @param factory The list item factory (unused).
 * @param item The list item receiving a label.
 * @param data Unused callback data.
 */
static void suggestion_setup(GtkSignalListItemFactory* factory, GtkListItem* item, gpointer data)
{
    GtkWidget* label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_list_item_set_child(item, label);
}

/**
 * @brief Displays the name assigned to a visible row.
 *
 * @param factory The list item factory (unused).
 * @param item The list item containing the name to display.
 * @param data Unused callback data.
 */
static void suggestion_bind(GtkSignalListItemFactory* factory, GtkListItem* item, gpointer data)
{
    GtkStringObject* name = gtk_list_item_get_item(item);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), gtk_string_object_get_string(name));
}

/**
 * @brief Accepts an explicit selection and closes its popup.
 *
 * @param list The suggestion list (unused).
 * @param position The zero-based selected position.
 * @param field The name field receiving the selection.
 */
static void suggestion_activated(GtkListView* list, guint position, NameField* field)
{
    GtkStringObject* name = g_list_model_get_item(G_LIST_MODEL(field->selection), position);
    gtk_single_selection_set_selected(field->selection, position);
    field->updating = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(field->entry), gtk_string_object_get_string(name));
    gtk_editable_set_position(GTK_EDITABLE(field->entry), -1);
    field->updating = FALSE;
    g_object_unref(name);
    queue_choices_update(field);

    gtk_popover_popdown(field->popup);
    gtk_text_grab_focus_without_selecting(GTK_TEXT(field->entry));
}

/**
 * @brief Lets Down navigate suggestions and Escape dismiss them.
 *
 * @param controller The key event controller (unused).
 * @param key The pressed key.
 * @param code The hardware key code (unused).
 * @param state The modifier keys (unused).
 * @param field The name field handling the key.
 * @return gboolean TRUE if the key was consumed.
 */
static gboolean suggestions_key(GtkEventControllerKey* controller, guint key, guint code, GdkModifierType state, NameField* field)
{
    GtkWidget* popup = GTK_WIDGET(field->popup);
    if (key == GDK_KEY_Escape && gtk_widget_get_visible(popup)) {
        gtk_popover_popdown(field->popup);
        gtk_text_grab_focus_without_selecting(GTK_TEXT(field->entry));
        return TRUE;
    }

    if (key == GDK_KEY_Tab || key == GDK_KEY_ISO_Left_Tab) {
        gtk_popover_popdown(field->popup);
        gtk_text_grab_focus_without_selecting(GTK_TEXT(field->entry));
        return FALSE;
    }

    GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(field->entry));
    GtkWidget* focus = root ? gtk_root_get_focus(root) : NULL;
    if (key != GDK_KEY_Down || (focus && gtk_widget_is_ancestor(focus, popup))) {
        return FALSE;
    }

    if (field->pending) {
        g_source_remove(field->pending);
        update_choices(field);
    }

    gtk_popover_popup(field->popup);
    if (g_list_model_get_n_items(G_LIST_MODEL(field->selection))) {
        guint selected = gtk_single_selection_get_selected(field->selection);
        gtk_widget_child_focus(field->list, GTK_DIR_TAB_FORWARD);
        gtk_list_view_scroll_to(GTK_LIST_VIEW(field->list), selected == GTK_INVALID_LIST_POSITION ? 0 : selected,
            GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT, NULL);
    }

    return TRUE;
}

/**
 * @brief Toggles the same suggestions popup used while typing.
 *
 * @param gesture The arrow click gesture (unused).
 * @param presses The click count (unused).
 * @param x The click x-coordinate (unused).
 * @param y The click y-coordinate (unused).
 * @param field The name field whose popup should be toggled.
 */
static void arrow_released(GtkGestureClick* gesture, int presses, double x, double y, NameField* field)
{
    if (gtk_widget_get_visible(GTK_WIDGET(field->popup))) {
        gtk_popover_popdown(field->popup);
    } else {
        gtk_popover_popup(field->popup);
    }

    gtk_text_grab_focus_without_selecting(GTK_TEXT(field->entry));
}

/**
 * @brief Closes suggestions when a click lands outside this control.
 *
 * @param gesture The click gesture on the window.
 * @param presses The click count (unused).
 * @param x The click x-coordinate in the window.
 * @param y The click y-coordinate in the window.
 * @param field The name field whose popup may need to close.
 */
static void outside_pressed(GtkGestureClick* gesture, int presses, double x, double y, NameField* field)
{
    GtkPopover* popup = field->popup;
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    if (!gtk_widget_get_visible(GTK_WIDGET(popup))
        || (event && gdk_event_get_surface(event) == gtk_native_get_surface(GTK_NATIVE(popup)))) {
        return;
    }

    GtkWidget* root = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GtkWidget* target = gtk_widget_pick(root, x, y, GTK_PICK_DEFAULT);
    if (!target || (target != GTK_WIDGET(field) && !gtk_widget_is_ancestor(target, GTK_WIDGET(field)))) {
        gtk_popover_popdown(field->popup);
    }
}

/**
 * @brief Watches outside clicks while the field is on screen.
 *
 * @param widget The newly visible name field.
 * @param field The name field state.
 */
static void name_field_mapped(GtkWidget* widget, NameField* field)
{
    field->dismiss = GTK_EVENT_CONTROLLER(gtk_gesture_click_new());
    gtk_event_controller_set_propagation_phase(field->dismiss, GTK_PHASE_CAPTURE);
    g_signal_connect(field->dismiss, "pressed", G_CALLBACK(outside_pressed), field);
    gtk_widget_add_controller(GTK_WIDGET(gtk_widget_get_root(widget)), g_object_ref(field->dismiss));
}

/**
 * @brief Stops watching outside clicks when the field is hidden.
 *
 * @param widget The hidden name field (unused).
 * @param field The name field state.
 */
static void name_field_unmapped(GtkWidget* widget, NameField* field)
{
    gtk_popover_popdown(field->popup);
    if (field->dismiss) {
        GtkWidget* root = gtk_event_controller_get_widget(field->dismiss);
        if (root) {
            gtk_widget_remove_controller(root, field->dismiss);
        }

        g_clear_object(&field->dismiss);
    }
}

/**
 * @brief Releases suggestions and any queued update.
 *
 * @param object The name field to release.
 */
static void name_field_dispose(GObject* object)
{
    NameField* field = (NameField*)object;
    g_clear_handle_id(&field->pending, g_source_remove);
    if (field->entry) {
        name_field_unmapped(NULL, field);
        gtk_editable_finish_delegate(GTK_EDITABLE(field));
        gtk_widget_unparent(field->entry);
        field->entry = NULL;
        gtk_widget_unparent(field->arrow);
        gtk_widget_unparent(GTK_WIDGET(field->popup));
    }

    if (field->names) {
        g_signal_handlers_disconnect_by_data(field->names, field);
    }

    g_clear_object(&field->names);
    g_clear_object(&field->browse);
    g_clear_object(&field->matches);
    G_OBJECT_CLASS(name_field_parent_class)->dispose(object);
}

/**
 * @brief Reads a property from the editable text child.
 *
 * @param object The name field.
 * @param id The property ID.
 * @param value Receives the property value.
 * @param spec The property definition.
 */
static void name_field_get_property(GObject* object, guint id, GValue* value, GParamSpec* spec)
{
    if (!gtk_editable_delegate_get_property(object, id, value, spec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}

/**
 * @brief Writes a property to the editable text child.
 *
 * @param object The name field.
 * @param id The property ID.
 * @param value The new property value.
 * @param spec The property definition.
 */
static void name_field_set_property(GObject* object, guint id, const GValue* value, GParamSpec* spec)
{
    if (!gtk_editable_delegate_set_property(object, id, value, spec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}

/**
 * @brief Measures the text and arrow as one field.
 *
 * @param widget The name field to measure.
 * @param orientation The dimension to measure.
 * @param for_size The available size in the other dimension, or -1 if unknown.
 * @param minimum Receives the minimum size.
 * @param natural Receives the preferred size.
 * @param minimum_baseline Receives the minimum baseline, or -1 if unavailable.
 * @param natural_baseline Receives the preferred baseline, or -1 if unavailable.
 */
static void name_field_measure(GtkWidget* widget, GtkOrientation orientation, int for_size,
    int* minimum, int* natural, int* minimum_baseline, int* natural_baseline)
{
    NameField* field = (NameField*)widget;
    int arrow_min, arrow_nat;
    gtk_widget_measure(field->arrow, orientation, -1, &arrow_min, &arrow_nat, NULL, NULL);
    gtk_widget_measure(field->entry, orientation, for_size, minimum, natural, minimum_baseline, natural_baseline);
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        *minimum += arrow_min;
        *natural += arrow_nat;
    } else {
        *minimum = MAX(*minimum, arrow_min);
        *natural = MAX(*natural, arrow_nat);
    }
}

/**
 * @brief Places the arrow beside the text and anchors the popup below them.
 *
 * @param widget The name field to arrange.
 * @param width The available width in pixels.
 * @param height The available height in pixels.
 * @param baseline The assigned baseline, or -1 if unavailable.
 */
static void name_field_size_allocate(GtkWidget* widget, int width, int height, int baseline)
{
    NameField* field = (NameField*)widget;
    int arrow_width;
    gtk_widget_measure(field->arrow, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &arrow_width, NULL, NULL);
    arrow_width = MIN(arrow_width, width);
    gboolean rtl = gtk_widget_get_direction(widget) == GTK_TEXT_DIR_RTL;
    gtk_widget_size_allocate(field->entry,
        &(GtkAllocation) { rtl ? arrow_width : 0, 0, width - arrow_width, height }, baseline);
    gtk_widget_size_allocate(field->arrow,
        &(GtkAllocation) { rtl ? 0 : width - arrow_width, 0, arrow_width, height }, baseline);

    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds(widget, widget, &bounds)) {
        gtk_widget_set_size_request(GTK_WIDGET(field->popup), bounds.size.width, -1);
    }

    gtk_popover_present(field->popup);
}

/**
 * @brief Sends focus to the text without changing its selection.
 *
 * @param widget The name field to focus.
 * @return gboolean TRUE if the text child received focus.
 */
static gboolean name_field_grab_focus(GtkWidget* widget)
{
    return gtk_text_grab_focus_without_selecting(GTK_TEXT(((NameField*)widget)->entry));
}

/**
 * @brief Configures the editable widget and its native entry styling.
 *
 * @param class The name field class to initialize.
 */
static void name_field_class_init(NameFieldClass* class)
{
    GObjectClass* object_class = G_OBJECT_CLASS(class);
    object_class->dispose = name_field_dispose;
    object_class->get_property = name_field_get_property;
    object_class->set_property = name_field_set_property;
    gtk_editable_install_properties(object_class, 1);
    class->measure = name_field_measure;
    class->size_allocate = name_field_size_allocate;
    class->grab_focus = name_field_grab_focus;
    gtk_widget_class_set_css_name(class, "entry");
    gtk_widget_class_set_accessible_role(class, GTK_ACCESSIBLE_ROLE_COMBO_BOX);
}

/**
 * @brief Reports whether suggestions are open to assistive tools.
 *
 * @param popup The suggestions popup.
 * @param spec The changed property (unused).
 * @param field The name field whose accessible state should be updated.
 */
static void popup_visible(GtkPopover* popup, GParamSpec* spec, NameField* field)
{
    gtk_accessible_update_state(GTK_ACCESSIBLE(field),
        GTK_ACCESSIBLE_STATE_EXPANDED, gtk_widget_get_visible(GTK_WIDGET(popup)), -1);
}

/**
 * @brief Builds the text, arrow and scrollable popup from GTK's example.
 *
 * @param field The name field to initialize.
 */
static void name_field_init(NameField* field)
{
    field->entry = gtk_text_new();
    gtk_widget_set_parent(field->entry, GTK_WIDGET(field));
    gtk_editable_init_delegate(GTK_EDITABLE(field));
    field->arrow = gtk_image_new_from_icon_name("pan-down-symbolic");
    gtk_widget_add_css_class(field->arrow, "right");
    gtk_widget_set_parent(field->arrow, GTK_WIDGET(field));
    GtkGesture* press = gtk_gesture_click_new();
    g_signal_connect(press, "released", G_CALLBACK(arrow_released), field);
    gtk_widget_add_controller(field->arrow, GTK_EVENT_CONTROLLER(press));

    field->selection = gtk_single_selection_new(NULL);
    gtk_single_selection_set_autoselect(field->selection, FALSE);
    gtk_single_selection_set_can_unselect(field->selection, TRUE);
    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(suggestion_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(suggestion_bind), NULL);
    field->list = gtk_list_view_new(GTK_SELECTION_MODEL(field->selection), factory);
    gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(field->list), TRUE);
    g_signal_connect(field->list, "activate", G_CALLBACK(suggestion_activated), field);
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 240);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), field->list);
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(content), scroll);
    field->empty = gtk_label_new("No matching suggestions");
    gtk_box_append(GTK_BOX(content), field->empty);
    field->popup = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_position(field->popup, GTK_POS_BOTTOM);
    gtk_popover_set_has_arrow(field->popup, FALSE);
    gtk_popover_set_autohide(field->popup, FALSE);
    gtk_popover_set_child(field->popup, content);
    gtk_widget_set_halign(GTK_WIDGET(field->popup), GTK_ALIGN_START);
    gtk_widget_add_css_class(GTK_WIDGET(field->popup), "menu");
    gtk_widget_set_parent(GTK_WIDGET(field->popup), GTK_WIDGET(field));
    g_signal_connect(field->popup, "notify::visible", G_CALLBACK(popup_visible), field);
    gtk_accessible_update_property(GTK_ACCESSIBLE(field),
        GTK_ACCESSIBLE_PROPERTY_AUTOCOMPLETE, GTK_ACCESSIBLE_AUTOCOMPLETE_LIST,
        GTK_ACCESSIBLE_PROPERTY_HAS_POPUP, TRUE, -1);
    gtk_accessible_update_relation(GTK_ACCESSIBLE(field), GTK_ACCESSIBLE_RELATION_CONTROLS, field->list, NULL, -1);

    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(suggestions_key), field);
    gtk_widget_add_controller(GTK_WIDGET(field), keys);
    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect_swapped(focus, "leave", G_CALLBACK(gtk_popover_popdown), field->popup);
    gtk_widget_add_controller(GTK_WIDGET(field), focus);
    g_signal_connect(field->entry, "changed", G_CALLBACK(text_changed), field);
    g_signal_connect(field, "map", G_CALLBACK(name_field_mapped), field);
    g_signal_connect(field, "unmap", G_CALLBACK(name_field_unmapped), field);
}

/**
 * @brief Adds a labelled editable field with its available names.
 *
 * @param grid The grid receiving the field.
 * @param column The zero-based grid column.
 * @param title The field label, including its mnemonic.
 * @param kind Whether the field provides games or categories.
 * @return GtkEditable* The field added to the grid.
 */
static GtkEditable* add_name_field(GtkGrid* grid, int column, const char* title, NameFieldKind kind)
{
    GtkWidget* label = gtk_label_new_with_mnemonic(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_grid_attach(grid, label, column, 0, 1, 1);

    NameField* field = g_object_new(name_field_get_type(), NULL);
    field->game = kind == NAME_FIELD_GAME;
    field->names = field->game ? g_object_ref(catalogue.names) : new_names(FALLBACK_CATEGORIES, FALSE);
    field->browse = field->game ? g_object_ref(catalogue.sorted) : G_LIST_MODEL(g_object_ref(field->names));
    field->matches = g_list_store_new(GTK_TYPE_STRING_OBJECT);
    g_signal_connect(field->names, "items-changed", G_CALLBACK(names_changed), field);
    update_choices(field);
    gtk_widget_set_hexpand(GTK_WIDGET(field), TRUE);
    gtk_editable_set_width_chars(GTK_EDITABLE(field), field->game ? 32 : 20);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), GTK_WIDGET(field));
    gtk_grid_attach(grid, GTK_WIDGET(field), column, 1, 1, 1);
    return GTK_EDITABLE(field);
}

/**
 * @brief Updates a field's suggestions without changing its entered text.
 *
 * @param entry The game or category field to update.
 * @param choices The NULL-terminated suggestions, or NULL to clear them.
 */
void ls_editor_game_set_choices(GtkEditable* entry, const char* const* choices)
{
    NameField* field = (NameField*)entry;
    g_signal_handlers_disconnect_by_data(field->names, field);
    g_object_unref(field->names);
    g_object_unref(field->browse);
    field->names = new_names(choices, field->game);
    field->browse = field->game ? sorted_names(field->names) : G_LIST_MODEL(g_object_ref(field->names));
    g_signal_connect(field->names, "items-changed", G_CALLBACK(names_changed), field);
    queue_choices_update(field);
}

/**
 * @brief Saves catalogue names and IDs without changing the splits file.
 */
static void save_cache(void)
{
    char* directory = g_path_get_dirname(catalogue.cache_path);
    if (g_mkdir_with_parents(directory, 0700) == 0) {
        char* contents = json_dumps(catalogue.games, JSON_COMPACT);
        if (!g_file_set_contents(catalogue.cache_path, contents, -1, NULL)) {
            g_debug("Could not cache game names in %s", catalogue.cache_path);
        }

        g_free(contents);
    }

    g_free(directory);
}

/**
 * @brief Stops category requests when this editor closes.
 *
 * @param fields The game and category container.
 * @param state The selected game and category request.
 */
static void fields_unmapped(GtkWidget* fields, GameFields* state)
{
    if (state->categories_request) {
        g_cancellable_cancel(state->categories_request);
    }

    g_signal_handlers_disconnect_by_data(catalogue.names, fields);
}

/**
 * @brief Releases the editor's category request and selection.
 *
 * @param data The game and category state to release.
 */
static void fields_free(gpointer data)
{
    GameFields* state = data;
    g_free(state->selected_id);
    g_clear_object(&state->categories_request);
    g_free(state);
}

/**
 * @brief Reads an API response, ignoring cancelled requests and malformed data.
 *
 * @param file The file containing the API response.
 * @param result The asynchronous read result.
 * @param request The request and its cancellation state.
 * @return json_t* The response owned by the caller, or NULL if unavailable or invalid.
 */
static json_t* response_finish(GObject* file, GAsyncResult* result, GTask* request)
{
    GBytes* bytes = g_file_load_bytes_finish(G_FILE(file), result, NULL, NULL);
    json_t* response = NULL;
    if (bytes && !g_cancellable_is_cancelled(g_task_get_cancellable(request))) {
        gsize size;
        const char* contents = g_bytes_get_data(bytes, &size);
        response = json_loadb(contents, size, 0, NULL);
        if (!json_is_array(json_object_get(response, "data"))) {
            json_decref(response);
            response = NULL;
        }
    }

    g_clear_pointer(&bytes, g_bytes_unref);
    return response;
}

/**
 * @brief Reads an API URL while retaining its widgets and cancellation token.
 *
 * @param fields The widgets to retain, or NULL for a shared request.
 * @param uri The API URL to read.
 * @param cancel The cancellation token, or NULL.
 * @param callback The callback that receives the read result and owns the request.
 */
static void request_api(GtkWidget* fields, const char* uri, GCancellable* cancel, GAsyncReadyCallback callback)
{
    GTask* request = g_task_new(fields, cancel, NULL, NULL);
    g_task_set_task_data(request, g_strdup(uri), g_free);
    GFile* file = g_file_new_for_uri(uri);
    g_file_load_bytes_async(file, cancel, callback, request);
    g_object_unref(file);
}

/**
 * @brief Displays only the selected game's full-game categories.
 *
 * @param file The file containing the category response.
 * @param result The asynchronous read result.
 * @param data The API request, released when finished.
 */
static void categories_loaded(GObject* file, GAsyncResult* result, gpointer data)
{
    GTask* request = data;
    GameFields* state = g_object_get_data(g_task_get_source_object(request), "game-fields");
    json_t* response = response_finish(file, result, request);
    if (response) {
        GPtrArray* names = g_ptr_array_new();
        size_t index;
        json_t* category;
        json_array_foreach(json_object_get(response, "data"), index, category)
        {
            const char* name = json_string_value(json_object_get(category, "name"));
            const char* type = json_string_value(json_object_get(category, "type"));
            if (name && *name && g_strcmp0(type, "per-game") == 0) {
                g_ptr_array_add(names, (gpointer)name);
            }
        }

        g_ptr_array_add(names, NULL);
        ls_editor_game_set_choices(state->category, (const char* const*)names->pdata);
        g_ptr_array_unref(names);
        json_decref(response);
    }

    g_object_unref(request);
}

/**
 * @brief Loads categories only for an exact catalogue game name.
 *
 * @param entry The game name field.
 * @param fields The game and category container.
 */
static void game_changed(GtkEditable* entry, GtkWidget* fields)
{
    GameFields* state = g_object_get_data(G_OBJECT(fields), "game-fields");
    const char* text = gtk_editable_get_text(entry);
    const char* id = *text ? json_string_value(json_object_get(catalogue.games, text)) : NULL;
    if (g_strcmp0(id, state->selected_id) == 0) {
        return;
    }

    if (state->categories_request) {
        g_cancellable_cancel(state->categories_request);
        g_clear_object(&state->categories_request);
    }

    g_free(state->selected_id);
    state->selected_id = g_strdup(id);
    ls_editor_game_set_choices(state->category, FALLBACK_CATEGORIES);
    if (!id || !*id) {
        return;
    }

    state->categories_request = g_cancellable_new();
    char* escaped = g_uri_escape_string(id, NULL, FALSE);
    char* uri = g_strdup_printf(CATEGORIES_URL_FULL, escaped);
    request_api(fields, uri, state->categories_request, categories_loaded);
    g_free(uri);
    g_free(escaped);
}

/**
 * @brief Resolves entered game names when catalogue data arrives.
 *
 * @param names The changed catalogue (unused).
 * @param position The first changed position (unused).
 * @param removed The number of removed names (unused).
 * @param added The number of added names (unused).
 * @param fields The game and category container to refresh.
 */
static void catalogue_changed(GListModel* names, guint position, guint removed, guint added, GtkWidget* fields)
{
    GameFields* state = g_object_get_data(G_OBJECT(fields), "game-fields");
    game_changed(GTK_EDITABLE(state->game), fields);
}

/**
 * @brief Publishes the cached or refreshed catalogue to all open editors.
 */
static void publish_games(void)
{
    GPtrArray* names = g_ptr_array_new();
    const char* name;
    json_t* id;
    json_object_foreach(catalogue.games, name, id)
    {
        g_ptr_array_add(names, (gpointer)name);
    }

    g_ptr_array_add(names, NULL);
    gtk_string_list_splice(catalogue.names, 0, g_list_model_get_n_items(G_LIST_MODEL(catalogue.names)),
        (const char* const*)names->pdata);
    g_ptr_array_unref(names);
}

/**
 * @brief Adds a page of games and follows the API's next-page link.
 *
 * @param file The file containing the games response.
 * @param result The asynchronous read result.
 * @param data The API request, released when finished.
 */
static void games_loaded(GObject* file, GAsyncResult* result, gpointer data)
{
    GTask* request = data;
    json_t* response = response_finish(file, result, request);
    if (!response) {
        if (json_object_size(catalogue.refreshed)) {
            publish_games();
            save_cache();
        }

        g_object_unref(request);
        return;
    }

    GPtrArray* additions = g_ptr_array_new();
    size_t index;
    json_t* game;
    json_array_foreach(json_object_get(response, "data"), index, game)
    {
        json_t* id = json_object_get(game, "id");
        const char* name = json_string_value(json_object_get(json_object_get(game, "names"), "international"));
        if (name && *name && json_is_string(id) && *json_string_value(id)
            && !json_object_get(catalogue.refreshed, name)) {
            const char* previous = json_string_value(json_object_get(catalogue.games, name));
            if (!previous || !*previous) {
                GHashTable* aliases = g_object_get_data(G_OBJECT(catalogue.names), "aliases");
                g_hash_table_replace(aliases, g_strdup(name), search_aliases(name, TRUE));
                g_ptr_array_add(additions, (gpointer)name);
            }

            json_object_set(catalogue.games, name, id);
            json_object_set(catalogue.refreshed, name, id);
        }
    }

    if (additions->len) {
        g_ptr_array_add(additions, NULL);
        gtk_string_list_splice(catalogue.names, g_list_model_get_n_items(G_LIST_MODEL(catalogue.names)), 0,
            (const char* const*)additions->pdata);
    }

    g_ptr_array_unref(additions);
    json_t* next = NULL;
    json_t* link;
    json_array_foreach(json_object_get(json_object_get(response, "pagination"), "links"), index, link)
    {
        if (g_strcmp0(json_string_value(json_object_get(link, "rel")), "next") == 0) {
            next = link;
            break;
        }
    }

    if (next) {
        const char* uri = json_string_value(json_object_get(next, "uri"));
        if (uri && g_str_has_prefix(uri, CATEGORIES_URL)
            && g_strcmp0(uri, g_task_get_task_data(request)) != 0) {
            request_api(NULL, uri, NULL, games_loaded);
        } else {
            publish_games();
            save_cache();
        }
    } else if (json_object_size(catalogue.refreshed)) {
        json_decref(catalogue.games);
        catalogue.games = g_steal_pointer(&catalogue.refreshed);
        publish_games();
        save_cache();
    }

    json_decref(response);
    g_object_unref(request);
}

/**
 * @brief Loads the disk cache and prepares search aliases off the UI thread.
 *
 * @param task The task receiving the cached games and aliases.
 * @param source Unused source object.
 * @param data Unused task data.
 * @param cancel Unused cancellation token.
 */
static void load_cache(GTask* task, gpointer source, gpointer data, GCancellable* cancel)
{
    json_t* games = json_load_file(catalogue.cache_path, 0, NULL);
    if (!json_is_object(games)) {
        json_decref(games);
        games = json_object();
    }

    GHashTable* aliases = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_strfreev);
    const char* name;
    void* next;
    json_t* id;
    json_object_foreach_safe(games, next, name, id)
    {
        if (*name && json_is_string(id) && *json_string_value(id)) {
            g_hash_table_insert(aliases, g_strdup(name), search_aliases(name, TRUE));
        } else {
            json_object_del(games, name);
        }
    }

    g_task_set_task_data(task, aliases, (GDestroyNotify)g_hash_table_unref);
    g_task_return_pointer(task, games, (GDestroyNotify)json_decref);
}

/**
 * @brief Makes cached names available and starts one refresh for this session.
 *
 * @param source Unused source object.
 * @param result The completed cache-loading task.
 * @param data Unused callback data.
 */
static void cache_loaded(GObject* source, GAsyncResult* result, gpointer data)
{
    GTask* task = G_TASK(result);
    catalogue.games = g_task_propagate_pointer(task, NULL);
    GHashTable* aliases = g_task_get_task_data(task);
    g_object_set_data_full(G_OBJECT(catalogue.names), "aliases", g_hash_table_ref(aliases), (GDestroyNotify)g_hash_table_unref);
    publish_games();
    request_api(NULL, GAMES_URL, NULL, games_loaded);
}

/**
 * @brief Creates the game and category fields above the editor tabs.
 *
 * @param game Receives the game field owned by the returned widget.
 * @param category Receives the category field owned by the returned widget.
 * @return GtkWidget* The container holding both fields.
 */
GtkWidget* ls_editor_game_new(GtkEditable** game, GtkEditable** category)
{
    if (!catalogue.names) {
        catalogue.names = new_names(NULL, TRUE);
        catalogue.sorted = sorted_names(catalogue.names);
        catalogue.refreshed = json_object();
        catalogue.cache_path = g_build_filename(g_get_user_cache_dir(), "libresplit", "src-games.json", NULL);
        GTask* task = g_task_new(NULL, NULL, cache_loaded, NULL);
        g_task_run_in_thread(task, load_cache);
        g_object_unref(task);
    }

    GtkWidget* fields = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(fields), 18);
    gtk_grid_set_row_spacing(GTK_GRID(fields), 8);
    gtk_widget_set_hexpand(fields, TRUE);
    gtk_widget_set_valign(fields, GTK_ALIGN_START);
    *game = add_name_field(GTK_GRID(fields), 0, "_Game", NAME_FIELD_GAME);
    *category = add_name_field(GTK_GRID(fields), 1, "_Category", NAME_FIELD_CATEGORY);

    GameFields* state = g_new0(GameFields, 1);
    state->game = *game;
    state->category = *category;
    g_object_set_data_full(G_OBJECT(fields), "game-fields", state, fields_free);
    g_signal_connect(*game, "changed", G_CALLBACK(game_changed), fields);
    g_signal_connect(fields, "unmap", G_CALLBACK(fields_unmapped), state);
    g_signal_connect_object(catalogue.names, "items-changed", G_CALLBACK(catalogue_changed), fields, 0);
    return fields;
}
