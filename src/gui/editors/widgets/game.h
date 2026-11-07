#pragma once

#include <gtk/gtk.h>

GtkWidget* ls_editor_game_new(GtkEditable** game, GtkEditable** category);
void ls_editor_game_set_choices(GtkEditable* entry, const char* const* choices);
