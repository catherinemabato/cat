/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 henrik andersson.

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
#include "common/darktable.h"
#include "common/collection.h"
#include "common/tags.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "views/view.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include <gdk/gdkkeysyms.h>
#include <math.h>

#define FLOATING_ENTRY_WIDTH 150

DT_MODULE(1)

static gboolean _lib_tagging_tag_show(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval, GdkModifierType modifier, dt_lib_module_t *self);

typedef struct dt_lib_tagging_t
{
  char keyword[1024];
  GtkEntry *entry;
  GtkTreeView *current, *related;
  int imgsel;

  GtkWidget *attach_button, *detach_button, *new_button, *delete_button;

  GtkWidget *floating_tag_window;
  int floating_tag_imgid;
}
dt_lib_tagging_t;

typedef enum dt_lib_tagging_cols_t
{
  DT_LIB_TAGGING_COL_TAG=0,
  DT_LIB_TAGGING_COL_ID,
  DT_LIB_TAGGING_NUM_COLS
}
dt_lib_tagging_cols_t;

const char*
name ()
{
  return _("tagging");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_MAP | DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "attach"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "detach"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "new"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "delete"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "tag"), GDK_t, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t*)self->data;

  dt_accel_connect_button_lib(self, "attach", d->attach_button);
  dt_accel_connect_button_lib(self, "detach", d->detach_button);
  dt_accel_connect_button_lib(self, "new", d->new_button);
  dt_accel_connect_button_lib(self, "delete", d->delete_button);
  dt_accel_connect_lib(self, "tag", g_cclosure_new(G_CALLBACK(_lib_tagging_tag_show), self, NULL));
}

static void
update (dt_lib_module_t *self, int which)
{
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  GList *tags=NULL;
  uint32_t count;

  if(which == 0) // tags of selected images
  {
    int imgsel = -1;
    DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
    d->imgsel = imgsel;
    count = dt_tag_get_attached(imgsel,&tags);
  }
  else // related tags of typed text
    count = dt_tag_get_suggestions(d->keyword,&tags);

  GtkTreeIter iter;
  GtkTreeView *view;
  if(which == 0) view = d->current;
  else           view = d->related;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
  gtk_list_store_clear(GTK_LIST_STORE(model));

  if( count >0 && tags )
  {
    do
    {
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                          DT_LIB_TAGGING_COL_TAG, ((dt_tag_t*)tags->data)->tag,
                          DT_LIB_TAGGING_COL_ID, ((dt_tag_t*)tags->data)->id,
                          -1);
    }
    while( (tags=g_list_next(tags)) !=NULL );

    // Free result...
    dt_tag_free_result(&tags);
  }



  gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
  g_object_unref(model);
}

static void
set_keyword(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  const gchar *beg = g_strrstr(gtk_entry_get_text(d->entry), ",");
  if(!beg) beg = gtk_entry_get_text(d->entry);
  else
  {
    if(*beg == ',') beg++;
    if(*beg == ' ') beg++;
  }
  sprintf(d->keyword,"%s", beg);
  update (self, 1);
}

static void
attach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter) &&
      !gtk_tree_model_get_iter_first(model, &iter)) return;
  guint tagid;
  gtk_tree_model_get (model, &iter,
                      DT_LIB_TAGGING_COL_ID, &tagid,
                      -1);

  int imgsel = -1;
  if(tagid <= 0) return;

  imgsel = dt_view_get_image_to_act_on();

  dt_tag_attach(tagid,imgsel);
  dt_image_synch_xmp(imgsel);

  dt_collection_hint_message(darktable.collection);
}

static void
detach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->current;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  guint tagid;
  gtk_tree_model_get (model, &iter,
                      DT_LIB_TAGGING_COL_ID, &tagid,
                      -1);

  int imgsel = -1;
  if(tagid <= 0) return;

  imgsel = dt_view_get_image_to_act_on();

  dt_tag_detach(tagid,imgsel);
  dt_image_synch_xmp(imgsel);

  dt_collection_hint_message(darktable.collection);
}

static void
attach_activated (GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  attach_selected_tag(self, d);
  update(self, 0);
}

static void
detach_activated (GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  detach_selected_tag(self, d);
  update(self, 0);
}

static void
attach_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  attach_selected_tag(self, d);
  update(self, 0);
}

static void
detach_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  detach_selected_tag(self, d);
  update(self, 0);
}

static void
new_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  const gchar *tag = gtk_entry_get_text(d->entry);

  /** attach tag to selected images  */
  dt_tag_attach_string_list(tag, -1);
  dt_image_synch_xmp(-1);

  update(self, 1);
  update(self, 0);

  /** clear input box */
  gtk_entry_set_text(d->entry, "");

  dt_collection_hint_message(darktable.collection);
}

static void
entry_activated (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  const gchar *tag = gtk_entry_get_text(d->entry);
  if(!tag || tag[0] == '\0') return;

  /** attach tag to selected images  */
  dt_tag_attach_string_list(tag, -1);
  dt_image_synch_xmp(-1);

  update(self, 1);
  update(self, 0);
  gtk_entry_set_text(d->entry, "");

  dt_collection_hint_message(darktable.collection);
}

static void
tag_name_changed (GtkEntry *entry, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  set_keyword(self, d);
}

static void
delete_button_clicked (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;

  int res = GTK_RESPONSE_YES;

  guint tagid;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->related;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  gtk_tree_model_get (model, &iter,
                      DT_LIB_TAGGING_COL_ID, &tagid,
                      -1);

  // First check how many images are affected by the remove
  int count = dt_tag_remove(tagid,FALSE);
  if( count > 0 && dt_conf_get_bool("plugins/lighttable/tagging/ask_before_delete_tag") )
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    const gchar *tagname=dt_tag_get_name(tagid);
    dialog = gtk_message_dialog_new(GTK_WINDOW(win),
                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                    GTK_MESSAGE_QUESTION,
                                    GTK_BUTTONS_YES_NO,
                                    ngettext("do you really want to delete the tag `%s'?\n%d image is assigned this tag!",
                                        "do you really want to delete the tag `%s'?\n%d images are assigned this tag!", count),
                                    tagname,count);
    gtk_window_set_title(GTK_WINDOW(dialog), _("delete tag?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }
  if(res != GTK_RESPONSE_YES) return;

  GList *tagged_images = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select imgid from tagged_images where tagid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    tagged_images = g_list_append(tagged_images, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  }
  sqlite3_finalize(stmt);

  dt_tag_remove(tagid,TRUE);

  GList *list_iter;
  if((list_iter = g_list_first(tagged_images)) != NULL)
  {
    do
    {
      dt_image_synch_xmp(GPOINTER_TO_INT(list_iter->data));
    }
    while((list_iter=g_list_next(list_iter)) != NULL);
  }
  g_list_free(g_list_first(tagged_images));

  update(self, 0);
  update(self, 1);

  dt_collection_hint_message(darktable.collection);
}

void
gui_reset (dt_lib_module_t *self)
{
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  // clear entry box and query
  gtk_entry_set_text(d->entry, "");
  set_keyword(self, d);
}

int
position ()
{
  return 500;
}

static void _lib_tagging_redraw_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self =(dt_lib_module_t *)user_data;
  dt_lib_tagging_t *d   = (dt_lib_tagging_t *)self->data;
  int imgsel = -1;
  DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
  if(imgsel != d->imgsel) update (self, 0);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)malloc(sizeof(dt_lib_tagging_t));
  self->data = (void *)d;
  d->imgsel = -1;

  self->widget = gtk_vbox_new(TRUE, 5);
  gtk_widget_set_size_request(self->widget,100,-1);

  GtkBox *box, *hbox;
  GtkWidget *button;
  GtkWidget *w;
  GtkListStore *liststore;

  // left side, current
  box = GTK_BOX(gtk_vbox_new(FALSE, 0));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
  w = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  d->current = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_tree_view_set_headers_visible(d->current, FALSE);
  liststore = gtk_list_store_new(DT_LIB_TAGGING_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT);
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(d->current, col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_LIB_TAGGING_COL_TAG);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(d->current),
                              GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(d->current, GTK_TREE_MODEL(liststore));
  g_object_unref(liststore);
  g_object_set(G_OBJECT(d->current), "tooltip-text", _("attached tags,\ndoubleclick to detach"), (char *)NULL);
  g_signal_connect(G_OBJECT (d->current), "row-activated", G_CALLBACK (detach_activated), (gpointer)self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(d->current));

  // attach/detach buttons
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("attach"));
  d->attach_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("attach tag to all selected images"), (char *)NULL);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (attach_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("detach"));
  d->detach_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("detach tag from all selected images"), (char *)NULL);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (detach_button_clicked), (gpointer)self);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // right side, related
  box = GTK_BOX(gtk_vbox_new(FALSE, 5));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 5);

  // text entry and new button
  w = gtk_entry_new();
  g_object_set(G_OBJECT(w), "tooltip-text", _("enter tag name"), (char *)NULL);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  gtk_widget_add_events(GTK_WIDGET(w), GDK_KEY_RELEASE_MASK);
  // g_signal_connect(G_OBJECT(w), "key-release-event",
  g_signal_connect(G_OBJECT(w), "changed",
                   G_CALLBACK(tag_name_changed), (gpointer)self);
  g_signal_connect(G_OBJECT (w), "activate",
                   G_CALLBACK (entry_activated), (gpointer)self);
  d->entry = GTK_ENTRY(w);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->entry));

  // related tree view
  w = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(w,-1,100);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  d->related = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_tree_view_set_headers_visible(d->related, FALSE);
  liststore = gtk_list_store_new(DT_LIB_TAGGING_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT);
  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(d->related, col);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_LIB_TAGGING_COL_TAG);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(d->related),
                              GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(d->related, GTK_TREE_MODEL(liststore));
  g_object_unref(liststore);
  g_object_set(G_OBJECT(d->related), "tooltip-text", _("related tags,\ndoubleclick to attach"), (char *)NULL);
  g_signal_connect(G_OBJECT (d->related), "row-activated", G_CALLBACK (attach_activated), (gpointer)self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(d->related));

  // attach and delete buttons
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("new"));
  d->new_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("create a new tag with the\nname you entered"), (char *)NULL);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (new_button_clicked), (gpointer)self);

  button = gtk_button_new_with_label(_("delete"));
  d->delete_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("delete selected tag"), (char *)NULL);
  gtk_box_pack_start(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT (button), "clicked",
                   G_CALLBACK (delete_button_clicked), (gpointer)self);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // add entry completion
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, gtk_tree_view_get_model(GTK_TREE_VIEW(d->related)));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_set_completion(d->entry, completion);

  /* connect to mouse over id */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE, G_CALLBACK(_lib_tagging_redraw_callback), self);

  set_keyword(self, d);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t*)self->data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->entry));
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_tagging_redraw_callback), self);
  free(self->data);
  self->data = NULL;
}

// http://stackoverflow.com/questions/4631388/transparent-floating-gtkentry
static gboolean
_lib_tagging_tag_key_press(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t*)self->data;
  switch(event->keyval)
  {
    case GDK_Escape:
      gtk_widget_destroy(d->floating_tag_window);
      return TRUE;
    case GDK_Tab:
      return TRUE;
    case GDK_Return:
    case GDK_KP_Enter:
    {
      const gchar *tag = gtk_entry_get_text(GTK_ENTRY(entry));
      /* attach tag to images  */
      if(d->floating_tag_imgid > 0) // just a single image
      {
        dt_tag_attach_string_list(tag, d->floating_tag_imgid);
        dt_image_synch_xmp(d->floating_tag_imgid);
      }
      else // all selected images
      {
        GList *selected_images = g_list_first(dt_collection_get_selected(darktable.collection));
        if(selected_images)
        {
          GList *iter = selected_images;
          do
          {
            int imgid = GPOINTER_TO_INT(iter->data);
            dt_tag_attach_string_list(tag, imgid);
            dt_image_synch_xmp(imgid);
          }
          while( (iter=g_list_next(iter)) !=NULL );
        }
        g_list_free(selected_images);
      }
      update(self, 1);
      update(self, 0);
      gtk_widget_destroy(d->floating_tag_window);
      return TRUE;
    }
  }
  return FALSE; /* event not handled */
}

static gboolean
_lib_tagging_tag_show(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval, GdkModifierType modifier, dt_lib_module_t* self)
{
  int mouse_over_id = -1;
  int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");

  // the order is:
  // if(zoom == 1) => currently shown image
  // else if(selection not empty) => selected images
  // else if(cursor over image) => hovered image
  // else => return
  if(zoom == 1 || dt_collection_get_selected_count(darktable.collection) == 0)
  {
    DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
    if(mouse_over_id < 0)
      return TRUE;
  }

  dt_lib_tagging_t *d = (dt_lib_tagging_t*)self->data;
  d->floating_tag_imgid = mouse_over_id;

  gint x, y;
  gint px, py, w, h;
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *center = dt_ui_center(darktable.gui->ui);
  gdk_window_get_origin(gtk_widget_get_window(center), &px, &py);
  gdk_window_get_size(gtk_widget_get_window(center),&w,&h);
  x = px + 0.5*(w-FLOATING_ENTRY_WIDTH);
  y = py + h - 50;

  /* put the floating box at the mouse pointer */
//   gint pointerx, pointery;
//   gtk_widget_get_pointer(center, &pointerx, &pointery);
//   x = px + pointerx + 1;
//   y = py + pointery + 1;

  d->floating_tag_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  /* stackoverflow.com/questions/1925568/how-to-give-keyboard-focus-to-a-pop-up-gtk-window */
  GTK_WIDGET_SET_FLAGS(d->floating_tag_window, GTK_CAN_FOCUS);
  gtk_window_set_decorated(GTK_WINDOW(d->floating_tag_window), FALSE);
  gtk_window_set_has_frame(GTK_WINDOW(d->floating_tag_window), FALSE);
  gtk_window_set_type_hint(GTK_WINDOW(d->floating_tag_window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  gtk_window_set_transient_for(GTK_WINDOW(d->floating_tag_window), GTK_WINDOW(window));
  gtk_window_set_opacity(GTK_WINDOW(d->floating_tag_window), 0.8);
  gtk_window_move(GTK_WINDOW(d->floating_tag_window), x, y);


  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_size_request(entry, FLOATING_ENTRY_WIDTH, -1);
  gtk_widget_add_events(entry, GDK_FOCUS_CHANGE_MASK);

  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, gtk_tree_view_get_model(GTK_TREE_VIEW(d->related)));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_set_completion(GTK_ENTRY(entry), completion);

  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
  gtk_container_add(GTK_CONTAINER(d->floating_tag_window), entry);
  g_signal_connect_swapped(entry, "focus-out-event", G_CALLBACK(gtk_widget_destroy), d->floating_tag_window);
  g_signal_connect(entry, "key-press-event", G_CALLBACK(_lib_tagging_tag_key_press), self);

  gtk_widget_show_all(d->floating_tag_window);
  gtk_widget_grab_focus(entry);
  gtk_window_present(GTK_WINDOW(d->floating_tag_window));

  return TRUE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
