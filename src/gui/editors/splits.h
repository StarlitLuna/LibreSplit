#pragma once

#include <gtk/gtk.h>

GtkWindow* ls_splits_editor_new(GtkWindow* parent);
gboolean ls_splits_editor_can_open(void);
void ls_splits_editor_open(GSimpleAction* action, GVariant* parameter, gpointer app);
void ls_splits_editor_set_settings(GtkWindow* window, GHashTable* definitions);
