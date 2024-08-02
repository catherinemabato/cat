/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dtgtk/resetlabel.h"

G_DEFINE_TYPE(GtkDarktableResetLabel, dtgtk_reset_label, GTK_TYPE_EVENT_BOX);

static void dtgtk_reset_label_class_init(GtkDarktableResetLabelClass *klass)
{
}

static void dtgtk_reset_label_init(GtkDarktableResetLabel *label)
{
}

static gboolean _reset_label_callback(
    GtkGestureMultiPress *gesture,
    int n_press,
    double x,
    double y,
    gpointer user_data)
{
  if(n_press == 2)
  {
    // on double click, reset all settings
    GtkDarktableResetLabel *label = (GtkDarktableResetLabel *)user_data;

    memcpy(((char *)label->module->params) + label->offset,
           ((char *)label->module->default_params) + label->offset, label->size);
    if(label->module->gui_update) label->module->gui_update(label->module);
    dt_dev_add_history_item(darktable.develop, label->module, FALSE);

    return TRUE;
  }

  return FALSE;
}

// public functions
GtkWidget *dtgtk_reset_label_new(const gchar *text, dt_iop_module_t *module, void *param, int param_size, GCallback reset_callback)
{
  GtkDarktableResetLabel *label;
  label = g_object_new(dtgtk_reset_label_get_type(), NULL);
  label->module = module;
  label->offset = param - (void *)module->params;
  label->size = param_size;

  label->lb = GTK_LABEL(gtk_label_new(text));
  gtk_widget_set_halign(GTK_WIDGET(label->lb), GTK_ALIGN_START);
  gtk_label_set_ellipsize(GTK_LABEL(label->lb), PANGO_ELLIPSIZE_END);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(label), FALSE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(label), _("double-click to reset"));
  gtk_container_add(GTK_CONTAINER(label), GTK_WIDGET(label->lb));
  gtk_widget_add_events(GTK_WIDGET(label), GDK_BUTTON_PRESS_MASK);

  label->gesture_button_primary = dtgtk_button_default_handler_new(
      GTK_WIDGET(label),
      GDK_BUTTON_PRIMARY,
      G_CALLBACK(_reset_label_callback),
      NULL,
      label);

  return (GtkWidget *)label;
}

void dtgtk_reset_label_set_text(GtkDarktableResetLabel *label, const gchar *str)
{
  gtk_label_set_text(label->lb, str);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

