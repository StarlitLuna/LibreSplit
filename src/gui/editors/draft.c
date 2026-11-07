#include "draft.h"

/**
 * @brief Releases both timing values.
 *
 * @param time The draft timing values to release.
 */
static void time_clear(LSDraftTime* time)
{
    g_clear_pointer(&time->real_time, g_free);
    g_clear_pointer(&time->game_time, g_free);
}

/**
 * @brief Releases a split's draft values.
 *
 * @param data The draft row to release.
 */
static void row_free(gpointer data)
{
    LSSplitDraftRow* row = data;
    g_free(row->name);
    g_free(row->icon);
    time_clear(&row->pb_split);
    time_clear(&row->best_split);
    time_clear(&row->best_segment);
    g_free(row);
}

/**
 * @brief Creates an empty draft without loading a splits file.
 *
 * @return LSSplitsDraft* The new draft, owned by the caller.
 */
LSSplitsDraft* ls_splits_draft_new(void)
{
    LSSplitsDraft* draft = g_new0(LSSplitsDraft, 1);
    draft->rows = g_ptr_array_new_with_free_func(row_free);
    draft->settings = json_object();
    return draft;
}

/**
 * @brief Releases the draft and its rows.
 *
 * @param draft The draft to release.
 */
void ls_splits_draft_free(LSSplitsDraft* draft)
{
    g_ptr_array_unref(draft->rows);
    json_decref(draft->settings);
    g_free(draft->game);
    g_free(draft->category);
    g_free(draft->icon);
    g_free(draft->script);
    g_free(draft->layout);
    g_free(draft->theme);
    g_free(draft->theme_variant);
    g_free(draft->start_delay);
    g_free(draft->attempts);
    g_free(draft->completed_runs);
    g_free(draft->width);
    g_free(draft->height);
    time_clear(&draft->world_record);
    g_free(draft);
}

/**
 * @brief Replaces a draft field with a copy of the entered text.
 *
 * @param field The draft string to replace.
 * @param text The new text, or NULL to clear the value.
 */
void ls_splits_draft_set_text(char** field, const char* text)
{
    g_set_str(field, text);
}

/**
 * @brief Inserts a new empty split at the requested position.
 *
 * @param draft The draft receiving the row.
 * @param position The zero-based insertion position.
 * @return LSSplitDraftRow* The row owned by the draft, or NULL for an invalid position.
 */
LSSplitDraftRow* ls_splits_draft_add_row(LSSplitsDraft* draft, guint position)
{
    g_return_val_if_fail(position <= draft->rows->len, NULL);
    LSSplitDraftRow* row = g_new0(LSSplitDraftRow, 1);
    g_ptr_array_insert(draft->rows, position, row);
    return row;
}

/**
 * @brief Removes a split and releases its draft values.
 *
 * @param draft The draft containing the row.
 * @param position The zero-based position to remove.
 */
void ls_splits_draft_remove_row(LSSplitsDraft* draft, guint position)
{
    g_ptr_array_remove_index(draft->rows, position);
}

/**
 * @brief Moves a split while keeping its name, icon, and times together.
 *
 * @param draft The draft containing the row.
 * @param from The row's current zero-based position.
 * @param to The row's new zero-based position.
 */
void ls_splits_draft_move_row(LSSplitsDraft* draft, guint from, guint to)
{
    g_return_if_fail(from < draft->rows->len && to < draft->rows->len);
    gpointer row = g_ptr_array_steal_index(draft->rows, from);
    g_ptr_array_insert(draft->rows, to, row);
}

/**
 * @brief Copies supplied setting values into the isolated draft.
 *
 * @param draft The draft receiving the setting values.
 * @param definitions The supplied settings, or NULL to clear them.
 */
void ls_splits_draft_set_settings(LSSplitsDraft* draft, GHashTable* definitions)
{
    json_decref(draft->settings);
    draft->settings = json_object();
    if (!definitions) {
        return;
    }

    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, definitions);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Setting* setting = value;
        json_t* initial = lasr_setting_value_to_json(setting->config.type,
            setting->set ? &setting->val : &setting->config.default_val);
        json_object_set_new(draft->settings, setting->config.key, initial);
    }
}

/**
 * @brief Copies a setting's default value for a new array item.
 *
 * @param definition The setting whose default should be copied.
 * @return json_t* A new JSON value owned by the caller.
 */
json_t* ls_splits_draft_setting_default(const SettingDefinition* definition)
{
    return lasr_setting_value_to_json(definition->type, &definition->default_val);
}

/**
 * @brief Reads a setting by object key or array index.
 *
 * @param parent The JSON object or array containing the value.
 * @param key The object key. Ignored for arrays.
 * @param index The array index. Ignored for objects.
 * @return json_t* The borrowed value, or NULL if it is missing.
 */
json_t* ls_splits_draft_setting_get(json_t* parent, const char* key, size_t index)
{
    return json_is_array(parent) ? json_array_get(parent, index) : json_object_get(parent, key);
}

/**
 * @brief Replaces an object field or array item, taking ownership of its value.
 *
 * @param parent The JSON object or array to update.
 * @param key The object key. Ignored for arrays.
 * @param index The array index. Ignored for objects.
 * @param value The replacement value. Ownership is transferred.
 */
void ls_splits_draft_setting_set(json_t* parent, const char* key, size_t index, json_t* value)
{
    if (json_is_array(parent)) {
        json_array_set_new(parent, index, value);
    } else {
        json_object_set_new(parent, key, value);
    }
}
