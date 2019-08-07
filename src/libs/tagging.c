/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 henrik andersson.
    copyright (c) 2019 philippe weyland.

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
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <math.h>

#define FLOATING_ENTRY_WIDTH DT_PIXEL_APPLY_DPI(150)

DT_MODULE(1)

static gboolean _lib_tagging_tag_show(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, dt_lib_module_t *self);

typedef struct dt_lib_tagging_t
{
  char keyword[1024];
  GtkEntry *entry;
  GtkTreeView *current, *related;
  int imgsel;

  GtkWidget *attach_button, *detach_button, *new_button, *import_button, *export_button, *currentwindow, *relatedwindow;
  GtkWidget *toggle_tree_button, *toggle_suggestion_button, *toggle_sort_button, *toggle_hide_button, *toggle_dttags_button;
  gulong tree_button_handler, suggestion_button_handler, sort_button_handler, hide_button_handler;
  GtkListStore *current_liststore, *related_liststore;
  GtkTreeStore *related_treestore;
  GtkWidget *floating_tag_window;
  int floating_tag_imgid;
  gboolean tree_flag, suggestion_flag, sort_count_flag, hide_path_flag, dttags_flag;
} dt_lib_tagging_t;

typedef struct dt_tag_op_t
{
  gint tagid;
  guint count;
  char *newtagname;
  char *oldtagname;
  int select;
  gboolean tree_flag, suggestion_flag;
} dt_tag_op_t;

typedef enum dt_lib_tagging_cols_t
{
  DT_LIB_TAGGING_COL_TAG = 0,
  DT_LIB_TAGGING_COL_ID,
  DT_LIB_TAGGING_COL_PATH,
  DT_LIB_TAGGING_COL_COUNT,
  DT_LIB_TAGGING_COL_SEL,
  DT_LIB_TAGGING_COL_FLAGS,
  DT_LIB_TAGGING_NUM_COLS
} dt_lib_tagging_cols_t;

typedef enum dt_tag_sort_id
{
  DT_TAG_SORT_PATH_ID,
  DT_TAG_SORT_NAME_ID,
  DT_TAG_SORT_COUNT_ID
} dt_tag_sort_id;

const char *name(dt_lib_module_t *self)
{
  return _("tagging");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v1[] = {"lighttable", "darkroom", "map", "tethering", NULL};
  static const char *v2[] = {"lighttable", "map", "tethering", NULL};

  if(dt_conf_get_bool("plugins/darkroom/tagging/visible"))
    return v1;
  else
    return v2;
}

uint32_t container(dt_lib_module_t *self)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
    return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
  else
    return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "attach"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "detach"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "new"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "tag"), GDK_KEY_t, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  dt_accel_connect_button_lib(self, "attach", d->attach_button);
  dt_accel_connect_button_lib(self, "detach", d->detach_button);
  dt_accel_connect_button_lib(self, "new", d->new_button);
  dt_accel_connect_lib(self, "tag", g_cclosure_new(G_CALLBACK(_lib_tagging_tag_show), self, NULL));
}

static void propagate_sel_to_parents(GtkTreeModel *model, GtkTreeIter *iter)
{
  guint sel;
  GtkTreeIter parent, child = *iter;
  while(gtk_tree_model_iter_parent(model, &parent, &child))
  {
    gtk_tree_model_get(model, &parent, DT_LIB_TAGGING_COL_SEL, &sel, -1);
    if (!sel)
      gtk_tree_store_set(GTK_TREE_STORE(model), &parent, DT_LIB_TAGGING_COL_SEL, 1, -1);
    child = parent;
  }
}

static void init_treeview(dt_lib_module_t *self, int which)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GList *tags = NULL;
  uint32_t count;

  if(which == 0) // tags of selected images
  {
    const int imgsel = dt_control_get_mouse_over_id();
    d->imgsel = imgsel;
    count = dt_tag_get_attached(imgsel, &tags, d->dttags_flag ? FALSE : TRUE);
  }
  else // related tags of typed text
  {
    if (d->suggestion_flag)
      count = dt_tag_get_suggestions(d->keyword, &tags);
    else
      count = dt_tag_get_with_usage(d->keyword, &tags);
  }

  GtkTreeIter iter;
  GtkTreeView *view;
  if(which == 0)
    view = d->current;
  else
    view = d->related;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);

  if (which && d->tree_flag)
  {
    gtk_tree_store_clear(GTK_TREE_STORE(model));
    {
      char **last_tokens = NULL;
      int last_tokens_length = 0;
      GtkTreeIter last_parent = { 0 };
      GList *sorted_tags = dt_sort_tag(tags, 0);
      tags = sorted_tags;
      for(GList *taglist = tags; taglist; taglist = g_list_next(taglist))
      {
        const gchar *tag = ((dt_tag_t *)taglist->data)->tag;
        const guint id = ((dt_tag_t *)taglist->data)->id;
        const guint tagc = ((dt_tag_t *)taglist->data)->count;
        const guint sel = ((dt_tag_t *)taglist->data)->select;
        const guint flags = ((dt_tag_t *)taglist->data)->flags;
        if(tag == NULL) continue;
        char **tokens;
        tokens = g_strsplit(tag, "|", -1);
        if(tokens)
        {
          // find the number of common parts at the beginning of tokens and last_tokens
          GtkTreeIter parent = last_parent;
          const int tokens_length = g_strv_length(tokens);
          int common_length = 0;
          if(last_tokens)
          {
            while(tokens[common_length] && last_tokens[common_length] &&
                  !g_strcmp0(tokens[common_length], last_tokens[common_length]))
            {
              common_length++;
            }

            // point parent iter to where the entries should be added
            for(int i = common_length; i < last_tokens_length; i++)
            {
              gtk_tree_model_iter_parent(model, &parent, &last_parent);
              last_parent = parent;
            }
          }

          // insert everything from tokens past the common part
          char *pth = NULL;
          for(int i = 0; i < common_length; i++)
            pth = dt_util_dstrcat(pth, "%s|", tokens[i]);

          for(char **token = &tokens[common_length]; *token; token++)
          {
            pth = dt_util_dstrcat(pth, "%s|", *token);
            gchar *pth2 = g_strdup(pth);
            pth2[strlen(pth2) - 1] = '\0';
            gtk_tree_store_insert(GTK_TREE_STORE(model), &iter, common_length > 0 ? &parent : NULL, -1);
            gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                              DT_LIB_TAGGING_COL_TAG, *token,
                              DT_LIB_TAGGING_COL_ID, (token == &tokens[tokens_length-1]) ? id : 0,
                              DT_LIB_TAGGING_COL_PATH, pth2,
                              DT_LIB_TAGGING_COL_COUNT, (token == &tokens[tokens_length-1]) ? tagc : 0,
                              DT_LIB_TAGGING_COL_SEL, sel,
                              DT_LIB_TAGGING_COL_FLAGS, flags,
                              -1);
            if (sel)
              propagate_sel_to_parents(model, &iter);
            common_length++;
            parent = iter;
            g_free(pth2);
          }
          g_free(pth);

          // remember things for the next round
          if(last_tokens) g_strfreev(last_tokens);
          last_tokens = tokens;
          last_parent = parent;
          last_tokens_length = tokens_length;
        }
      }
      g_strfreev(last_tokens);
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
    g_object_unref(model);
    if (d->keyword[0])
      gtk_tree_view_expand_all(d->related);
  }
  else
  {
    gtk_list_store_clear(GTK_LIST_STORE(model));
    if(count > 0 && tags)
    {
      for (GList *tag = tags; tag; tag = g_list_next(tag))
      {
        const char *subtag = g_strrstr(((dt_tag_t *)tag->data)->tag, "|");
        gtk_list_store_append(GTK_LIST_STORE(model), &iter);
        gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                          DT_LIB_TAGGING_COL_TAG, !subtag ? ((dt_tag_t *)tag->data)->tag : subtag + 1,
                          DT_LIB_TAGGING_COL_ID, ((dt_tag_t *)tag->data)->id,
                          DT_LIB_TAGGING_COL_PATH, ((dt_tag_t *)tag->data)->tag,
                          DT_LIB_TAGGING_COL_COUNT, ((dt_tag_t *)tag->data)->count,
                          DT_LIB_TAGGING_COL_SEL, ((dt_tag_t *)tag->data)->select,
                          DT_LIB_TAGGING_COL_FLAGS, ((dt_tag_t *)tag->data)->flags,
                          -1);
      }
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
    g_object_unref(model);
  }
  dt_tag_free_result(&tags);
}

void tree_tagname_show(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model, GtkTreeIter *iter,
                     gpointer data, gboolean related)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  guint id;
  gchar *name;
  gchar *path;
  guint count;
  gchar *coltext;
  gint flags;

  gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_ID, &id, DT_LIB_TAGGING_COL_TAG, &name,
                  DT_LIB_TAGGING_COL_COUNT, &count, DT_LIB_TAGGING_COL_FLAGS, &flags,
                  DT_LIB_TAGGING_COL_PATH, &path, -1);
  const gboolean hide = related ? (d->tree_flag ? TRUE : d->hide_path_flag) : d->hide_path_flag;
  const gboolean istag = id && !(flags & DT_TF_CATEGORY);
  if ((related && !count) || (!related && count <= 1))
  {
    coltext = g_markup_printf_escaped(istag ? "%s" : "<i>%s</i>", hide ? name : path);
  }
  else
  {
    coltext = g_markup_printf_escaped(istag ? "%s (%d)" : "<i>%s</i> (%d)", hide ? name : path, count);
  }
  g_object_set(renderer, "markup", coltext, NULL);
  g_free(coltext);
  g_free(name);
}

void tree_tagname_show_current(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model, GtkTreeIter *iter,
                     gpointer data)
{
  tree_tagname_show(col, renderer, model, iter, data, 0);
}

void tree_tagname_show_related(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model, GtkTreeIter *iter,
                     gpointer data)
{
  tree_tagname_show(col, renderer, model, iter, data, 1);
}

void tree_select_show(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model, GtkTreeIter *iter,
                     gpointer data)
{
  guint id;
  guint select;
  gboolean active = FALSE;
  gboolean inconsistent = FALSE;

  gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_ID, &id, DT_LIB_TAGGING_COL_SEL, &select, -1);
  if (!id)
  {
    if (select) inconsistent = TRUE;
  }
  else
  {
    if (select == 2) active = TRUE;
    else if (select == 1) inconsistent = TRUE;
  }
  g_object_set(renderer, "active", active, "inconsistent", inconsistent, NULL);
}

static void sort_current_list(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  const gint sort = d->sort_count_flag ? DT_TAG_SORT_COUNT_ID : d->hide_path_flag ? DT_TAG_SORT_NAME_ID : DT_TAG_SORT_PATH_ID;
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->current_liststore), sort, GTK_SORT_ASCENDING);
}

static void sort_related_list(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  if (!d->tree_flag)
  {
    const gint sort = d->sort_count_flag ? DT_TAG_SORT_COUNT_ID : d->hide_path_flag ? DT_TAG_SORT_NAME_ID : DT_TAG_SORT_PATH_ID;
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->related_liststore), sort, GTK_SORT_ASCENDING);
  }
}

static void _lib_tagging_redraw_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  int imgsel = dt_control_get_mouse_over_id();
  if(imgsel != d->imgsel)
  {
    init_treeview(self, 0);
    sort_current_list(self);
    sort_related_list(self);
  }
}

static void _lib_tagging_tags_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  init_treeview(self, 0);
  init_treeview(self, 1);
  sort_current_list(self);
  sort_related_list(self);
}

static void raise_signal_tag_changed(dt_lib_module_t *self)
{ // raises change only for other modules
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
}

// find a tag on the tree
static gboolean find_tag_iter_tagid(GtkTreeModel *model, GtkTreeIter *iter, gint tagid)
{
  gint tag;
  do
  {
    gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_ID, &tag, -1);
    if (tag == tagid)
    {
      return TRUE;
    }
    GtkTreeIter child, parent = *iter;
    if (gtk_tree_model_iter_children(model, &child, &parent))
      if (find_tag_iter_tagid(model, &child, tagid))
      {
        *iter = child;
        return TRUE;
      }
  } while (gtk_tree_model_iter_next(model, iter));
  return FALSE;
}

// calculate the indeterminated state (1) where needed on the tree
static void calculate_sel_on_path(GtkTreeModel *model, GtkTreeIter *iter, gboolean root)
{
  GtkTreeIter child, parent = *iter;
  do
  {
    gint sel = 0;
    gtk_tree_model_get(model, &parent, DT_LIB_TAGGING_COL_SEL, &sel, -1);
    if (sel == 2)
    {
      propagate_sel_to_parents(model, &parent);
    }
    if (gtk_tree_model_iter_children(model, &child, &parent))
      calculate_sel_on_path(model, &child, FALSE);
  } while (!root && gtk_tree_model_iter_next(model, &parent));
}
/*
static void printf_tree_data(GtkTreeModel *model, GtkTreeIter *iter, char *proc_name)
{
  char *path = NULL;
  char *tag = NULL;
  gint tagid = 0;
  gint sel = 0;

  gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_PATH, &path, DT_LIB_TAGGING_COL_TAG, &tag,
            DT_LIB_TAGGING_COL_SEL, &sel, DT_LIB_TAGGING_COL_ID, &tagid, -1);
  printf("%s id %d tag %s path %s sel %d\n", proc_name path, tagid, tag, path, sel);
  g_free(path);
  g_free(tag);
} */

// reset the indeterminated selection (1) on the tree
static void reset_sel_on_path(GtkTreeModel *model, GtkTreeIter *iter, gboolean root)
{
  GtkTreeIter child, parent = *iter;
  do
  {
    if (gtk_tree_model_iter_children(model, &child, &parent))
    {
      gint sel = 0;
      gtk_tree_model_get(model, &parent, DT_LIB_TAGGING_COL_SEL, &sel, -1);
      if (sel == 1)
      {
        gtk_tree_store_set(GTK_TREE_STORE(model), &parent, DT_LIB_TAGGING_COL_SEL, 0, -1);
      }
      reset_sel_on_path(model, &child, FALSE);
    }
  } while (!root && gtk_tree_model_iter_next(model, &parent));
}

// reset all selection (1 & 2) on the tree
static void reset_sel_on_path_full(GtkTreeModel *model, GtkTreeIter *iter, gboolean root)
{
  GtkTreeIter child, parent = *iter;
  do
  {
    gtk_tree_store_set(GTK_TREE_STORE(model), &parent, DT_LIB_TAGGING_COL_SEL, 0, -1);
    if (gtk_tree_model_iter_children(model, &child, &parent))
      reset_sel_on_path_full(model, &child, FALSE);
  } while (!root && gtk_tree_model_iter_next(model, &parent));
}

//  try to find a node fully attached (2) which is the root of the update loop. If not the full tree will be used
static void find_root_iter_iter(GtkTreeModel *model, GtkTreeIter *iter, GtkTreeIter *parent)
{
  guint sel;
  GtkTreeIter child = *iter;
  while (gtk_tree_model_iter_parent(model, parent, &child))
  {
    gtk_tree_model_get(model, parent, DT_LIB_TAGGING_COL_SEL, &sel, -1);
    if (sel == 2)
    {
      char *path = NULL;
      gtk_tree_model_get(model, parent, DT_LIB_TAGGING_COL_PATH, &path, -1);
      g_free(path);
      return; // no need to go further
    }
    child = *parent;
  }
  *parent = child;  // last before root
  char *path = NULL;
  gtk_tree_model_get(model, parent, DT_LIB_TAGGING_COL_PATH, &path, -1);
  g_free(path);
}

// with tag detach update the tree selection
static void calculate_sel_on_tree(GtkTreeModel *model, GtkTreeIter *iter)
{
  GtkTreeIter parent;
  if (iter)
  {  // only on sub-tree
    find_root_iter_iter(model, iter, &parent);
    reset_sel_on_path(model, &parent, TRUE);
    calculate_sel_on_path(model, &parent, TRUE);
  }
  else
  { // on full tree
    gtk_tree_model_get_iter_first(model, &parent);
    reset_sel_on_path(model, &parent, FALSE);
    calculate_sel_on_path(model, &parent, FALSE);
  }
}

// get the new selected images and update the tree selection
static void update_sel_on_tree(GtkTreeModel *model)
{
  GList *tags = NULL;
  const guint count = dt_tag_get_attached(-1, &tags, TRUE);
  if(count > 0 && tags)
  {
    GtkTreeIter parent;
    gtk_tree_model_get_iter_first(model, &parent);
    reset_sel_on_path_full(model, &parent, FALSE);
    for (GList *tag = tags; tag; tag = g_list_next(tag))
    {
      GtkTreeIter iter = parent;
      if (find_tag_iter_tagid(model, &iter, ((dt_tag_t *)tag->data)->id))
      {
        gtk_tree_store_set(GTK_TREE_STORE(model), &iter, DT_LIB_TAGGING_COL_SEL, ((dt_tag_t *)tag->data)->select, -1);
        propagate_sel_to_parents(model, &iter);
      }
    }
  }
  dt_tag_free_result(&tags);
}

// delete a tag in the tree (tree or list)
static void delete_tree_tag(GtkTreeModel *model, GtkTreeIter *iter, gboolean tree)
{
  guint tagid = 0;
  gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_ID, &tagid, -1);
  if (tree)
  {
    if (tagid)
    {
      gtk_tree_store_set(GTK_TREE_STORE(model), iter, DT_LIB_TAGGING_COL_SEL, 0,
          DT_LIB_TAGGING_COL_ID, 0, DT_LIB_TAGGING_COL_COUNT, 0, -1);
      calculate_sel_on_tree(model, iter);
      GtkTreeIter child, parent = *iter;
      if (!gtk_tree_model_iter_children(model, &child, &parent))
        gtk_tree_store_remove(GTK_TREE_STORE(model), iter);
    }
  }
  else
  {
    gtk_list_store_remove(GTK_LIST_STORE(model), iter);
  }
}

// delete a branch of the tag tree (tree or list)
static void delete_tree_path(GtkTreeModel *model, GtkTreeIter *iter, gboolean root, gboolean tree)
{
  if (tree)
  {
    GtkTreeIter child, parent = *iter;
    gboolean valid = TRUE;
    do
    {
      if (gtk_tree_model_iter_children(model, &child, &parent))
        delete_tree_path(model, &child, FALSE, tree);
      GtkTreeIter tobedel = parent;
      valid = gtk_tree_model_iter_next(model, &parent);
      if (root)
      {
        gtk_tree_store_set(GTK_TREE_STORE(model), &tobedel, DT_LIB_TAGGING_COL_SEL, 0,
            DT_LIB_TAGGING_COL_COUNT, 0, -1);

        char *path2 = NULL;
        gtk_tree_model_get(model, &tobedel, DT_LIB_TAGGING_COL_PATH, &path2, -1);
        g_free(path2);

        calculate_sel_on_tree(model, &tobedel);
      }
      char *path = NULL;
      gtk_tree_model_get(model, &tobedel, DT_LIB_TAGGING_COL_PATH, &path, -1);
      g_free(path);
      gtk_tree_store_remove(GTK_TREE_STORE(model), &tobedel);
    } while (!root && valid);
  }
  else
  {
    GtkTreeIter child;
    char *path = NULL;
    gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_PATH, &path, -1);
    guint pathlen = strlen(path);
    gboolean valid = gtk_tree_model_get_iter_first(model, &child);
    while(valid)
    {
      char *path2 = NULL;
      gtk_tree_model_get(model, &child, DT_LIB_TAGGING_COL_PATH, &path2, -1);
      GtkTreeIter tobedel = child;
      valid = gtk_tree_model_iter_next (model, &child);
      if (strlen(path2) >= pathlen)
      {
        char letter = path2[pathlen];
        path2[pathlen] = '\0';
        if (g_strcmp0(path, path2) == 0)
        {
          path2[pathlen] = letter;
          gtk_list_store_remove(GTK_LIST_STORE(model), &tobedel);
        }
      }
      g_free(path2);
    }
    g_free(path);
  }
}

static void _lib_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  init_treeview(self, 0);
  sort_current_list(self);
  if (d->suggestion_flag)
  {
    init_treeview(self, 1);
    sort_related_list(self);
  }
  else if (d->tree_flag)
    update_sel_on_tree(GTK_TREE_MODEL(d->related_treestore));
}

static void set_keyword(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  const gchar *beg = g_strrstr(gtk_entry_get_text(d->entry), ",");
  if(!beg)
    beg = gtk_entry_get_text(d->entry);
  else
  {
    if(*beg == ',') beg++;
    if(*beg == ' ') beg++;
  }
  snprintf(d->keyword, sizeof(d->keyword), "%s", beg);
}

static gboolean update_tag_name_per_id(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, dt_tag_op_t *to)
{
  gint tag;
  gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_ID, &tag, -1);
  if (tag == to->tagid)
  {
    char *newtagname = to->newtagname;
    if (!to->suggestion_flag)
    {
      if (!to->tree_flag)
      {
        gtk_list_store_set(GTK_LIST_STORE(model), iter, DT_LIB_TAGGING_COL_PATH, newtagname,
                                    DT_LIB_TAGGING_COL_TAG, newtagname, -1);
      }
      else
      {
        char *subtag = g_strrstr(to->newtagname, "|");
        subtag = (!subtag) ? newtagname : subtag + 1;
        gtk_tree_store_set(GTK_TREE_STORE(model), iter, DT_LIB_TAGGING_COL_PATH, newtagname,
                                    DT_LIB_TAGGING_COL_TAG, subtag, -1);
      }
    }
    return TRUE;
  }
  return FALSE;
}

static gboolean update_tag_name_per_name(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, dt_tag_op_t *to)
{
  char *tagname;
  char *newtagname = to->newtagname;
  char *oldtagname = to->oldtagname;
  gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_PATH, &tagname, -1);
  if (g_strcmp0(tagname, oldtagname) == 0)
  {
    char *subtag = g_strrstr(to->newtagname, "|");
    subtag = (!subtag) ? newtagname : subtag + 1;
    gtk_tree_store_set(GTK_TREE_STORE(model), iter, DT_LIB_TAGGING_COL_PATH, newtagname,
                                DT_LIB_TAGGING_COL_TAG, subtag, -1);
    g_free(tagname);
    return TRUE;
  }
  g_free(tagname);
  return FALSE;
}

static void attach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)
     && !gtk_tree_model_get_iter_first(model, &iter))
    return;
  guint tagid;
  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_ID, &tagid, -1);

  int imgsel = -1;
  if(tagid <= 0) return;

  imgsel = dt_view_get_image_to_act_on();
  dt_tag_attach_from_gui(tagid, imgsel);

  init_treeview(self, 0);
  sort_current_list(self);
  if (!d->suggestion_flag)
  {
    const uint32_t count = dt_tag_images_count(tagid);
    if (!d->tree_flag)
    {
      gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_LIB_TAGGING_COL_COUNT, count,
                DT_LIB_TAGGING_COL_SEL, 2, -1);
    }
    else
    {
      gtk_tree_store_set(GTK_TREE_STORE(model), &iter, DT_LIB_TAGGING_COL_COUNT, count,
                DT_LIB_TAGGING_COL_SEL, 2, -1);
      propagate_sel_to_parents(model, &iter);
    }
  }
  else
  {
    init_treeview(self, 1);
  }
  raise_signal_tag_changed(self);
  dt_image_synch_xmp(imgsel);
}

static void detach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->current;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  guint tagid;
  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_ID, &tagid, -1);

  int imgsel = -1;
  if(tagid <= 0) return;

  imgsel = dt_view_get_image_to_act_on();
  GList *affected_images = dt_tag_get_images_from_selection(imgsel, tagid);

  dt_tag_detach(tagid, imgsel);

  init_treeview(self, 0);
  sort_current_list(self);
  if (!d->suggestion_flag)
  {
    const guint count = dt_tag_images_count(tagid);
    view = d->related;
    model = gtk_tree_view_get_model(view);
    gtk_tree_model_get_iter_first(model, &iter);
    if (find_tag_iter_tagid(model, &iter, tagid))
    {
      if (!d->tree_flag)
      {
        gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_LIB_TAGGING_COL_COUNT, count,
                                DT_LIB_TAGGING_COL_SEL, 0, -1);
      }
      else
      {
        gtk_tree_store_set(GTK_TREE_STORE(model), &iter, DT_LIB_TAGGING_COL_COUNT, count,
                                DT_LIB_TAGGING_COL_SEL, 0, -1);
        calculate_sel_on_tree(model, &iter);
      }
    }
  }
  else
  {
    init_treeview(self, 1);
  }
  raise_signal_tag_changed(self);

  // we have to check the conf option as dt_image_synch_xmp() doesn't when called for a single image
  if(dt_conf_get_bool("write_sidecar_files"))
  {
    for(GList *image_iter = affected_images; image_iter; image_iter = g_list_next(image_iter))
    {
      int imgid = GPOINTER_TO_INT(image_iter->data);
      dt_image_synch_xmp(imgid);
    }
  }
  g_list_free(affected_images);
}

static void detach_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  detach_selected_tag(self, d);
}

static void attach_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  attach_selected_tag(self, d);
}

static void detach_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  detach_selected_tag(self, d);
}

static void new_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  const gchar *tag = gtk_entry_get_text(d->entry);
  if(!tag || tag[0] == '\0') return;

  /** attach tag to selected images  */
  dt_tag_attach_string_list(tag, -1);
  dt_image_synch_xmp(-1);

  /** clear input box */
  gtk_entry_set_text(d->entry, "");

  init_treeview(self, 0);
  sort_current_list(self);
  init_treeview(self, 1);
  sort_related_list(self);
  raise_signal_tag_changed(self);
}

static void entry_activated(GtkButton *button, dt_lib_module_t *self)
{
  new_button_clicked(NULL, self);
}

static void tag_name_changed(GtkEntry *entry, dt_lib_module_t *self)
{
  set_keyword(self);
  init_treeview(self, 1);
  sort_related_list(self);
}

static void view_popup_menu_delete_tag(GtkWidget *menuitem, dt_lib_module_t *self, gboolean branch)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  int res = GTK_RESPONSE_YES;

  char *tagname;
  gint tagid;
  gchar *text;
  GtkWidget *label;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &tagname,
          DT_LIB_TAGGING_COL_ID, &tagid, -1);
  if (!tagid) return;
  guint img_count = dt_tag_remove(tagid, FALSE);

  if (img_count > 0) //&& dt_conf_get_bool("plugins/lighttable/tagging/ask_before_delete_tag"))
  {
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    GtkWidget *dialog = gtk_dialog_new_with_buttons(_("delete tag?"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                  _("delete"), GTK_RESPONSE_YES, _("cancel"), GTK_RESPONSE_NONE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(area), vbox);
    text = g_strdup_printf(_("tag: %s "), tagname);
    label = gtk_label_new(text);
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, TRUE, 0);
    g_free(text);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);
    text = g_strdup_printf(ngettext("do you really want to delete the tag `%s'?\n%d image is assigned this tag!",
             "do you really want to delete the tag `%s'?\n%d images are assigned this tag!", img_count), tagname, img_count);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), text);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
    g_free(text);

  #ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
  #endif
    gtk_widget_show_all(dialog);

    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }
  if(res != GTK_RESPONSE_YES)
  {
    g_free(tagname);
    return;
  }

  GList *tagged_images = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.tagged_images WHERE tagid=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    tagged_images = g_list_append(tagged_images, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  }
  sqlite3_finalize(stmt);

  // dt_tag_remove raises DT_SIGNAL_TAG_CHANGED. We don't want to reintialize the tree
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
  dt_tag_remove(tagid, TRUE);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
  dt_control_log(_("tag %s removed"), tagname);

  delete_tree_tag(model, &iter, d->tree_flag);

  GList *list_iter;
  if((list_iter = g_list_first(tagged_images)) != NULL)
  {
    do
    {
      dt_image_synch_xmp(GPOINTER_TO_INT(list_iter->data));
    } while((list_iter = g_list_next(list_iter)) != NULL);
  }
  g_list_free(tagged_images);
  g_free(tagname);
  raise_signal_tag_changed(self);
}

static void view_popup_menu_delete_path(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  int res = GTK_RESPONSE_YES;

  char *tagname;
  gint tagid;
  gchar *text;
  GtkWidget *label;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &tagname,
          DT_LIB_TAGGING_COL_ID, &tagid, -1);

  gint tag_count = 0;
  gint img_count = 0;
  dt_tag_count_tags_images(tagname, &tag_count, &img_count);
  if (tag_count == 0) return;

  if(img_count > 0) // && dt_conf_get_bool("plugins/lighttable/tagging/ask_before_delete_tag"))
  {
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    GtkWidget *dialog = gtk_dialog_new_with_buttons( _("delete branch"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                  _("delete"), GTK_RESPONSE_YES, _("cancel"), GTK_RESPONSE_NONE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(area), vbox);
    text = g_strdup_printf(_("tag: %s "), tagname);
    label = gtk_label_new(text);
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, TRUE, 0);
    g_free(text);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);
    text = g_strdup_printf(ngettext("<u>%d</u> tag will be deleted.", "<u>%d</u> tags will be deleted.", tag_count), tag_count);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), text);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
    g_free(text);
    text = g_strdup_printf(ngettext("<u>%d</u> image will be updated", "<u>%d</u> images will be updated ", img_count), img_count);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), text);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
    g_free(text);

  #ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
  #endif
    gtk_widget_show_all(dialog);

    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }
  if (res != GTK_RESPONSE_YES)
  {
    g_free(tagname);
    return;
  }

  GList *tag_family = NULL;
  GList *tagged_images = NULL;
  dt_tag_get_tags_images(tagname, &tag_family, &tagged_images);

  // dt_tag_remove raises DT_SIGNAL_TAG_CHANGED. We don't want to reintialize the tree
  for (GList *taglist = tag_family; taglist ; taglist = g_list_next(taglist))
  {
    dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
    dt_tag_remove(((dt_tag_t *)taglist->data)->id, TRUE);
    dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
    dt_control_log(_("tag %s removed"), ((dt_tag_t *)taglist->data)->tag);
  }

  g_free(tagname);

  delete_tree_path(model, &iter, TRUE, d->tree_flag);

  GList *list_iter;
  if((list_iter = g_list_first(tagged_images)) != NULL)
  {
    do
    {
      dt_image_synch_xmp(GPOINTER_TO_INT(list_iter->data));
    } while((list_iter = g_list_next(list_iter)) != NULL);
  }
  dt_tag_free_result(&tag_family);
  g_list_free(tagged_images);
  raise_signal_tag_changed(self);
}

// edit tag allows the user to rename a single tag, which can be an element of the hierarchy and change other parameters
static void view_popup_menu_edit_tag(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  char *tagname;
  gint tagid;
  gchar *text;
  GtkWidget *label;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &tagname,
          DT_LIB_TAGGING_COL_ID, &tagid, -1);
  char *subtag = g_strrstr(tagname, "|");

  gint tag_count;
  gint img_count;
  dt_tag_count_tags_images(tagname, &tag_count, &img_count);
  if (tag_count == 0) return;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("edit tag"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("save"), GTK_RESPONSE_YES, _("cancel"), GTK_RESPONSE_NONE, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(area), vbox);
  text = g_strdup_printf(_("tag: %s "), tagname);
  label = gtk_label_new(text);
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, TRUE, 0);
  g_free(text);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);
  text = g_strdup_printf(ngettext("<u>%d</u> tag will be updated.", "<u>%d</u> tags will be updated.", tag_count), tag_count);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  g_free(text);
  text = g_strdup_printf(ngettext("<u>%d</u> image will be updated", "<u>%d</u> images will be updated ", img_count), img_count);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  g_free(text);

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);
  label = gtk_label_new(_("name: "));
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(entry), subtag ? subtag + 1 : tagname);
  gtk_box_pack_end(GTK_BOX(box), entry, TRUE, TRUE, 0);

  gint flags = 0;
  char *synonyms_list = NULL;
  GtkWidget *helper;
  GtkWidget *private;
  GtkTextBuffer *buffer = NULL;
  if (tagid)
  {
    GtkWidget *vbox2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), vbox2, FALSE, TRUE, 0);
    flags = dt_tag_get_flags(tagid);
    helper = gtk_check_button_new_with_label(_("helper"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(helper), flags & DT_TF_CATEGORY);
    gtk_box_pack_end(GTK_BOX(vbox2), helper, FALSE, TRUE, 0);
    private = gtk_check_button_new_with_label(_("private"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(private), flags & DT_TF_PRIVATE);
    gtk_box_pack_end(GTK_BOX(vbox2), private, FALSE, TRUE, 0);

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_end(GTK_BOX(vbox), box, TRUE, TRUE, 0);
    label = gtk_label_new(_("synonyms: "));
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
    GtkWidget *synonyms = gtk_text_view_new();
//    gtk_widget_set_size_request(synonyms, -1, DT_PIXEL_APPLY_DPI(50));
    gtk_box_pack_end(GTK_BOX(box), synonyms, TRUE, TRUE, 0);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(synonyms), GTK_WRAP_WORD);
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(synonyms));
    synonyms_list = dt_tag_get_synonyms(tagid);
    if (synonyms_list) gtk_text_buffer_set_text(buffer, synonyms_list, -1);
    }

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
    const char *newtag = gtk_entry_get_text(GTK_ENTRY(entry));
    if (g_strcmp0(newtag, subtag ? subtag + 1 : tagname) != 0)
    {
      char *message = NULL;
      if (!newtag[0])
        message = _("empty tag is not allowed, aborting");
      if(strchr(newtag, '|') != 0)
        message = _("'|' character is not allowed for renaming tag.\nto modify the hierachy use rename path instead. Aborting.");
      if (message)
      {
        GtkWidget *warning_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
                        GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", message);
        gtk_dialog_run(GTK_DIALOG(warning_dialog));
        gtk_widget_destroy(warning_dialog);
        gtk_widget_destroy(dialog);
        g_free(tagname);
        return;
      }

      GList *tag_family = NULL;
      GList *tagged_images = NULL;
      dt_tag_get_tags_images(tagname, &tag_family, &tagged_images);

      const int tagname_len = strlen(tagname);
      char *new_prefix_tag;
      if (subtag)
      {
        const char letter = tagname[tagname_len - strlen(subtag) + 1];
        tagname[tagname_len - strlen(subtag) + 1] = '\0';
        new_prefix_tag = g_strconcat(tagname, newtag, NULL);
        tagname[tagname_len - strlen(subtag) + 1] = letter;
      }
      else
        new_prefix_tag = (char *)newtag;

      // check if one of the new tagnames already exists.
      gboolean tagname_exists = FALSE;
      for (GList *taglist = tag_family; taglist && !tagname_exists; taglist = g_list_next(taglist))
      {
        char *new_tagname = g_strconcat(new_prefix_tag, &((dt_tag_t *)taglist->data)->tag[tagname_len], NULL);
        tagname_exists = dt_tag_exists(new_tagname, NULL);
        if (tagname_exists)
        {
          GtkWidget *warning_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
                          GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                          _("at least one new tag name (%s) already exists, aborting"), new_tagname);
          gtk_dialog_run(GTK_DIALOG(warning_dialog));
          gtk_widget_destroy(warning_dialog);
        };
        g_free(new_tagname);
      }

      if (!tagname_exists)
      {
        dt_tag_op_t *to = g_malloc(sizeof(dt_tag_op_t));
        to->tree_flag = d->tree_flag;
        to->suggestion_flag = d->suggestion_flag;
        for (GList *taglist = tag_family; taglist; taglist = g_list_next(taglist))
        {
          char *new_tagname = g_strconcat(new_prefix_tag, &((dt_tag_t *)taglist->data)->tag[tagname_len], NULL);
          dt_tag_rename(((dt_tag_t *)taglist->data)->id, new_tagname);
          // when possible refresh the tree to not collapse it
          if (!d->suggestion_flag)
          {
            to->tagid = ((dt_tag_t *)taglist->data)->id;
            to->newtagname = new_tagname;
            gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)update_tag_name_per_id, to);
          }
          g_free(new_tagname);
        }
        if (!tagid && !d->suggestion_flag && d->tree_flag) // the node is not a tag. must be refreshed too.
        {
          to->oldtagname = tagname;
          to->newtagname = new_prefix_tag;
          gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)update_tag_name_per_name, to);
        }
        if (subtag) g_free(new_prefix_tag);
        g_free(to);

        if(dt_conf_get_bool("write_sidecar_files"))
        {
          for (GList *imagelist = tagged_images; imagelist; imagelist = g_list_next(imagelist))
          {
            dt_image_synch_xmp(GPOINTER_TO_INT(imagelist->data));
          }
        }
        raise_signal_tag_changed(self);
      }
      dt_tag_free_result(&tag_family);
      g_list_free(tagged_images);
    }

    if (tagid)
    {
      gint new_flags = ((gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(helper)) ? DT_TF_CATEGORY : 0) |
                      (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(private)) ? DT_TF_PRIVATE : 0));
      GtkTextIter start, end;
      gtk_text_buffer_get_start_iter(buffer, &start);
      gtk_text_buffer_get_end_iter(buffer, &end);
      gchar *new_synonyms_list = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
      if (new_flags != flags)
      {
        dt_tag_set_flags(tagid, new_flags);
        if (!d->tree_flag)
          gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_LIB_TAGGING_COL_FLAGS, new_flags, -1);
        else
          gtk_tree_store_set(GTK_TREE_STORE(model), &iter, DT_LIB_TAGGING_COL_FLAGS, new_flags, -1);
      }
      if (new_synonyms_list && g_strcmp0(synonyms_list, new_synonyms_list) != 0)
        dt_tag_set_synonyms(tagid, new_synonyms_list);
      g_free(new_synonyms_list);
    }
  }
  init_treeview(self, 0);
  gtk_widget_destroy(dialog);
  g_free(tagname);
}

// rename path allows the user to redefine a hierarchy
static void view_popup_menu_rename_path(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  char *tagname;
  gint tagid;
  gchar *text;
  GtkWidget *label;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &tagname,
          DT_LIB_TAGGING_COL_ID, &tagid, -1);

  gint tag_count;
  gint img_count;
  dt_tag_count_tags_images(tagname, &tag_count, &img_count);
  if (tag_count == 0) return;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("rename path?"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("rename"), GTK_RESPONSE_YES, _("cancel"), GTK_RESPONSE_NONE, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(area), vbox);
  text = g_strdup_printf(_("selected path: %s "), tagname);
  label = gtk_label_new(text);
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, TRUE, 0);
  g_free(text);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);
  text = g_strdup_printf(ngettext("<u>%d</u> tag will be updated.", "<u>%d</u> tags will be updated.", tag_count), tag_count);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  g_free(text);
  text = g_strdup_printf(ngettext("<u>%d</u> image will be updated", "<u>%d</u> images will be updated ", img_count), img_count);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  g_free(text);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(entry), tagname);
  gtk_box_pack_start(GTK_BOX(vbox), entry, FALSE, TRUE, 0);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
    const char *newtag = gtk_entry_get_text(GTK_ENTRY(entry));
    if (g_strcmp0(newtag, tagname) == 0)
      return;  // no change
    char *message = NULL;
    if (!newtag[0])
      message = _("empty tag is not allowed, aborting");
    if (strchr(newtag, '|') == &newtag[0] || strchr(newtag, '|') == &newtag[strlen(newtag)-1] || strstr(newtag, "||"))
      message = _("'|' misplaced, empty tag is not allowed, aborting");
    if (message)
    {
      GtkWidget *warning_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
                      GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", message);
      gtk_dialog_run(GTK_DIALOG(warning_dialog));
      gtk_widget_destroy(warning_dialog);
      gtk_widget_destroy(dialog);
      g_free(tagname);
      return;
    }
    GList *tag_family = NULL;
    GList *tagged_images = NULL;
    dt_tag_get_tags_images(tagname, &tag_family, &tagged_images);

    // check if one of the new tagnames already exists.
    const int tagname_len = strlen(tagname);
    gboolean tagname_exists = FALSE;
    for (GList *taglist = tag_family; taglist && !tagname_exists; taglist = g_list_next(taglist))
    {
      char *new_tagname = g_strconcat(newtag, &((dt_tag_t *)taglist->data)->tag[tagname_len], NULL);
      tagname_exists = dt_tag_exists(new_tagname, NULL);
      if (tagname_exists)
      {
        GtkWidget *warning_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
                        GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                        _("at least one new tagname (%s) already exists, aborting."), new_tagname);
        gtk_dialog_run(GTK_DIALOG(warning_dialog));
        gtk_widget_destroy(warning_dialog);
      };
      g_free(new_tagname);
    }

    if (!tagname_exists)
    {
      for (GList *taglist = tag_family; taglist; taglist = g_list_next(taglist))
      {
        char *new_tagname = g_strconcat(newtag, &((dt_tag_t *)taglist->data)->tag[tagname_len], NULL);
        dt_tag_rename(((dt_tag_t *)taglist->data)->id, new_tagname);
        g_free(new_tagname);
      }

      init_treeview(self, 0);
//      init_treeview(self, 1);

      if(dt_conf_get_bool("write_sidecar_files"))
      {
        for (GList *imagelist = tagged_images; imagelist; imagelist = g_list_next(imagelist))
        {
          dt_image_synch_xmp(GPOINTER_TO_INT(imagelist->data));
        }
      }
      raise_signal_tag_changed(self);
    }
    dt_tag_free_result(&tag_family);
    g_list_free(tagged_images);
  }
  gtk_widget_destroy(dialog);
  g_free(tagname);
}

static void view_popup_menu_copy_tag(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->related));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->related));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    char *tag;
    gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &tag, -1);
    gtk_entry_set_text(d->entry, tag);
    g_free(tag);
    gtk_entry_grab_focus_without_selecting(d->entry);
  }
}

static void view_popup_menu(GtkWidget *treeview, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GtkWidget *menu, *menuitem;

  menu = gtk_menu_new();

  menuitem = gtk_menu_item_new_with_label(_("copy to entry"));
  g_signal_connect(menuitem, "activate", (GCallback)view_popup_menu_copy_tag, self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

  if (!d->suggestion_flag)
  {
    menuitem = gtk_menu_item_new_with_label(_("delete tag"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    g_signal_connect(menuitem, "activate", (GCallback)view_popup_menu_delete_tag, self);
  }

  if (!d->suggestion_flag)
  {
    menuitem = gtk_menu_item_new_with_label(_("delete branch"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    g_signal_connect(menuitem, "activate", (GCallback)view_popup_menu_delete_path, self);
  }

  if (!d->suggestion_flag)
  {
    menuitem = gtk_menu_item_new_with_label(_("edit tag..."));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    g_signal_connect(menuitem, "activate", (GCallback)view_popup_menu_edit_tag, self);
  }

  if (d->tree_flag)
  {
    menuitem = gtk_menu_item_new_with_label(_("rename path..."));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    g_signal_connect(menuitem, "activate", (GCallback)view_popup_menu_rename_path, self);
  }

  gtk_widget_show_all(GTK_WIDGET(menu));

#if GTK_CHECK_VERSION(3, 22, 0)
  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
#else
  /* Note: event can be NULL here when called from view_onPopupMenu;
   *  gdk_event_get_time() accepts a NULL argument */
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, (event != NULL) ? event->button : 0,
                 gdk_event_get_time((GdkEvent *)event));
#endif
}

static gboolean view_onButtonPressed(GtkWidget *treeview, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  if((event->type == GDK_BUTTON_PRESS && event->button == 3)
    || (event->type == GDK_2BUTTON_PRESS && event->button == 1))
  {
    GtkTreeView *view = d->related;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    GtkTreePath *path = NULL;
    // Get tree path for row that was clicked
    if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL))
    {
      gtk_tree_selection_select_path(selection, path);
      if(event->type == GDK_BUTTON_PRESS && event->button == 3)
      {
        view_popup_menu(treeview, event, self);
        gtk_tree_path_free(path);
        return TRUE;
      }
      else if(event->type == GDK_2BUTTON_PRESS && event->button == 1)
      {
        attach_selected_tag(self, d);
        gtk_tree_path_free(path);
        return TRUE;
      }
    }
    gtk_tree_path_free(path);
  }
  return FALSE;
}

static gboolean mouse_scroll_current(GtkWidget *treeview, GdkEventScroll *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  if (event->state & GDK_CONTROL_MASK)
  {
    gint width, height;
    gtk_widget_get_size_request (GTK_WIDGET(d->currentwindow), &width, &height);
    height = height + 10.0 * event->delta_y;
    height = (height < 100.0) ? 100.0 : (height > 300.0) ? 300.0 : height;
    gtk_widget_set_size_request(GTK_WIDGET(d->currentwindow), -1, DT_PIXEL_APPLY_DPI((gint)height));
    dt_conf_set_int("plugins/lighttable/tagging/heightcurrentwindow", (gint)height);
    return TRUE;
  }
  return FALSE;
}

static gboolean mouse_scroll_related(GtkWidget *treeview, GdkEventScroll *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  if (event->state & GDK_CONTROL_MASK)
  {
    gint width, height;
    gtk_widget_get_size_request (GTK_WIDGET(d->relatedwindow), &width, &height);
    height = height + 10.0 * event->delta_y;
    height = (height < 300.0) ? 300.0 : (height > 500.0) ? 500.0 : height;
    gtk_widget_set_size_request(GTK_WIDGET(d->relatedwindow), -1, DT_PIXEL_APPLY_DPI((gint)height));
    dt_conf_set_int("plugins/lighttable/tagging/heightrelatedwindow", (gint)height);
    return TRUE;
  }
  return FALSE;
}

static gboolean related_tooltip_setup(GtkWidget *treeview, gint x, gint y, gboolean kb_mode,
      GtkTooltip* tooltip, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  gboolean res = FALSE;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  GtkTreePath *path = NULL;
  // Get tree path mouse position
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), x, y, &path, NULL, NULL, NULL))
  {
    GtkTreeModel *model;
    GtkTreeIter iter;
    gtk_tree_selection_select_path(selection, path);
    if (gtk_tree_selection_get_selected(selection, &model, &iter))
    {
      char *tagname;
      guint tagid;
      guint flags;
      gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_ID, &tagid, DT_LIB_TAGGING_COL_TAG, &tagname,
              DT_LIB_TAGGING_COL_FLAGS, &flags, -1);
      if (tagid)
      {
        char *synonyms = dt_tag_get_synonyms(tagid);
        if ((flags & DT_TF_PRIVATE) || (synonyms && synonyms[0]))
        {
          char *text = dt_util_dstrcat(NULL, _("%s"), tagname);
          text = dt_util_dstrcat(text, " %s\n", (flags & DT_TF_PRIVATE) ? _("(private)") : "");
          text = dt_util_dstrcat(text, "synonyms: %s", (synonyms && synonyms[0]) ? synonyms : " - ");
          gtk_tooltip_set_text (tooltip, text);
          g_free(text);
          res = TRUE;
        }
        g_free(synonyms);
      }
      g_free(tagname);
    }
  }
  gtk_tree_path_free(path);

  return res;
}

static void import_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  char *last_dirname = dt_conf_get_string("plugins/lighttable/tagging/last_import_export_location");
  if(!last_dirname || !*last_dirname)
  {
    g_free(last_dirname);
    last_dirname = g_strdup(g_get_home_dir());
  }

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(_("Select a keyword file"), GTK_WINDOW(win),
                                                       GTK_FILE_CHOOSER_ACTION_OPEN,
                                                       _("_cancel"), GTK_RESPONSE_CANCEL,
                                                       _("_import"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_dirname);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    char *dirname = g_path_get_dirname(filename);
    dt_conf_set_string("plugins/lighttable/tagging/last_import_export_location", dirname);
    ssize_t count = dt_tag_import(filename);
    if(count < 0)
      dt_control_log(_("error importing tags"));
    else
      dt_control_log(_("%zd tags imported"), count);
    g_free(filename);
    g_free(dirname);
  }

  g_free(last_dirname);
  gtk_widget_destroy(filechooser);
  init_treeview(self, 1);
  sort_related_list(self);
}

static void export_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  GDateTime *now = g_date_time_new_now_local();
  char *export_filename = g_date_time_format(now, "darktable_tags_%F_%R.txt");
  char *last_dirname = dt_conf_get_string("plugins/lighttable/tagging/last_import_export_location");
  if(!last_dirname || !*last_dirname)
  {
    g_free(last_dirname);
    last_dirname = g_strdup(g_get_home_dir());
  }

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(_("Select file to export to"), GTK_WINDOW(win),
                                                       GTK_FILE_CHOOSER_ACTION_SAVE,
                                                       _("_cancel"), GTK_RESPONSE_CANCEL,
                                                       _("_export"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif
  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(filechooser), TRUE);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_dirname);
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(filechooser), export_filename);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    char *dirname = g_path_get_dirname(filename);
    dt_conf_set_string("plugins/lighttable/tagging/last_import_export_location", dirname);
    const ssize_t count = dt_tag_export(filename);
    if(count < 0)
      dt_control_log(_("error exporting tags"));
    else
      dt_control_log(_("%zd tags exported"), count);
    g_free(filename);
    g_free(dirname);
  }

  g_date_time_unref(now);
  g_free(last_dirname);
  g_free(export_filename);
  gtk_widget_destroy(filechooser);
}

static void update_layout(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->related));

  const gboolean active_s = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->toggle_suggestion_button));
  d->suggestion_flag = !dt_conf_get_bool("plugins/darkroom/tagging/nosuggestion");
  if (active_s != d->suggestion_flag)
  {
    g_signal_handler_block (d->toggle_suggestion_button, d->suggestion_button_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->toggle_suggestion_button), d->suggestion_flag);
    g_signal_handler_unblock (d->toggle_suggestion_button, d->suggestion_button_handler);
  }

  const gboolean active_t = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->toggle_tree_button));
  d->tree_flag = dt_conf_get_bool("plugins/darkroom/tagging/treeview");
  if (active_t != d->tree_flag)
  {
    g_signal_handler_block (d->toggle_tree_button, d->tree_button_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->toggle_tree_button), d->tree_flag);
    g_signal_handler_unblock (d->toggle_tree_button, d->tree_button_handler);
  }

  if (d->tree_flag)
  {
    if (model == GTK_TREE_MODEL(d->related_liststore))
    {
      g_object_ref(model);
      gtk_tree_view_set_model(GTK_TREE_VIEW(d->related), NULL);
      gtk_list_store_clear(GTK_LIST_STORE(model));
      gtk_tree_view_set_model(GTK_TREE_VIEW(d->related), GTK_TREE_MODEL(d->related_treestore));
      g_object_unref(d->related_treestore);
    }
    gtk_widget_set_visible(d->toggle_suggestion_button, FALSE);
  }
  else
  {
    if (model == GTK_TREE_MODEL(d->related_treestore))
    {
      g_object_ref(model);
      gtk_tree_view_set_model(GTK_TREE_VIEW(d->related), NULL);
      gtk_tree_store_clear(GTK_TREE_STORE(model));
      gtk_tree_view_set_model(GTK_TREE_VIEW(d->related), GTK_TREE_MODEL(d->related_liststore));
      g_object_unref(d->related_liststore);
    }
    gtk_widget_set_visible(d->toggle_suggestion_button, TRUE);
  }

  const gboolean active_c = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->toggle_sort_button));
  d->sort_count_flag = dt_conf_get_bool("plugins/darkroom/tagging/listsortedbycount");
  if (active_c != d->sort_count_flag)
  {
    g_signal_handler_block (d->toggle_sort_button, d->sort_button_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->toggle_sort_button), d->sort_count_flag);
    g_signal_handler_unblock (d->toggle_sort_button, d->sort_button_handler);
  }

  const gboolean active_h = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->toggle_hide_button));
  d->hide_path_flag = dt_conf_get_bool("plugins/darkroom/tagging/hidehierarchy");
  if (active_h != d->hide_path_flag)
  {
    g_signal_handler_block (d->toggle_hide_button, d->hide_button_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->toggle_hide_button), d->hide_path_flag);
    g_signal_handler_unblock (d->toggle_hide_button, d->hide_button_handler);
  }
}

static void toggle_suggestion_button_callback(GtkToggleButton *source, dt_lib_module_t *self)
{
  if (dt_conf_get_bool("plugins/darkroom/tagging/nosuggestion"))
  {
    dt_conf_set_bool("plugins/darkroom/tagging/nosuggestion", FALSE);
  }
  else
  {
    dt_conf_set_bool("plugins/darkroom/tagging/nosuggestion", TRUE);
  }
  update_layout(self);
  init_treeview(self, 1);
  sort_related_list(self);
}

static void toggle_tree_button_callback(GtkToggleButton *source, dt_lib_module_t *self)
{
  if (dt_conf_get_bool("plugins/darkroom/tagging/treeview"))
  {
    dt_conf_set_bool("plugins/darkroom/tagging/treeview", FALSE);
  }
  else
  {
    dt_conf_set_bool("plugins/darkroom/tagging/treeview", TRUE);
  }
  update_layout(self);
  init_treeview(self, 1);
  sort_related_list(self);
}

static gint sort_tree_list_func(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  if (d->sort_count_flag)
  {
    guint count_a = 0;
    guint count_b = 0;
    gtk_tree_model_get(model, a, DT_LIB_TAGGING_COL_COUNT, &count_a, -1);
    gtk_tree_model_get(model, b, DT_LIB_TAGGING_COL_COUNT, &count_b, -1);
    return (count_b - count_a);
  }
  else
  {
    if(d->hide_path_flag)
    {
      char *tag_a = 0;
      char *tag_b = 0;
      gtk_tree_model_get(model, a, DT_LIB_TAGGING_COL_TAG, &tag_a, -1);
      gtk_tree_model_get(model, b, DT_LIB_TAGGING_COL_TAG, &tag_b, -1);
      const gboolean sort = g_ascii_strcasecmp(tag_a, tag_b);
      g_free(tag_a);
      g_free(tag_b);
      return sort;
    }
    else
    {
      char *tag_a = 0;
      char *tag_b = 0;
      gtk_tree_model_get(model, a, DT_LIB_TAGGING_COL_PATH, &tag_a, -1);
      gtk_tree_model_get(model, b, DT_LIB_TAGGING_COL_PATH, &tag_b, -1);
      for(char *letter = tag_a; *letter; letter++)
        if(*letter == '|') *letter = '\1';
      for(char *letter = tag_b; *letter; letter++)
        if(*letter == '|') *letter = '\1';
      const gboolean sort = g_ascii_strcasecmp(tag_a, tag_b);
      g_free(tag_a);
      g_free(tag_b);
      return sort;
    }
  }
}

static void toggle_sort_button_callback(GtkToggleButton *source, dt_lib_module_t *self)
{
  if (dt_conf_get_bool("plugins/darkroom/tagging/listsortedbycount"))
  {
    dt_conf_set_bool("plugins/darkroom/tagging/listsortedbycount", FALSE);
  }
  else
  {
    dt_conf_set_bool("plugins/darkroom/tagging/listsortedbycount", TRUE);
  }
  update_layout(self);
  sort_current_list(self);
  sort_related_list(self);
}

static void toggle_hide_button_callback(GtkToggleButton *source, dt_lib_module_t *self)
{
  if (dt_conf_get_bool("plugins/darkroom/tagging/hidehierarchy"))
  {
    dt_conf_set_bool("plugins/darkroom/tagging/hidehierarchy", FALSE);
  }
  else
  {
    dt_conf_set_bool("plugins/darkroom/tagging/hidehierarchy", TRUE);
  }
  update_layout(self);
  sort_current_list(self);
  sort_related_list(self);
}

static void toggle_dttags_button_callback(GtkToggleButton *source, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  d->dttags_flag = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->toggle_dttags_button));
  init_treeview(self, 0);
  sort_current_list(self);
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  // clear entry box and query
  gtk_entry_set_text(d->entry, "");
  set_keyword(self);
  init_treeview(self, 1);
  sort_related_list(self);
}

int position()
{
  return 500;
}

void gui_init(dt_lib_module_t *self)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("init\n");
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)malloc(sizeof(dt_lib_tagging_t));
  self->data = (void *)d;
  d->imgsel = -1;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  GtkBox *box, *hbox;
  GtkWidget *button;
  GtkWidget *w;
  GtkListStore *liststore;
  GtkTreeStore *treestore;
  GtkTreeViewColumn *col;
  GtkCellRenderer *renderer;
  gint height;

  // left side, current
  box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
  w = gtk_scrolled_window_new(NULL, NULL);
  d->currentwindow = w;
  height = dt_conf_get_int("plugins/lighttable/tagging/heightcurrentwindow");
  gtk_widget_set_size_request(w, -1, DT_PIXEL_APPLY_DPI(height ? height : 100));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  d->current = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_tree_view_set_headers_visible(d->current, FALSE);
  liststore = gtk_list_store_new(DT_LIB_TAGGING_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT,
                                G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_PATH_ID,
                  (GtkTreeIterCompareFunc)sort_tree_list_func, self, NULL);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_NAME_ID,
                  (GtkTreeIterCompareFunc)sort_tree_list_func, self, NULL);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_COUNT_ID,
                  (GtkTreeIterCompareFunc)sort_tree_list_func, self, NULL);
  d->current_liststore = liststore;

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(d->current, col);
  renderer = gtk_cell_renderer_toggle_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, tree_select_show, NULL, NULL);
  g_object_set(renderer, "indicator-size", 10, NULL);  // too big by default

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(d->current, col);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, tree_tagname_show_current, (gpointer)self, NULL);

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(d->current), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(d->current, GTK_TREE_MODEL(liststore));
  g_object_unref(liststore);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->current), _("attached tags,\ndouble-click to detach"
                                                        "\nCtrl-wheel scroll to resize the window"));
  dt_gui_add_help_link(GTK_WIDGET(d->current), "tagging.html#tagging_usage");
  g_signal_connect(G_OBJECT(d->current), "row-activated", G_CALLBACK(detach_activated), (gpointer)self);
  g_signal_connect(G_OBJECT(d->current), "scroll-event", G_CALLBACK(mouse_scroll_current), (gpointer)self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(d->current));

  // attach/detach buttons
  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  button = gtk_button_new_with_label(_("attach"));
  d->attach_button = button;
  gtk_widget_set_tooltip_text(button, _("attach tag to all selected images"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(attach_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("detach"));
  d->detach_button = button;
  gtk_widget_set_tooltip_text(button, _("detach tag from all selected images"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(detach_button_clicked), (gpointer)self);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);

  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_minus_simple, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  d->toggle_hide_button = button;
  gtk_widget_set_tooltip_text(button, _("toggle list with / without hierarchy"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_end(hbox, button, FALSE, TRUE, 0);
  d->hide_button_handler = g_signal_connect(G_OBJECT(button), "clicked",
                                            G_CALLBACK(toggle_hide_button_callback), (gpointer)self);

  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_sorting, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  d->toggle_sort_button = button;
  gtk_widget_set_tooltip_text(button, _("toggle sort by name or by count"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_end(hbox, button, FALSE, TRUE, 0);
  d->sort_button_handler = g_signal_connect(G_OBJECT(button), "clicked",
                                            G_CALLBACK(toggle_sort_button_callback), (gpointer)self);

  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_check_mark, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  d->toggle_dttags_button = button;
  gtk_widget_set_tooltip_text(button, _("toggle show or not darktable tags"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_end(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(toggle_dttags_button_callback), (gpointer)self);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // right side, related
  box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);

  // text entry and new button
  w = gtk_entry_new();
  gtk_widget_set_tooltip_text(w, _("enter tag name"));
  dt_gui_add_help_link(w, "tagging.html#tagging_usage");
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  gtk_widget_add_events(GTK_WIDGET(w), GDK_KEY_RELEASE_MASK);
  // g_signal_connect(G_OBJECT(w), "key-release-event",
  g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(tag_name_changed), (gpointer)self);
  g_signal_connect(G_OBJECT(w), "activate", G_CALLBACK(entry_activated), (gpointer)self);
  d->entry = GTK_ENTRY(w);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->entry));

  // related tree view
  w = gtk_scrolled_window_new(NULL, NULL);
  d->relatedwindow = w;
  height = dt_conf_get_int("plugins/lighttable/tagging/heightrelatedwindow");
  gtk_widget_set_size_request(w, -1, DT_PIXEL_APPLY_DPI(height ? height : 300));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  d->related = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_tree_view_set_headers_visible(d->related, FALSE);
  liststore = gtk_list_store_new(DT_LIB_TAGGING_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT,
                                G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_PATH_ID,
                  (GtkTreeIterCompareFunc)sort_tree_list_func, self, NULL);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_NAME_ID,
                  (GtkTreeIterCompareFunc)sort_tree_list_func, self, NULL);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_COUNT_ID,
                  (GtkTreeIterCompareFunc)sort_tree_list_func, self, NULL);
  d->related_liststore = liststore;
  treestore = gtk_tree_store_new(DT_LIB_TAGGING_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT,
                                G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);
  d->related_treestore = treestore;

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(d->related, col);
  renderer = gtk_cell_renderer_toggle_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, tree_select_show, NULL, NULL);
  g_object_set(renderer, "indicator-size", 10, NULL);  // too big by default

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(d->related, col);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, tree_tagname_show_related, (gpointer)self, NULL);
  gtk_tree_view_set_expander_column (d->related, col);

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(d->related), GTK_SELECTION_SINGLE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->related), _("related tags,\ndouble-click to attach,"
                                                      "\nright-click for other actions on selected tag,"
                                                      "\nCtrl-wheel scroll to resize the window"));
  dt_gui_add_help_link(GTK_WIDGET(d->related), "tagging.html#tagging_usage");
  g_signal_connect(G_OBJECT(d->related), "button-press-event", G_CALLBACK(view_onButtonPressed), (gpointer)self);
  g_signal_connect(G_OBJECT(d->related), "scroll-event", G_CALLBACK(mouse_scroll_related), (gpointer)self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(d->related));
  gtk_tree_view_set_model(d->related, GTK_TREE_MODEL(liststore));
  g_object_unref(liststore);
  g_object_set(G_OBJECT(d->related), "has-tooltip", TRUE, NULL);
  g_signal_connect(G_OBJECT(d->related), "query-tooltip", G_CALLBACK(related_tooltip_setup), (gpointer)self);

  // buttons
  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  button = gtk_button_new_with_label(_("new"));
  d->new_button = button;
  gtk_widget_set_tooltip_text(button, _("create a new tag with the\nname you entered"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(new_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(C_("verb", "import"));
  d->import_button = button;
  gtk_widget_set_tooltip_text(button, _("import tags from a Lightroom keyword file"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(import_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(C_("verb", "export"));
  d->export_button = button;
  gtk_widget_set_tooltip_text(button, _("export all tags to a Lightroom keyword file"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(export_button_clicked), (gpointer)self);

  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_treelist, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  d->toggle_tree_button = button;
  gtk_widget_set_tooltip_text(button, _("toggle list / tree view"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_end(hbox, button, FALSE, TRUE, 0);
  d->tree_button_handler = g_signal_connect(G_OBJECT(button), "clicked",
                                            G_CALLBACK(toggle_tree_button_callback), (gpointer)self);

  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_plus_simple, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  d->toggle_suggestion_button = button;
  gtk_widget_set_tooltip_text(button, _("toggle list with / without suggestion"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_end(hbox, button, FALSE, TRUE, 0);
  d->suggestion_button_handler = g_signal_connect(G_OBJECT(button), "clicked",
                                            G_CALLBACK(toggle_suggestion_button_callback), (gpointer)self);
  gtk_widget_set_no_show_all(GTK_WIDGET(button), TRUE);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // add entry completion
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, gtk_tree_view_get_model(GTK_TREE_VIEW(d->related)));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_set_completion(d->entry, completion);

  /* connect to mouse over id */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_lib_tagging_redraw_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_TAG_CHANGED,
                            G_CALLBACK(_lib_tagging_tags_changed_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_lib_selection_changed_callback), self);

  update_layout(self);
  init_treeview(self, 0);
  sort_current_list(self);
  set_keyword(self);
  init_treeview(self, 1);
  sort_related_list(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->entry));
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_tagging_redraw_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
  free(self->data);
  self->data = NULL;
}

// http://stackoverflow.com/questions/4631388/transparent-floating-gtkentry
static gboolean _lib_tagging_tag_key_press(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
      gtk_widget_destroy(d->floating_tag_window);
      return TRUE;
    case GDK_KEY_Tab:
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      const gchar *tag = gtk_entry_get_text(GTK_ENTRY(entry));
      // both these functions can deal with -1 for all selected images. no need for extra code in here!
      dt_tag_attach_string_list(tag, d->floating_tag_imgid);
      dt_image_synch_xmp(d->floating_tag_imgid);
      gtk_widget_destroy(d->floating_tag_window);
      dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
      return TRUE;
    }
  }
  return FALSE; /* event not handled */
}

static gboolean _lib_tagging_tag_destroy(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_widget_destroy(GTK_WIDGET(user_data));
  return FALSE;
}

static gboolean _match_selected_func(GtkEntryCompletion *completion, GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data)
{
  char *tag = NULL;
  int column = gtk_entry_completion_get_text_column(completion);

  if(gtk_tree_model_get_column_type(model, column) != G_TYPE_STRING) return TRUE;

  GtkEditable *e = (GtkEditable *)gtk_entry_completion_get_entry(completion);
  if(!GTK_IS_EDITABLE(e))
  {
    return FALSE;
  }

  gtk_tree_model_get(model, iter, column, &tag, -1);

  gint cut_off, cur_pos = gtk_editable_get_position(e);

  gchar *currentText = gtk_editable_get_chars(e, 0, -1);
  const gchar *lastTag = g_strrstr(currentText, ",");
  if(lastTag == NULL)
  {
    cut_off = 0;
  }
  else
  {
    cut_off = (int)(g_utf8_strlen(currentText, -1) - g_utf8_strlen(lastTag, -1))+1;
  }
  free(currentText);

  gtk_editable_delete_text(e, cut_off, cur_pos);
  cur_pos = cut_off;
  gtk_editable_insert_text(e, tag, -1, &cur_pos);
  gtk_editable_set_position(e, cur_pos);
  return TRUE;
}

static gboolean _completion_match_func(GtkEntryCompletion *completion, const gchar *key, GtkTreeIter *iter,
                                       gpointer user_data)
{
  gboolean res = FALSE;

  GtkEditable *e = (GtkEditable *)gtk_entry_completion_get_entry(completion);

  if(!GTK_IS_EDITABLE(e))
  {
    return FALSE;
  }

  gint cur_pos = gtk_editable_get_position(e);
  gboolean onLastTag = (g_strstr_len(&key[cur_pos], -1, ",") == NULL);
  if(!onLastTag)
  {
    return FALSE;
  }

  char *tag = NULL;
  GtkTreeModel *model = gtk_entry_completion_get_model(completion);
  int column = gtk_entry_completion_get_text_column(completion);

  if(gtk_tree_model_get_column_type(model, column) != G_TYPE_STRING) return FALSE;

  gtk_tree_model_get(model, iter, column, &tag, -1);

  const gchar *lastTag = g_strrstr(key, ",");
  if(lastTag != NULL)
  {
    lastTag++;
  }
  else
  {
    lastTag = key;
  }
  if(lastTag[0] == '\0' && key[0] != '\0')
  {
    return FALSE;
  }

  if(tag)
  {
    char *normalized = g_utf8_normalize(tag, -1, G_NORMALIZE_ALL);
    if(normalized)
    {
      char *casefold = g_utf8_casefold(normalized, -1);
      if(casefold)
      {
        res = g_strstr_len(casefold, -1, lastTag) != NULL;
      }
      g_free(casefold);
    }
    g_free(normalized);
    g_free(tag);
  }

  return res;
}

static gboolean _lib_tagging_tag_show(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, dt_lib_module_t *self)
{
  const int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
  int mouse_over_id = -1;

  // the order is:
  // if(zoom == 1) => currently shown image
  // else if(selection not empty) => selected images
  // else if(cursor over image) => hovered image
  // else => return
  if(zoom == 1 || dt_collection_get_selected_count(darktable.collection) == 0)
  {
    mouse_over_id = dt_control_get_mouse_over_id();
    if(mouse_over_id < 0) return TRUE;
  }

  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  d->floating_tag_imgid = mouse_over_id;

  gint x, y;
  gint px, py, w, h;
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *center = dt_ui_center(darktable.gui->ui);
  gdk_window_get_origin(gtk_widget_get_window(center), &px, &py);

  w = gdk_window_get_width(gtk_widget_get_window(center));
  h = gdk_window_get_height(gtk_widget_get_window(center));

  x = px + 0.5 * (w - FLOATING_ENTRY_WIDTH);
  y = py + h - 50;

  /* put the floating box at the mouse pointer */
  //   gint pointerx, pointery;
  //   GdkDevice *device =
  //   gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gtk_widget_get_display(widget)));
  //   gdk_window_get_device_position (gtk_widget_get_window (widget), device, &pointerx, &pointery, NULL);
  //   x = px + pointerx + 1;
  //   y = py + pointery + 1;

  d->floating_tag_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->floating_tag_window);
#endif
  /* stackoverflow.com/questions/1925568/how-to-give-keyboard-focus-to-a-pop-up-gtk-window */
  gtk_widget_set_can_focus(d->floating_tag_window, TRUE);
  gtk_window_set_decorated(GTK_WINDOW(d->floating_tag_window), FALSE);
  gtk_window_set_type_hint(GTK_WINDOW(d->floating_tag_window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  gtk_window_set_transient_for(GTK_WINDOW(d->floating_tag_window), GTK_WINDOW(window));
  gtk_widget_set_opacity(d->floating_tag_window, 0.8);
  gtk_window_move(GTK_WINDOW(d->floating_tag_window), x, y);


  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_size_request(entry, FLOATING_ENTRY_WIDTH, -1);
  gtk_widget_add_events(entry, GDK_FOCUS_CHANGE_MASK);

  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, gtk_tree_view_get_model(GTK_TREE_VIEW(d->related)));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_completion_set_popup_set_width(completion, FALSE);
  g_signal_connect(G_OBJECT(completion), "match-selected", G_CALLBACK(_match_selected_func), self);
  gtk_entry_completion_set_match_func(completion, _completion_match_func, NULL, NULL);
  gtk_entry_set_completion(GTK_ENTRY(entry), completion);

  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
  gtk_container_add(GTK_CONTAINER(d->floating_tag_window), entry);
  g_signal_connect(entry, "focus-out-event", G_CALLBACK(_lib_tagging_tag_destroy), d->floating_tag_window);
  g_signal_connect(entry, "key-press-event", G_CALLBACK(_lib_tagging_tag_key_press), self);

  gtk_widget_show_all(d->floating_tag_window);
  gtk_widget_grab_focus(entry);
  gtk_window_present(GTK_WINDOW(d->floating_tag_window));

  return TRUE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
