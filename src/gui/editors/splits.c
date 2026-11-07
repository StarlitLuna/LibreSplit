#include "splits.h"
#include "draft.h"
#include "src/gui/app_window.h"
#include "src/gui/game.h"
#include "src/gui/widgets/dialog.h"
#include "src/lasr/auto-splitter.h"
#include "widgets/game.h"

/**
 * @brief The editor's widgets and the draft they display.
 */
typedef struct {
    GtkWindow* window;
    LSSplitsDraft* draft;
    GtkListBox* rows;
    GtkWidget* remove;
    GtkWidget* up;
    GtkWidget* down;
    GtkLabel* count;
    GtkDropDown* timing_view;
    GtkSizeGroup* columns[6];
    GtkEntry* script;
    GtkWidget* settings_box;
    GHashTable* settings;
} Editor;

/**
 * @brief The editable controls belonging to one split row.
 */
typedef struct {
    GtkWidget* number;
    GtkWidget* name;
    GtkWidget* pb_split;
    GtkWidget* best_split;
    GtkWidget* best_segment;
} SplitRow;

/**
 * @brief The controls and pending image read for an icon portrait.
 */
typedef struct {
    GtkWidget* portrait;
    GtkWidget* image;
    GtkWidget* popover;
    GtkWidget* choose;
    GtkWidget* url;
    GtkWidget* use;
    GtkWidget* message;
    GCancellable* load;
    char** value;
    gboolean busy;
} IconPicker;

/**
 * @brief Binds a setting control to its value in the draft.
 */
typedef struct {
    const SettingDefinition* definition;
    json_t* parent;
    size_t index;
    GtkWidget* items;
} SettingControl;

static GtkWidget* setting_control_new(const SettingDefinition* definition, json_t* parent, size_t index);
static void setting_array_refresh(SettingControl* control);
static void script_load(GtkEntry* entry, Editor* editor);

static GtkWindow* editor_window = NULL;

/**
 * @brief Checks whether a loaded splits file is ready to edit.
 *
 * @return gboolean TRUE if a splits file is loaded and ready to edit.
 */
gboolean ls_splits_editor_can_open(void)
{
    LSAppWindow* win = ls_get_main_app_window();
    return win && win->game && win->timer && !is_saving()
        && atomic_load(&run_state) == LASR_NOT_STARTED;
}

/**
 * @brief Clears the open editor and restores the auto-splitter state.
 *
 * @param widget The destroyed editor window (unused).
 * @param data Whether the auto-splitter was enabled before editing.
 */
static void editor_destroy(GtkWidget* widget, gpointer data)
{
    editor_window = NULL;
    dialog_count_dec();
    if (ls_get_main_app_window()) {
        atomic_store(&auto_splitter_enabled, GPOINTER_TO_INT(data));
    }
}

/**
 * @brief Opens the splits editor or brings its existing window forward.
 *
 * @param action Unused action.
 * @param parameter Unused action parameter.
 * @param app Unused application pointer.
 */
void ls_splits_editor_open(GSimpleAction* action, GVariant* parameter, gpointer app)
{
    if (editor_window) {
        gtk_window_present(editor_window);
        return;
    }

    if (!ls_splits_editor_can_open()) {
        return;
    }

    LSAppWindow* win = ls_get_main_app_window();
    gboolean resume = atomic_load(&auto_splitter_enabled);
    stop_auto_splitter();
    editor_window = ls_splits_editor_new(GTK_WINDOW(win));
    dialog_count_inc();
    set_main_window_keep_above(FALSE);
    g_signal_connect(editor_window, "destroy", G_CALLBACK(editor_destroy), GINT_TO_POINTER(resume));
    Editor* editor = g_object_get_data(G_OBJECT(editor_window), "editor");
    gtk_editable_set_text(GTK_EDITABLE(editor->script), win->game->auto_splitter_file ? win->game->auto_splitter_file : "");
    script_load(editor->script, editor);
    gtk_window_present(editor_window);
}

/**
 * @brief Closes the editor when Escape is pressed.
 *
 * @param controller The key event controller (unused).
 * @param keyval The pressed key.
 * @param keycode The hardware key code (unused).
 * @param state The modifier keys (unused).
 * @param data The editor window to close.
 * @return gboolean TRUE if Escape was handled.
 */
static gboolean close_on_escape(GtkEventControllerKey* controller, guint keyval,
    guint keycode, GdkModifierType state, gpointer data)
{
    if (keyval != GDK_KEY_Escape) {
        return FALSE;
    }

    gtk_window_close(GTK_WINDOW(data));
    return TRUE;
}

/**
 * @brief Creates a tab's content with space around its controls.
 *
 * @param tabs The notebook receiving the page.
 * @param title The tab label.
 * @return GtkWidget* The content box inside the new tab.
 */
static GtkWidget* page_new(GtkNotebook* tabs, const char* title)
{
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(page, 24);
    gtk_widget_set_margin_bottom(page, 24);
    gtk_widget_set_margin_start(page, 24);
    gtk_widget_set_margin_end(page, 24);
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page);
    gtk_notebook_append_page(tabs, scroll, gtk_label_new(title));
    return page;
}

/**
 * @brief Creates a grid for labelled editor fields.
 *
 * @return GtkWidget* The new form grid.
 */
static GtkWidget* form_new(void)
{
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_valign(grid, GTK_ALIGN_START);
    return grid;
}

/**
 * @brief Adds a label and its control to a form.
 *
 * @param grid The form grid.
 * @param row The zero-based grid row.
 * @param title The field label.
 * @param field The control to add.
 * @return GtkWidget* The field label.
 */
static GtkWidget* add_field(GtkWidget* grid, int row, const char* title, GtkWidget* field)
{
    GtkWidget* label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_set_size_request(label, 150, -1);
    gtk_widget_set_valign(label, GTK_ALIGN_BASELINE_CENTER);
    gtk_widget_set_valign(field, GTK_ALIGN_BASELINE_CENTER);
    gtk_widget_set_hexpand(field, TRUE);
    gtk_accessible_update_relation(GTK_ACCESSIBLE(field), GTK_ACCESSIBLE_RELATION_LABELLED_BY, label, NULL, -1);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), field, 1, row, 1, 1);
    return label;
}

/**
 * @brief Creates an entry with a hint for its expected value.
 *
 * @param hint The placeholder text, or NULL for none.
 * @return GtkWidget* The new text entry.
 */
static GtkWidget* entry_new(const char* hint)
{
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), hint);
    return entry;
}

/**
 * @brief Creates a removal button with a red cross.
 *
 * @param name The accessible name describing what is removed.
 * @param compact Whether to use a small outlined button.
 * @return GtkWidget* The removal button.
 */
static GtkWidget* remove_button_new(const char* name, gboolean compact)
{
    GtkWidget* button = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(button, "remove-button");
    if (compact) {
        gtk_widget_add_css_class(button, "compact");
    }

    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    return button;
}

/**
 * @brief Keeps an entered value in the isolated draft.
 *
 * @param entry The edited text field.
 * @param value The draft string to update.
 */
static void text_changed(GtkEditable* entry, char** value)
{
    ls_splits_draft_set_text(value, gtk_editable_get_text(entry));
}

/**
 * @brief Creates an entry for a draft value.
 *
 * @param value The draft string to display and update.
 * @param hint The placeholder text, or NULL for none.
 * @return GtkWidget* The entry bound to the draft value.
 */
static GtkWidget* draft_entry_new(char** value, const char* hint)
{
    GtkWidget* entry = entry_new(hint);
    gtk_editable_set_text(GTK_EDITABLE(entry), *value ? *value : "");
    g_signal_connect(entry, "changed", G_CALLBACK(text_changed), value);
    return entry;
}

/**
 * @brief Shows an inline message, or hides it when it is empty.
 *
 * @param label The message label.
 * @param message The text to show, or an empty string to hide the label.
 */
static void show_message(GtkWidget* label, const char* message)
{
    gtk_widget_remove_css_class(label, "error-message");
    gtk_label_set_text(GTK_LABEL(label), message);
    gtk_widget_set_visible(label, *message != '\0');
}

/**
 * @brief Shows an inline error in the theme's error color.
 *
 * @param label The error label.
 * @param message The error text to show.
 */
static void show_error(GtkWidget* label, const char* message)
{
    show_message(label, message);
    gtk_widget_add_css_class(label, "error-message");
}

/**
 * @brief Releases an icon picker's pending read.
 *
 * @param data The icon picker to release.
 */
static void icon_picker_free(gpointer data)
{
    IconPicker* picker = data;
    g_object_unref(picker->load);
    g_free(picker);
}

/**
 * @brief Displays an image and keeps an embedded copy in the draft.
 *
 * @param picker The icon picker to update.
 * @param bytes The image data to import.
 */
static void icon_use_bytes(IconPicker* picker, GBytes* bytes)
{
    GError* error = NULL;
    GdkTexture* texture = gdk_texture_new_from_bytes(bytes, &error);
    if (!texture) {
        show_error(picker->message, error->message);
        g_error_free(error);
        return;
    }

    GBytes* png = gdk_texture_save_to_png_bytes(texture);
    gsize size;
    const guchar* contents = g_bytes_get_data(png, &size);
    char* encoded = g_base64_encode(contents, size);
    char* uri = g_strconcat("data:image/png;base64,", encoded, NULL);
    ls_splits_draft_set_text(picker->value, uri);
    gtk_image_set_from_paintable(GTK_IMAGE(picker->image), GDK_PAINTABLE(texture));
    show_message(picker->message, "");
    gtk_popover_popdown(GTK_POPOVER(picker->popover));
    g_free(uri);
    g_free(encoded);
    g_bytes_unref(png);
    g_object_unref(texture);
}

/**
 * @brief Finishes reading an icon without replacing it after cancellation.
 *
 * @param file The file whose read completed.
 * @param result The asynchronous read result.
 * @param data The retained portrait widget, released after the read.
 */
static void icon_loaded(GObject* file, GAsyncResult* result, gpointer data)
{
    GtkWidget* portrait = data;
    IconPicker* picker = g_object_get_data(G_OBJECT(portrait), "icon-picker");
    GError* error = NULL;
    GBytes* bytes = g_file_load_bytes_finish(G_FILE(file), result, NULL, &error);
    picker->busy = FALSE;
    if (gtk_widget_get_root(portrait)) {
        if (!g_cancellable_is_cancelled(picker->load)) {
            if (bytes) {
                icon_use_bytes(picker, bytes);
            } else {
                show_error(picker->message, error->message);
            }
        }

        gtk_editable_set_editable(GTK_EDITABLE(picker->url), TRUE);
        gtk_widget_set_sensitive(picker->choose, TRUE);
        gtk_widget_set_sensitive(picker->use, *gtk_editable_get_text(GTK_EDITABLE(picker->url)) != '\0');
    }

    g_clear_pointer(&bytes, g_bytes_unref);
    g_clear_error(&error);
    g_object_unref(portrait);
}

/**
 * @brief Reads a selected icon while keeping the editor responsive.
 *
 * @param picker The icon picker receiving the image.
 * @param file The image file to read.
 */
static void icon_load(IconPicker* picker, GFile* file)
{
    if (picker->busy) {
        return;
    }

    picker->busy = TRUE;
    g_cancellable_reset(picker->load);
    gtk_editable_set_editable(GTK_EDITABLE(picker->url), FALSE);
    gtk_widget_set_sensitive(picker->choose, FALSE);
    gtk_widget_set_sensitive(picker->use, FALSE);
    show_message(picker->message, "Loading image...");
    g_file_load_bytes_async(file, picker->load, icon_loaded, g_object_ref(picker->portrait));
}

/**
 * @brief Imports the image entered in the URL field.
 *
 * @param button The widget that requested the import (unused).
 * @param picker The icon picker containing the URL.
 */
static void icon_use_url(GtkWidget* button, IconPicker* picker)
{
    const char* uri = gtk_editable_get_text(GTK_EDITABLE(picker->url));
    if (!*uri || picker->busy) {
        return;
    }

    if (g_str_has_prefix(uri, "data:image/") && strstr(uri, ";base64,")) {
        gsize size;
        guchar* contents = g_base64_decode(strstr(uri, ";base64,") + 8, &size);
        GBytes* bytes = g_bytes_new_take(contents, size);
        icon_use_bytes(picker, bytes);
        g_bytes_unref(bytes);
        return;
    }

    if (!g_uri_is_valid(uri, G_URI_FLAGS_NONE, NULL)) {
        show_error(picker->message, "Enter a complete image URL");
        return;
    }

    GFile* file = g_file_new_for_uri(uri);
    icon_load(picker, file);
    g_object_unref(file);
}

/**
 * @brief Enables importing when the URL field has text.
 *
 * @param entry The edited URL field.
 * @param picker The icon picker to update.
 */
static void icon_url_changed(GtkEditable* entry, IconPicker* picker)
{
    gtk_widget_set_sensitive(picker->use, !picker->busy && *gtk_editable_get_text(entry) != '\0');
    show_message(picker->message, "");
}

/**
 * @brief Cancels an unfinished URL import when its popup closes.
 *
 * @param popover The closed popup (unused).
 * @param picker The icon picker whose read should be cancelled.
 */
static void icon_closed(GtkPopover* popover, IconPicker* picker)
{
    g_cancellable_cancel(picker->load);
    show_message(picker->message, "");
}

/**
 * @brief Removes the current icon with a right click.
 *
 * @param gesture The right-click gesture.
 * @param presses The click count (unused).
 * @param x The click x-coordinate (unused).
 * @param y The click y-coordinate (unused).
 * @param picker The icon picker to clear.
 */
static void icon_remove(GtkGestureClick* gesture, int presses, double x, double y, IconPicker* picker)
{
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    g_cancellable_cancel(picker->load);
    gtk_popover_popdown(GTK_POPOVER(picker->popover));
    ls_splits_draft_set_text(picker->value, "");
    gtk_image_set_from_icon_name(GTK_IMAGE(picker->image), "image-x-generic-symbolic");
}

/**
 * @brief Imports the icon chosen in the native file chooser.
 *
 * @param dialog The image file chooser.
 * @param result The asynchronous selection result.
 * @param data The retained portrait widget, released after selection.
 */
static void icon_file_chosen(GObject* dialog, GAsyncResult* result, gpointer data)
{
    GtkWidget* portrait = data;
    IconPicker* picker = g_object_get_data(G_OBJECT(portrait), "icon-picker");
    GError* error = NULL;
    GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(dialog), result, &error);
    if (gtk_widget_get_root(portrait)) {
        if (file) {
            gtk_popover_popup(GTK_POPOVER(picker->popover));
            icon_load(picker, file);
        } else if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)
            && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            gtk_popover_popup(GTK_POPOVER(picker->popover));
            show_error(picker->message, error->message);
        }
    }

    g_clear_object(&file);
    g_clear_error(&error);
    g_object_unref(portrait);
}

/**
 * @brief Opens the native image chooser for a portrait.
 *
 * @param button The Choose File button (unused).
 * @param picker The icon picker receiving the selection.
 */
static void icon_choose_file(GtkButton* button, IconPicker* picker)
{
    gtk_popover_popdown(GTK_POPOVER(picker->popover));
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose Icon");
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images");
    gtk_file_filter_add_mime_type(filter, "image/*");
    gtk_file_dialog_set_default_filter(dialog, filter);
    gtk_file_dialog_open(dialog, GTK_WINDOW(gtk_widget_get_root(picker->portrait)), NULL, icon_file_chosen, g_object_ref(picker->portrait));
    g_object_unref(filter);
    g_object_unref(dialog);
}

/**
 * @brief Creates the portrait and its icon picker popup.
 *
 * @param value The draft icon string to update.
 * @param title The portrait tooltip.
 * @param size The displayed icon size in pixels.
 * @return GtkWidget* The portrait button with its icon picker.
 */
static GtkWidget* icon_new(char** value, const char* title, int size)
{
    IconPicker* state = g_new0(IconPicker, 1);
    state->value = value;
    state->load = g_cancellable_new();
    GtkWidget* portrait = gtk_menu_button_new();
    state->portrait = portrait;
    g_object_set_data_full(G_OBJECT(portrait), "icon-picker", state, icon_picker_free);
    gtk_widget_set_size_request(portrait, size == 64 ? 80 : 32, size == 64 ? 80 : 32);
    gtk_widget_set_valign(portrait, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(portrait, title);
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(portrait), size > 24);
    GtkWidget* image = gtk_image_new_from_icon_name("image-x-generic-symbolic");
    state->image = image;
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(portrait), image);

    GtkWidget* picker = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(content), picker);
    GtkWidget* url = entry_new("Image URL");
    state->url = url;
    gtk_editable_set_width_chars(GTK_EDITABLE(url), 32);
    gtk_accessible_update_property(GTK_ACCESSIBLE(url), GTK_ACCESSIBLE_PROPERTY_LABEL, "Image URL", -1);
    gtk_box_append(GTK_BOX(picker), url);

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(actions), TRUE);
    GtkWidget* choose = gtk_button_new_with_label("Choose File");
    state->choose = choose;
    g_signal_connect(choose, "clicked", G_CALLBACK(icon_choose_file), state);
    gtk_box_append(GTK_BOX(actions), choose);
    GtkWidget* use = gtk_button_new_with_label("Use Icon");
    state->use = use;
    gtk_widget_set_sensitive(use, FALSE);
    gtk_box_append(GTK_BOX(actions), use);
    gtk_box_append(GTK_BOX(picker), actions);
    state->message = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(state->message), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(state->message), 40);
    gtk_label_set_xalign(GTK_LABEL(state->message), 0.0f);
    gtk_widget_set_visible(state->message, FALSE);
    gtk_box_append(GTK_BOX(content), state->message);

    GtkWidget* popover = gtk_popover_new();
    state->popover = popover;
    gtk_popover_set_child(GTK_POPOVER(popover), content);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(portrait), popover);
    g_signal_connect(url, "changed", G_CALLBACK(icon_url_changed), state);
    g_signal_connect(url, "activate", G_CALLBACK(icon_use_url), state);
    g_signal_connect(use, "clicked", G_CALLBACK(icon_use_url), state);
    g_signal_connect(popover, "closed", G_CALLBACK(icon_closed), state);
    g_signal_connect_swapped(portrait, "unmap", G_CALLBACK(g_cancellable_cancel), state->load);
    GtkGesture* right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(right_click), GTK_PHASE_CAPTURE);
    g_signal_connect(right_click, "pressed", G_CALLBACK(icon_remove), state);
    gtk_widget_add_controller(portrait, GTK_EVENT_CONTROLLER(right_click));
    return portrait;
}

/**
 * @brief Clears a chosen resource from its entry.
 *
 * @param button The Remove button (unused).
 * @param entry The resource entry to clear.
 */
static void resource_remove(GtkButton* button, GtkEditable* entry)
{
    gtk_editable_set_text(entry, "");
}

/**
 * @brief Clears a resource warning when its path changes.
 *
 * @param entry The edited resource entry (unused).
 * @param message The warning label to clear.
 */
static void resource_changed(GtkEditable* entry, GtkWidget* message)
{
    show_message(message, "");
}

/**
 * @brief Places a chosen path in the editor without loading it into the app.
 *
 * @param dialog The resource file chooser.
 * @param result The asynchronous selection result.
 * @param data The retained resource entry, released after selection.
 */
static void resource_chosen(GObject* dialog, GAsyncResult* result, gpointer data)
{
    GtkWidget* entry = data;
    GError* error = NULL;
    GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(dialog), result, &error);
    if (gtk_widget_get_root(entry)) {
        if (file && g_file_is_native(file)) {
            char* path = g_file_get_path(file);
            gtk_editable_set_text(GTK_EDITABLE(entry), path);
            g_signal_emit_by_name(entry, "activate");
            g_free(path);
        } else if (file) {
            show_error(g_object_get_data(G_OBJECT(entry), "resource-message"), "Choose a local file.");
        } else if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)
            && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            show_error(g_object_get_data(G_OBJECT(entry), "resource-message"), error->message);
        }
    }

    g_clear_object(&file);
    g_clear_error(&error);
    g_object_unref(entry);
}

/**
 * @brief Opens the native chooser for a resource path.
 *
 * @param button The Choose button (unused).
 * @param entry The entry receiving the chosen path.
 */
static void resource_choose(GtkButton* button, GtkWidget* entry)
{
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose a File");
    GtkFileFilter* filter = g_object_get_data(G_OBJECT(entry), "resource-filter");
    gtk_file_dialog_set_default_filter(dialog, filter);
    const char* text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (*text) {
        GFile* initial = g_file_new_for_path(text);
        gtk_file_dialog_set_initial_file(dialog, initial);
        g_object_unref(initial);
    }

    gtk_file_dialog_open(dialog, GTK_WINDOW(gtk_widget_get_root(entry)), NULL, resource_chosen, g_object_ref(entry));
    g_object_unref(dialog);
}

/**
 * @brief Creates a resource entry and its filtered file chooser.
 *
 * @param value The draft resource path to display and update.
 * @param filter_name The file filter label.
 * @param suffix The allowed file extension without the dot.
 * @param input Receives the entry widget, or NULL if it is not needed.
 * @return GtkWidget* The resource entry and its controls.
 */
static GtkWidget* resource_new(char** value, const char* filter_name, const char* suffix, GtkEntry** input)
{
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_set_baseline_child(GTK_BOX(content), 0);
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* entry = draft_entry_new(value, "File path");
    gtk_widget_set_valign(entry, GTK_ALIGN_BASELINE_CENTER);
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, filter_name);
    gtk_file_filter_add_suffix(filter, suffix);
    g_object_set_data_full(G_OBJECT(entry), "resource-filter", filter, g_object_unref);
    if (input) {
        *input = GTK_ENTRY(entry);
    }

    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_append(GTK_BOX(row), entry);
    GtkWidget* choose = gtk_button_new_with_label("Choose…");
    g_signal_connect(choose, "clicked", G_CALLBACK(resource_choose), entry);
    gtk_box_append(GTK_BOX(row), choose);
    GtkWidget* remove = remove_button_new("Remove file", TRUE);
    g_object_bind_property(gtk_entry_get_buffer(GTK_ENTRY(entry)), "length", remove, "sensitive", G_BINDING_SYNC_CREATE);
    g_signal_connect(remove, "clicked", G_CALLBACK(resource_remove), entry);
    gtk_box_append(GTK_BOX(row), remove);
    gtk_box_append(GTK_BOX(content), row);
    GtkWidget* message = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(message), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(message), TRUE);
    gtk_widget_set_visible(message, FALSE);
    gtk_box_append(GTK_BOX(content), message);
    g_object_set_data(G_OBJECT(entry), "resource-message", message);
    g_signal_connect(entry, "changed", G_CALLBACK(resource_changed), message);
    return content;
}

/**
 * @brief Updates the actions available for the selected split.
 *
 * @param editor The editor whose row buttons should be updated.
 */
static void update_row_actions(Editor* editor)
{
    GtkListBoxRow* selected = gtk_list_box_get_selected_row(editor->rows);
    int index = selected ? gtk_list_box_row_get_index(selected) : -1;
    gtk_widget_set_sensitive(editor->remove, index >= 0);
    gtk_widget_set_sensitive(editor->up, index > 0);
    gtk_widget_set_sensitive(editor->down, index >= 0 && (guint)index + 1 < editor->draft->rows->len);
}

/**
 * @brief Updates row numbers, the split count, and row actions.
 *
 * @param editor The editor whose split rows changed.
 */
static void rows_changed(Editor* editor)
{
    guint count = editor->draft->rows->len;
    guint position = 1;
    for (GtkWidget* row = gtk_widget_get_first_child(GTK_WIDGET(editor->rows)); row; row = gtk_widget_get_next_sibling(row)) {
        SplitRow* fields = g_object_get_data(G_OBJECT(row), "split-row");
        char* number = g_strdup_printf("%u", position++);
        gtk_label_set_text(GTK_LABEL(fields->number), number);
        g_free(number);
    }

    char* text = g_strdup_printf("%u %s", count, count == 1 ? "split" : "splits");
    gtk_label_set_text(editor->count, text);
    g_free(text);
    update_row_actions(editor);
}

/**
 * @brief Refreshes row actions when the selection changes.
 *
 * @param list The split list.
 * @param row The selected row (unused).
 * @param editor The splits editor.
 */
static void row_selected(GtkListBox* list, GtkListBoxRow* row, Editor* editor)
{
    if (gtk_widget_get_root(GTK_WIDGET(list))) {
        update_row_actions(editor);
    }
}

/**
 * @brief Selects a split when one of its controls receives focus.
 *
 * @param controller The focus controller attached to the row.
 * @param rows The split list containing the row.
 */
static void row_focused(GtkEventControllerFocus* controller, GtkListBox* rows)
{
    GtkWidget* widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    GtkWidget* row = gtk_widget_get_ancestor(widget, GTK_TYPE_LIST_BOX_ROW);
    gtk_list_box_select_row(rows, GTK_LIST_BOX_ROW(row));
}

/**
 * @brief Aligns a split control with its column heading.
 *
 * @param editor The editor holding the column sizes.
 * @param row The box receiving the cell.
 * @param cell The split control to add.
 * @param column The zero-based column index.
 */
static void row_cell(Editor* editor, GtkWidget* row, GtkWidget* cell, guint column)
{
    gtk_size_group_add_widget(editor->columns[column], cell);
    gtk_widget_set_hexpand(cell, column == 2);
    gtk_box_append(GTK_BOX(row), cell);
}

/**
 * @brief Allows only digits, colons, and periods in a time field.
 *
 * @param entry The editable text field.
 * @param text The proposed text.
 * @param length The byte length, or -1 for a NUL-terminated string.
 * @param position The insertion position (unused).
 * @param data Unused callback data.
 */
static void time_insert_text(GtkEditable* entry, const char* text, int length, int* position, gpointer data)
{
    gsize bytes = length < 0 ? strlen(text) : (gsize)length;
    for (gsize i = 0; i < bytes; ++i) {
        if (!g_ascii_isdigit(text[i]) && text[i] != ':' && text[i] != '.') {
            g_signal_stop_emission_by_name(entry, "insert-text");
            return;
        }
    }
}

/**
 * @brief Adds one editable time to a cell.
 *
 * @param stack The stack receiving the time entry.
 * @param value The draft timing text to update.
 * @param record The timing record label.
 * @param method The timing method name: real or game.
 */
static void time_cell_add(GtkStack* stack, char** value, const char* record, const char* method)
{
    GtkWidget* entry = draft_entry_new(value, "h:mm:ss.ffffff");
    g_signal_connect(gtk_editable_get_delegate(GTK_EDITABLE(entry)), "insert-text", G_CALLBACK(time_insert_text), NULL);
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), 14);
    gtk_entry_set_alignment(GTK_ENTRY(entry), 1.0f);
    gtk_widget_add_css_class(entry, "split-cell");
    char* title = g_strdup_printf("%s · %s time", record, method);
    gtk_widget_set_tooltip_text(entry, title);
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
    g_free(title);
    gtk_stack_add_named(stack, entry, method);
}

/**
 * @brief Creates a time cell with separate real and game entries.
 *
 * @param time The draft real and game time values.
 * @param record The timing record label.
 * @param visible_method The method to show initially: real or game.
 * @return GtkWidget* The stack containing both time entries.
 */
static GtkWidget* time_cell_new(LSDraftTime* time, const char* record, const char* visible_method)
{
    GtkWidget* stack = gtk_stack_new();
    time_cell_add(GTK_STACK(stack), &time->real_time, record, "real");
    time_cell_add(GTK_STACK(stack), &time->game_time, record, "game");
    gtk_stack_set_visible_child_name(GTK_STACK(stack), visible_method);
    return stack;
}

/**
 * @brief Inserts an editable split after the current selection.
 *
 * @param button The Add button (unused).
 * @param editor The editor receiving the split.
 */
static void add_row(GtkButton* button, Editor* editor)
{
    GtkListBoxRow* selected = gtk_list_box_get_selected_row(editor->rows);
    guint position = selected ? (guint)gtk_list_box_row_get_index(selected) + 1 : editor->draft->rows->len;
    LSSplitDraftRow* data = ls_splits_draft_add_row(editor->draft, position);
    SplitRow* fields = g_new0(SplitRow, 1);
    GtkWidget* row = gtk_list_box_row_new();
    g_object_set_data_full(G_OBJECT(row), "split-row", fields, g_free);
    GtkWidget* cells = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), cells);
    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(row_focused), editor->rows);
    gtk_widget_add_controller(cells, focus);
    fields->number = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(fields->number), 0.0f);
    row_cell(editor, cells, fields->number, 0);
    row_cell(editor, cells, icon_new(&data->icon, "Split icon", 24), 1);
    fields->name = draft_entry_new(&data->name, "Split name");
    gtk_editable_set_width_chars(GTK_EDITABLE(fields->name), 16);
    gtk_widget_add_css_class(fields->name, "split-cell");
    row_cell(editor, cells, fields->name, 2);
    const char* method = gtk_drop_down_get_selected(editor->timing_view) ? "game" : "real";
    fields->pb_split = time_cell_new(&data->pb_split, "PB split", method);
    fields->best_split = time_cell_new(&data->best_split, "Best split", method);
    fields->best_segment = time_cell_new(&data->best_segment, "Best segment", method);
    row_cell(editor, cells, fields->pb_split, 3);
    row_cell(editor, cells, fields->best_split, 4);
    row_cell(editor, cells, fields->best_segment, 5);
    gtk_list_box_insert(editor->rows, row, position);
    gtk_list_box_select_row(editor->rows, GTK_LIST_BOX_ROW(row));
    gtk_widget_grab_focus(fields->name);
    rows_changed(editor);
}

/**
 * @brief Removes the selected split and selects its nearest neighbour.
 *
 * @param button The Remove button (unused).
 * @param editor The editor containing the selected split.
 */
static void remove_row(GtkButton* button, Editor* editor)
{
    GtkListBoxRow* row = gtk_list_box_get_selected_row(editor->rows);
    if (!row) {
        return;
    }

    guint position = gtk_list_box_row_get_index(row);
    gtk_list_box_remove(editor->rows, GTK_WIDGET(row));
    ls_splits_draft_remove_row(editor->draft, position);
    if (editor->draft->rows->len) {
        gtk_list_box_select_row(editor->rows, gtk_list_box_get_row_at_index(editor->rows, MIN(position, editor->draft->rows->len - 1)));
    }

    rows_changed(editor);
}

/**
 * @brief Moves the selected split one place without rebuilding its controls.
 *
 * @param button The Up or Down button that determines the direction.
 * @param editor The editor containing the selected split.
 */
static void move_row(GtkButton* button, Editor* editor)
{
    GtkListBoxRow* row = gtk_list_box_get_selected_row(editor->rows);
    if (!row) {
        return;
    }

    int from = gtk_list_box_row_get_index(row);
    int to = from + (GTK_WIDGET(button) == editor->up ? -1 : 1);
    if (to < 0 || (guint)to >= editor->draft->rows->len) {
        return;
    }

    ls_splits_draft_move_row(editor->draft, from, to);
    g_object_ref(row);
    gtk_list_box_unselect_row(editor->rows, row);
    gtk_list_box_remove(editor->rows, GTK_WIDGET(row));
    gtk_list_box_insert(editor->rows, GTK_WIDGET(row), to);
    g_object_unref(row);
    gtk_list_box_select_row(editor->rows, row);
    rows_changed(editor);
}

/**
 * @brief Switches visible timing fields without changing their values.
 *
 * @param dropdown The selected timing view.
 * @param spec The changed property (unused).
 * @param editor The editor whose time entries should be switched.
 */
static void timing_view_changed(GtkDropDown* dropdown, GParamSpec* spec, Editor* editor)
{
    const char* method = gtk_drop_down_get_selected(dropdown) ? "game" : "real";
    for (GtkWidget* row = gtk_widget_get_first_child(GTK_WIDGET(editor->rows)); row; row = gtk_widget_get_next_sibling(row)) {
        SplitRow* fields = g_object_get_data(G_OBJECT(row), "split-row");
        gtk_stack_set_visible_child_name(GTK_STACK(fields->pb_split), method);
        gtk_stack_set_visible_child_name(GTK_STACK(fields->best_split), method);
        gtk_stack_set_visible_child_name(GTK_STACK(fields->best_segment), method);
    }
}

/**
 * @brief Creates the split table, row controls, and timing selector.
 *
 * @param tabs The notebook receiving the Splits tab.
 * @param editor The editor supplying the draft and row controls.
 */
static void splits_page_new(GtkNotebook* tabs, Editor* editor)
{
    GtkWidget* page = page_new(tabs, "Splits");
    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(add, "add-button");
    gtk_accessible_update_property(GTK_ACCESSIBLE(add), GTK_ACCESSIBLE_PROPERTY_LABEL, "Add split", -1);
    editor->remove = remove_button_new("Remove split", FALSE);
    editor->up = gtk_button_new_from_icon_name("go-up-symbolic");
    editor->down = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(editor->up, "Move split up");
    gtk_widget_set_tooltip_text(editor->down, "Move split down");
    gtk_box_append(GTK_BOX(toolbar), add);
    gtk_box_append(GTK_BOX(toolbar), editor->remove);
    gtk_box_append(GTK_BOX(toolbar), editor->up);
    gtk_box_append(GTK_BOX(toolbar), editor->down);
    g_signal_connect(add, "clicked", G_CALLBACK(add_row), editor);
    g_signal_connect(editor->remove, "clicked", G_CALLBACK(remove_row), editor);
    g_signal_connect(editor->up, "clicked", G_CALLBACK(move_row), editor);
    g_signal_connect(editor->down, "clicked", G_CALLBACK(move_row), editor);
    GtkWidget* label = gtk_label_new_with_mnemonic("Edit _times");
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(toolbar), label);
    const char* methods[] = { "Real time", "Game time", NULL };
    GtkWidget* method = gtk_drop_down_new_from_strings(methods);
    editor->timing_view = GTK_DROP_DOWN(method);
    g_signal_connect(method, "notify::selected", G_CALLBACK(timing_view_changed), editor);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), method);
    gtk_box_append(GTK_BOX(toolbar), method);
    gtk_box_append(GTK_BOX(page), toolbar);

    GtkWidget* table = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(table, "split-table");
    GtkWidget* headings = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(headings, "split-heading");
    const char* titles[] = { "#", "Icon", "Split name", "PB split", "Best split", "Best segment" };
    for (size_t i = 0; i < G_N_ELEMENTS(titles); ++i) {
        GtkWidget* heading = gtk_label_new(titles[i]);
        gtk_label_set_xalign(GTK_LABEL(heading), i >= 3 ? 1.0f : 0.0f);
        if (i < 2) {
            gtk_widget_set_size_request(heading, 32, -1);
        }

        row_cell(editor, headings, heading, i);
    }

    gtk_box_append(GTK_BOX(table), headings);
    editor->rows = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(editor->rows, GTK_SELECTION_SINGLE);
    gtk_box_append(GTK_BOX(table), GTK_WIDGET(editor->rows));
    gtk_box_append(GTK_BOX(page), table);

    GtkWidget* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    editor->count = GTK_LABEL(gtk_label_new(NULL));
    gtk_box_append(GTK_BOX(footer), GTK_WIDGET(editor->count));
    GtkWidget* format = gtk_label_new("Times: h:mm:ss.ffffff");
    gtk_widget_set_hexpand(format, TRUE);
    gtk_widget_set_halign(format, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(footer), format);
    gtk_box_append(GTK_BOX(page), footer);
    g_signal_connect(editor->rows, "row-selected", G_CALLBACK(row_selected), editor);
    rows_changed(editor);
}

/**
 * @brief Keeps the run's comparison method separate from the table's timing view.
 *
 * @param dropdown The comparison method selector.
 * @param spec The changed property (unused).
 * @param draft The draft to update.
 */
static void comparison_changed(GtkDropDown* dropdown, GParamSpec* spec, LSSplitsDraft* draft)
{
    draft->comparison_method = gtk_drop_down_get_selected(dropdown);
}

/**
 * @brief Creates the run counters, timing method, and record fields.
 *
 * @param tabs The notebook receiving the Run tab.
 * @param editor The editor supplying the draft.
 */
static void run_page_new(GtkNotebook* tabs, Editor* editor)
{
    GtkWidget* page = page_new(tabs, "Run");
    GtkWidget* form = form_new();
    const char* methods[] = { "Real time", "Game time", NULL };
    LSSplitsDraft* draft = editor->draft;
    GtkWidget* method = gtk_drop_down_new_from_strings(methods);
    g_signal_connect(method, "notify::selected", G_CALLBACK(comparison_changed), draft);
    add_field(form, 0, "Compare using", method);
    add_field(form, 1, "Start delay", draft_entry_new(&draft->start_delay, "0:00.000000"));
    add_field(form, 2, "Attempts", draft_entry_new(&draft->attempts, "0"));
    add_field(form, 3, "Completed runs", draft_entry_new(&draft->completed_runs, "0"));
    add_field(form, 4, "World record · real", draft_entry_new(&draft->world_record.real_time, "h:mm:ss.ffffff"));
    add_field(form, 5, "World record · game", draft_entry_new(&draft->world_record.game_time, "h:mm:ss.ffffff"));
    gtk_box_append(GTK_BOX(page), form);
}

/**
 * @brief Releases a setting control's draft reference.
 *
 * @param data The setting control to release.
 */
static void setting_control_free(gpointer data)
{
    SettingControl* control = data;
    json_decref(control->parent);
    g_free(control);
}

/**
 * @brief Keeps setting text as entered until the draft is validated.
 *
 * @param entry The edited setting field.
 * @param control The setting and draft value to update.
 */
static void setting_text_changed(GtkEditable* entry, SettingControl* control)
{
    ls_splits_draft_setting_set(control->parent, control->definition->key, control->index, json_string(gtk_editable_get_text(entry)));
}

/**
 * @brief Keeps a setting's checked state in the draft.
 *
 * @param button The setting checkbox.
 * @param control The setting and draft value to update.
 */
static void setting_toggled(GtkCheckButton* button, SettingControl* control)
{
    ls_splits_draft_setting_set(control->parent, control->definition->key, control->index, json_boolean(gtk_check_button_get_active(button)));
}

/**
 * @brief Keeps the selected option's value in the draft.
 *
 * @param dropdown The setting option selector.
 * @param spec The changed property (unused).
 * @param control The setting and draft value to update.
 */
static void setting_option_changed(GtkDropDown* dropdown, GParamSpec* spec, SettingControl* control)
{
    guint selected = gtk_drop_down_get_selected(dropdown);
    if (selected < control->definition->options_count) {
        ls_splits_draft_setting_set(control->parent, control->definition->key, control->index, json_integer(control->definition->options[selected].value));
    }
}

/**
 * @brief Removes one array item and refreshes the remaining controls.
 *
 * @param button The Remove button holding the item index.
 * @param control The array setting to update.
 */
static void setting_item_remove(GtkButton* button, SettingControl* control)
{
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "item-index"));
    json_array_remove(ls_splits_draft_setting_get(control->parent, control->definition->key, control->index), index);
    setting_array_refresh(control);
}

/**
 * @brief Adds an array item using its declared default.
 *
 * @param button The Add Item button (unused).
 * @param control The array setting receiving the item.
 */
static void setting_item_add(GtkButton* button, SettingControl* control)
{
    json_array_append_new(ls_splits_draft_setting_get(control->parent, control->definition->key, control->index),
        ls_splits_draft_setting_default(control->definition->items));
    setting_array_refresh(control);
}

/**
 * @brief Groups settings in a labelled frame, optionally with a collapse control.
 *
 * @param title The group label.
 * @param content The settings to enclose.
 * @param collapsible Whether the group can be collapsed.
 * @param description The heading tooltip, or NULL for none.
 * @return GtkWidget* The settings frame.
 */
static GtkWidget* settings_group_new(const char* title, GtkWidget* content, gboolean collapsible, const char* description)
{
    GtkWidget* frame = gtk_frame_new(NULL);
    gtk_widget_add_css_class(frame, "settings-group");
    if (collapsible) {
        GtkWidget* expander = gtk_expander_new(title);
        gtk_widget_set_tooltip_text(gtk_expander_get_label_widget(GTK_EXPANDER(expander)), description);
        gtk_expander_set_expanded(GTK_EXPANDER(expander), TRUE);
        gtk_widget_set_margin_top(content, 12);
        gtk_expander_set_child(GTK_EXPANDER(expander), content);
        content = expander;
    } else {
        gtk_widget_add_css_class(frame, "titled");
        GtkWidget* group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget* heading = gtk_grid_new();
        gtk_grid_set_row_homogeneous(GTK_GRID(heading), TRUE);
        GtkWidget* label = gtk_label_new(title);
        gtk_widget_set_margin_start(label, 6);
        gtk_widget_set_margin_end(label, 6);
        gtk_grid_attach(GTK_GRID(heading), label, 1, 0, 1, 2);
        GtkWidget* start = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(start, 12, -1);
        gtk_widget_add_css_class(start, "group-start");
        gtk_grid_attach(GTK_GRID(heading), start, 0, 1, 1, 1);
        GtkWidget* end = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(end, TRUE);
        gtk_widget_add_css_class(end, "group-end");
        gtk_grid_attach(GTK_GRID(heading), end, 2, 1, 1, 1);
        gtk_widget_add_css_class(content, "group-body");
        gtk_box_append(GTK_BOX(group), heading);
        gtk_box_append(GTK_BOX(group), content);
        gtk_accessible_update_relation(GTK_ACCESSIBLE(frame), GTK_ACCESSIBLE_RELATION_LABELLED_BY, label, NULL, -1);
        content = group;
    }

    gtk_frame_set_child(GTK_FRAME(frame), content);
    return frame;
}

/**
 * @brief Adds a setting row or a nested group to a form.
 *
 * @param form The settings grid.
 * @param row The zero-based setting row.
 * @param definition The setting definition.
 * @param parent The draft object or array holding the setting value.
 * @param index The array index. Ignored for objects.
 */
static void add_setting(GtkWidget* form, int row, const SettingDefinition* definition, json_t* parent, size_t index)
{
    GtkWidget* field = setting_control_new(definition, parent, index);
    gboolean nested = definition->type == SETTING_OBJECT || definition->type == SETTING_ARRAY;
    char* title = json_is_array(parent) ? g_strdup_printf("Item %zu", index + 1) : g_strdup(definition->name ? definition->name : definition->key);
    if (nested) {
        GtkWidget* group = settings_group_new(title, field, TRUE, definition->desc);
        gtk_grid_attach(GTK_GRID(form), group, 0, row, 2, 1);
    } else {
        GtkWidget* label = add_field(form, row, title, field);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 22);
        gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
        gtk_widget_set_valign(field, GTK_ALIGN_START);
    }

    g_free(title);
}

/**
 * @brief Displays the array's current items with individual remove buttons.
 *
 * @param control The array setting whose item controls should be rebuilt.
 */
static void setting_array_refresh(SettingControl* control)
{
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(control->items))) {
        gtk_box_remove(GTK_BOX(control->items), child);
    }

    json_t* array = ls_splits_draft_setting_get(control->parent, control->definition->key, control->index);
    for (size_t i = 0; i < json_array_size(array); ++i) {
        GtkWidget* row = form_new();
        const SettingDefinition* item = control->definition->items;
        add_setting(row, 0, item, array, i);
        GtkWidget* remove = remove_button_new("Remove item", TRUE);
        gtk_widget_set_valign(remove, GTK_ALIGN_START);
        g_object_set_data(G_OBJECT(remove), "item-index", GUINT_TO_POINTER(i));
        g_signal_connect(remove, "clicked", G_CALLBACK(setting_item_remove), control);
        gtk_grid_attach(GTK_GRID(row), remove, 2, 0, 1, 1);
        gtk_box_append(GTK_BOX(control->items), row);
    }
}

/**
 * @brief Creates a control for an existing LASR setting type.
 *
 * @param definition The setting definition.
 * @param parent The draft JSON object or array holding the value.
 * @param index The array index. Ignored for objects.
 * @return GtkWidget* The setting control.
 */
static GtkWidget* setting_control_new(const SettingDefinition* definition, json_t* parent, size_t index)
{
    SettingControl* control = g_new0(SettingControl, 1);
    control->definition = definition;
    control->parent = json_incref(parent);
    control->index = index;
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    g_object_set_data_full(G_OBJECT(content), "setting-control", control, setting_control_free);
    json_t* value = ls_splits_draft_setting_get(parent, definition->key, index);
    if (!value) {
        ls_splits_draft_setting_set(parent, definition->key, index, ls_splits_draft_setting_default(definition));
        value = ls_splits_draft_setting_get(parent, definition->key, index);
    }

    GtkWidget* field = NULL;
    switch (definition->type) {
        case SETTING_BOOLEAN:
            field = gtk_check_button_new();
            gtk_check_button_set_active(GTK_CHECK_BUTTON(field), json_is_true(value));
            g_signal_connect(field, "toggled", G_CALLBACK(setting_toggled), control);
            break;

        case SETTING_STRING:
        case SETTING_INTEGER:
        case SETTING_NUMBER:
            {
                field = entry_new(definition->type == SETTING_INTEGER
                        ? "Whole number"
                        : (definition->type == SETTING_NUMBER ? "Number" : NULL));

                char* text = json_is_string(value) ? g_strdup(json_string_value(value)) : json_dumps(value, JSON_ENCODE_ANY);
                gtk_editable_set_text(GTK_EDITABLE(field), text ? text : "");
                g_free(text);
                g_signal_connect(field, "changed", G_CALLBACK(setting_text_changed), control);
                break;
            }

        case SETTING_OPTION:
            {
                GtkStringList* names = gtk_string_list_new(NULL);
                guint selected = GTK_INVALID_LIST_POSITION;
                for (size_t i = 0; i < definition->options_count; ++i) {
                    gtk_string_list_append(names, definition->options[i].desc);
                    if (json_is_integer(value) && definition->options[i].value == json_integer_value(value)) {
                        selected = i;
                    }
                }

                field = gtk_drop_down_new(G_LIST_MODEL(names), NULL);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(field), selected);
                g_signal_connect(field, "notify::selected", G_CALLBACK(setting_option_changed), control);
                break;
            }

        case SETTING_OBJECT:
            field = form_new();
            for (size_t i = 0; i < definition->fields.count; ++i) {
                add_setting(field, i, &definition->fields.values[i], value, 0);
            }

            break;
        case SETTING_ARRAY:
            {
                control->items = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
                gtk_box_append(GTK_BOX(content), control->items);
                setting_array_refresh(control);
                field = gtk_button_new_with_label("Add Item");
                gtk_widget_set_halign(field, GTK_ALIGN_START);
                g_signal_connect(field, "clicked", G_CALLBACK(setting_item_add), control);
                break;
            }

        case SETTING_INVALID:
            break;
    }

    if (field) {
        if (definition->type != SETTING_OBJECT && definition->type != SETTING_ARRAY) {
            gtk_widget_set_tooltip_text(field, definition->desc);
        }

        gtk_box_append(GTK_BOX(content), field);
    }

    return content;
}

/**
 * @brief Compares settings by their definition order.
 *
 * @param a The first setting.
 * @param b The second setting.
 * @return int -1, 0, or 1 if the first order is lower, equal, or higher.
 */
static int setting_order_compare(gconstpointer a, gconstpointer b)
{
    const Setting* first = a;
    const Setting* second = b;
    if (first->order < second->order) {
        return -1;
    }

    return first->order > second->order;
}

/**
 * @brief Displays supplied settings without loading or running a script.
 *
 * @param window The splits editor window.
 * @param script_settings The supplied settings, or NULL to clear them.
 */
void ls_splits_editor_set_settings(GtkWindow* window, GHashTable* script_settings)
{
    Editor* editor = g_object_get_data(G_OBJECT(window), "editor");
    GHashTable* settings = script_settings ? g_hash_table_ref(script_settings) : NULL;

    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(editor->settings_box))) {
        gtk_box_remove(GTK_BOX(editor->settings_box), child);
    }

    g_clear_pointer(&editor->settings, g_hash_table_unref);
    editor->settings = settings;
    ls_splits_draft_set_settings(editor->draft, settings);
    gboolean selected = editor->draft->script && *editor->draft->script;
    if (!selected || !settings || !g_hash_table_size(settings)) {
        return;
    }

    GtkWidget* form = form_new();
    GList* items = g_list_sort(g_hash_table_get_values(settings), setting_order_compare);
    int row = 0;
    for (GList* item = items; item; item = item->next) {
        const Setting* setting = item->data;
        add_setting(form, row++, &setting->config, editor->draft->settings, 0);
    }

    g_list_free(items);
    gtk_box_append(GTK_BOX(editor->settings_box), settings_group_new("Settings", form, FALSE, NULL));
}

/**
 * @brief Clears the previous script's settings when its path changes.
 *
 * @param entry The edited script path (unused).
 * @param editor The editor whose settings should be cleared.
 */
static void script_changed(GtkEditable* entry, Editor* editor)
{
    ls_splits_editor_set_settings(editor->window, NULL);
}

/**
 * @brief Reloads the selected script's settings and saved values into the draft.
 *
 * @param entry The script path entry.
 * @param editor The editor receiving the settings.
 */
static void script_load(GtkEntry* entry, Editor* editor)
{
    ls_splits_editor_set_settings(editor->window, NULL);
    GtkWidget* message = g_object_get_data(G_OBJECT(entry), "resource-message");
    show_message(message, "");
    if (!editor->draft->script || !*editor->draft->script) {
        return;
    }

    char* error = NULL;
    GHashTable* script_settings = lasr_settings_read(editor->draft->script, &error);
    ls_splits_editor_set_settings(editor->window, script_settings);
    g_clear_pointer(&script_settings, g_hash_table_unref);
    if (error) {
        show_error(message, error);
        g_free(error);
    }
}

/**
 * @brief Creates the script field and its settings area.
 *
 * @param tabs The notebook receiving the Auto-splitter tab.
 * @param editor The editor supplying the draft and settings area.
 */
static void auto_splitter_page_new(GtkNotebook* tabs, Editor* editor)
{
    GtkWidget* page = page_new(tabs, "Auto-splitter");
    GtkWidget* form = form_new();
    add_field(form, 0, "Script", resource_new(&editor->draft->script, "Lua scripts (*.lua)", "lua", &editor->script));
    gtk_box_append(GTK_BOX(page), form);
    g_signal_connect(editor->script, "changed", G_CALLBACK(script_changed), editor);
    g_signal_connect(editor->script, "activate", G_CALLBACK(script_load), editor);
    editor->settings_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(page), editor->settings_box);
}

/**
 * @brief Creates the layout, theme, and window size fields.
 *
 * @param tabs The notebook receiving the Layout & Theme tab.
 * @param editor The editor supplying the draft.
 */
static void layout_page_new(GtkNotebook* tabs, Editor* editor)
{
    GtkWidget* page = page_new(tabs, "Layout & Theme");
    LSSplitsDraft* draft = editor->draft;
    GtkWidget* form = form_new();
    add_field(form, 0, "Layout", resource_new(&draft->layout, "JSON files (*.json)", "json", NULL));
    add_field(form, 1, "Theme", resource_new(&draft->theme, "CSS files (*.css)", "css", NULL));
    add_field(form, 2, "Theme variant", resource_new(&draft->theme_variant, "CSS files (*.css)", "css", NULL));
    add_field(form, 3, "Window width", draft_entry_new(&draft->width, NULL));
    add_field(form, 4, "Window height", draft_entry_new(&draft->height, NULL));
    gtk_box_append(GTK_BOX(page), form);
}

/**
 * @brief Applies the editor's style
 *
 * @param form The editor form to style.
 */
static void editor_style(GtkWidget* form)
{
    gtk_widget_add_css_class(form, "split-editor-form");
    GdkDisplay* display = gtk_widget_get_display(form);
    if (g_object_get_data(G_OBJECT(display), "libresplit-editor-style")) {
        return;
    }

    GtkCssProvider* style = gtk_css_provider_new();
    gtk_css_provider_load_from_string(style,
        ".split-editor-form notebook > header { padding: 0 24px; }"
        ".split-editor-form label.error-message { color: @error_color; }"
        ".split-editor-form frame.settings-group { border: 1px solid alpha(currentColor, .25); border-radius: 3px; padding: 12px; }"
        ".split-editor-form frame.settings-group.titled { border: 0; padding: 0; }"
        ".split-editor-form .group-start { border: solid alpha(currentColor, .25); border-width: 1px 0 0 1px; border-radius: 3px 0 0 0; }"
        ".split-editor-form .group-end { border: solid alpha(currentColor, .25); border-width: 1px 1px 0 0; border-radius: 0 3px 0 0; }"
        ".split-editor-form .group-body { border: solid alpha(currentColor, .25); border-width: 0 1px 1px; border-radius: 0 0 3px 3px; padding: 12px; }"
        ".split-editor-form button.add-button > image { color: @success_color; }"
        ".split-editor-form button.remove-button > image { color: @error_color; }"
        ".split-editor-form button.remove-button.compact:not(:hover):not(:active):not(:focus-visible) { background: none; box-shadow: none; }"
        ".split-editor-form button.remove-button:disabled > image { opacity: .5; }"
        ".split-editor-form .split-table { border: 1px solid alpha(currentColor, .25); border-radius: 3px; }"
        ".split-editor-form .split-heading { padding: 8px; }"
        ".split-editor-form .split-table row { padding: 5px 8px; border-bottom: 1px solid alpha(currentColor, .15); }"
        ".split-editor-form .split-table row:hover:not(:selected) { background: alpha(@theme_selected_bg_color, .25); }"
        ".split-editor-form .split-table row:selected { background: alpha(@theme_selected_bg_color, .5); color: inherit; }"
        ".split-editor-form .split-table row menubutton > button { color: inherit; }"
        ".split-editor-form .split-cell { background: transparent; border-color: transparent; box-shadow: none; min-height: 32px; }"
        ".split-editor-form .split-cell text { background: transparent; padding-right: 1px; }"
        ".split-editor-form .split-cell:focus-within { border-color: alpha(currentColor, .4); }");

    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_set_data_full(G_OBJECT(display), "libresplit-editor-style", style, g_object_unref);
}

/**
 * @brief Creates the editor form without loading or changing a splits file.
 *
 * @param editor The editor supplying the draft and controls.
 * @return GtkWidget* The complete editor form.
 */
static GtkWidget* editor_form_new(Editor* editor)
{
    GtkWidget* form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(form, TRUE);
    editor_style(form);

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_margin_top(header, 24);
    gtk_widget_set_margin_bottom(header, 24);
    gtk_widget_set_margin_start(header, 24);
    gtk_widget_set_margin_end(header, 24);
    gtk_box_append(GTK_BOX(header), icon_new(&editor->draft->icon, "Game icon", 64));
    GtkEditable* game;
    GtkEditable* category;
    gtk_box_append(GTK_BOX(header), ls_editor_game_new(&game, &category));
    g_signal_connect(game, "changed", G_CALLBACK(text_changed), &editor->draft->game);
    g_signal_connect(category, "changed", G_CALLBACK(text_changed), &editor->draft->category);
    gtk_box_append(GTK_BOX(form), header);

    GtkWidget* tabs = gtk_notebook_new();
    g_signal_connect(tabs, "unmap", G_CALLBACK(gtk_widget_set_focus_child), NULL);
    gtk_widget_set_vexpand(tabs, TRUE);
    splits_page_new(GTK_NOTEBOOK(tabs), editor);
    run_page_new(GTK_NOTEBOOK(tabs), editor);
    auto_splitter_page_new(GTK_NOTEBOOK(tabs), editor);
    layout_page_new(GTK_NOTEBOOK(tabs), editor);
    gtk_box_append(GTK_BOX(form), tabs);
    return form;
}

/**
 * @brief Releases the editor's draft and column sizing groups.
 *
 * @param data The editor state to release.
 */
static void editor_free(gpointer data)
{
    Editor* editor = data;
    for (guint i = 0; i < G_N_ELEMENTS(editor->columns); ++i) {
        g_object_unref(editor->columns[i]);
    }

    ls_splits_draft_free(editor->draft);
    g_clear_pointer(&editor->settings, g_hash_table_unref);
    g_free(editor);
}

/**
 * @brief Creates the editor window with its draft form and action buttons.
 *
 * @param parent The parent window, or NULL for a standalone editor.
 * @return GtkWindow* The new splits editor window.
 */
GtkWindow* ls_splits_editor_new(GtkWindow* parent)
{
    GtkWindow* window = GTK_WINDOW(gtk_window_new());
    Editor* editor = g_new0(Editor, 1);
    editor->window = window;
    editor->draft = ls_splits_draft_new();
    for (guint i = 0; i < G_N_ELEMENTS(editor->columns); ++i) {
        editor->columns[i] = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    }

    g_object_set_data_full(G_OBJECT(window), "editor", editor, editor_free);
    gtk_window_set_title(window, "Edit Splits — LibreSplit");
    gtk_window_set_default_size(window, 940, 780);
    gtk_window_set_resizable(window, TRUE);
    gtk_window_set_modal(window, TRUE);
    if (parent) {
        gtk_window_set_application(window, gtk_window_get_application(parent));
        gtk_window_set_transient_for(window, parent);
        gtk_window_set_destroy_with_parent(window, TRUE);
    }

    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(close_on_escape), window);
    gtk_widget_add_controller(GTK_WIDGET(window), keys);

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(window, content);

    gtk_box_append(GTK_BOX(content), editor_form_new(editor));
    gtk_box_append(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_widget_set_margin_top(actions, 16);
    gtk_widget_set_margin_bottom(actions, 16);
    gtk_widget_set_margin_start(actions, 24);
    gtk_widget_set_margin_end(actions, 24);

    GtkWidget* close = gtk_button_new_with_mnemonic("_Close");
    gtk_widget_set_size_request(close, 96, -1);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_close), window);
    gtk_box_append(GTK_BOX(actions), close);

    GtkWidget* save = gtk_button_new_with_mnemonic("_Save");
    gtk_widget_set_size_request(save, 96, -1);
    gtk_widget_set_sensitive(save, FALSE);
    gtk_box_append(GTK_BOX(actions), save);
    gtk_box_append(GTK_BOX(content), actions);

    return window;
}
