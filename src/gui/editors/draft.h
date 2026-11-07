#pragma once

#include "src/lasr/settings.h"
#include <glib.h>

/**
 * @brief Real and game time text in the draft.
 */
typedef struct {
    char* real_time;
    char* game_time;
} LSDraftTime;

/**
 * @brief A split's text, icon, and timing records in the draft.
 */
typedef struct {
    char* name;
    char* icon;
    LSDraftTime pb_split;
    LSDraftTime best_split;
    LSDraftTime best_segment;
} LSSplitDraftRow;

/**
 * @brief Unsaved editor values, separate from the loaded game.
 */
typedef struct {
    GPtrArray* rows;
    char* game;
    char* category;
    char* icon;
    char* script;
    char* layout;
    char* theme;
    char* theme_variant;
    char* start_delay;
    char* attempts;
    char* completed_runs;
    char* width;
    char* height;
    LSDraftTime world_record;
    guint comparison_method;
    json_t* settings;
} LSSplitsDraft;

LSSplitsDraft* ls_splits_draft_new(void);
void ls_splits_draft_free(LSSplitsDraft* draft);
void ls_splits_draft_set_text(char** field, const char* text);
LSSplitDraftRow* ls_splits_draft_add_row(LSSplitsDraft* draft, guint position);
void ls_splits_draft_remove_row(LSSplitsDraft* draft, guint position);
void ls_splits_draft_move_row(LSSplitsDraft* draft, guint from, guint to);
void ls_splits_draft_set_settings(LSSplitsDraft* draft, GHashTable* definitions);
json_t* ls_splits_draft_setting_default(const SettingDefinition* definition);
json_t* ls_splits_draft_setting_get(json_t* parent, const char* key, size_t index);
void ls_splits_draft_setting_set(json_t* parent, const char* key, size_t index, json_t* value);
