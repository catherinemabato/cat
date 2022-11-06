/*
    This file is part of darktable,
    Copyright (C) 2011-2022 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/filters/filters.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_tool_filter_filter_t
{
  dt_collection_properties_t prop;
  gchar *raw_text;

  dt_lib_filters_rule_t *rule;
} dt_lib_tool_filter_filter_t;

typedef struct dt_lib_tool_filter_t
{
  GtkWidget *filter_box;
  GtkWidget *sort_box;
<<<<<<< HEAD
  GtkWidget *count;
=======

  GList *filters;
  GList *sorts;
>>>>>>> 07f7975e37 (filters : reactivate topbar)
} dt_lib_tool_filter_t;

const char *name(dt_lib_module_t *self)
{
  return _("filter");
}

const char **views(dt_lib_module_t *self)
{
  /* for now, show in all view due this affects filmroll too

     TODO: Consider to add flag for all views, which prevents
           unloading/loading a module while switching views.

   */
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 2001;
}

static GtkWidget *_lib_filter_get_filter_box(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  return d->filter_box;
}
static GtkWidget *_lib_filter_get_sort_box(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  return d->sort_box;
}

static GtkWidget *_lib_filter_get_count(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  return d->count;
}

static void _dt_collection_updated(gpointer instance, dt_collection_change_t query_change,
                                   dt_collection_properties_t changed_property, gpointer imgs, int next,
                                   gpointer self)
{
  /* TODO
  dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)dm->data;

  gchar *where_ext = dt_collection_get_extended_where(darktable.collection, 99999);
  if(g_strcmp0(where_ext, d->last_where_ext))
  {
    g_free(d->last_where_ext);
    d->last_where_ext = g_strdup(where_ext);
    for(int i = 0; i <= d->nb_rules; i++)
    {
      _widget_update(&d->rule[i]);
    }
  }*/
}

static void _filters_changed(void *data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;

  // save the values
  char confname[200] = { 0 };
  int i = 0;
  int last = -1;
  for(GList *iter = d->filters; iter; iter = g_list_next(iter))
  {
    dt_lib_tool_filter_filter_t *f = (dt_lib_tool_filter_filter_t *)iter->data;
    snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/item%1d", i);
    dt_conf_set_int(confname, f->prop);
    snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/string%1d", i);
    dt_conf_set_string(confname, f->raw_text);
    last = f->prop;
    i++;
  }
  dt_conf_set_int("plugins/lighttable/topbar/num_rules", i);

  if(i == 0) return;

  // update the query without throwing signal everywhere
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, last, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
}

static void _filter_free(gpointer data)
{
  dt_lib_tool_filter_filter_t *filter = (dt_lib_tool_filter_filter_t *)data;
  if(filter->raw_text) g_free(filter->raw_text);
  dt_filters_free(filter->rule);
}

static void _filters_init(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;

  // first, let's reset all remaining filters
  g_list_free_full(d->filters, _filter_free);
  d->filters = NULL;

  // then we read the number of existing filters
  ++darktable.gui->reset;
  const int nb = MAX(dt_conf_get_int("plugins/lighttable/topbar/num_rules"), 0);
  char confname[200] = { 0 };

  // create or update defined rules
  for(int i = 0; i < nb; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/item%1d", i);
    const int prop = dt_conf_get_int(confname);
    if(!dt_filters_exists(prop)) continue;

    // create a new filter structure
    dt_lib_tool_filter_filter_t *f = (dt_lib_tool_filter_filter_t *)g_malloc0(sizeof(dt_lib_tool_filter_filter_t));

    f->prop = prop;
    snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/string%1d", i);
    f->raw_text = dt_conf_get_string(confname);

    f->rule = (dt_lib_filters_rule_t *)g_malloc0(sizeof(dt_lib_filters_rule_t));
    f->rule->parent = self;
    f->rule->rule_changed = _filters_changed;
    if(f->rule->w_special_box) gtk_widget_destroy(f->rule->w_special_box);
    f->rule->w_special_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    dt_filters_init(f->rule, f->prop, f->raw_text, self, TRUE);

    gtk_box_pack_start(GTK_BOX(d->filter_box), f->rule->w_special_box, FALSE, TRUE, 0);
    d->filters = g_list_append(d->filters, f);
  }

  gtk_widget_show_all(d->filter_box);

  --darktable.gui->reset;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)g_malloc0(sizeof(dt_lib_tool_filter_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(self->widget, GTK_ALIGN_CENTER);

  d->filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->filter_box, "header-rule-box");
  gtk_box_pack_start(GTK_BOX(self->widget), d->filter_box, FALSE, FALSE, 0);

  d->sort_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->sort_box, "header-sort-box");
  gtk_box_pack_start(GTK_BOX(self->widget), d->sort_box, FALSE, FALSE, 0);
  GtkWidget *label = gtk_label_new(_("sort by"));
  gtk_box_pack_start(GTK_BOX(d->sort_box), label, TRUE, TRUE, 0);

  /* label to display selected count */
  d->count = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(d->count), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(self->widget), d->count, TRUE, FALSE, 0);

  /* initialize proxy */
  darktable.view_manager->proxy.filter.module = self;
  darktable.view_manager->proxy.filter.get_filter_box = _lib_filter_get_filter_box;
  darktable.view_manager->proxy.filter.get_sort_box = _lib_filter_get_sort_box;
<<<<<<< HEAD
  darktable.view_manager->proxy.filter.get_count = _lib_filter_get_count;
=======

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                                  G_CALLBACK(_dt_collection_updated), self);

  // initialize the filters
  _filters_init(self);
>>>>>>> 07f7975e37 (filters : reactivate topbar)
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  g_list_free_full(d->filters, _filter_free);
  g_free(self->data);
  self->data = NULL;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_collection_updated), self);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
