/*
    This file is part of darktable,
    copyright (c) 2011-2012 Henrik Andersson.

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
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "gui/hist_dialog.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"

#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef enum dt_lib_filmstrip_select_t
{
  DT_LIB_FILMSTRIP_SELECT_NONE,
  DT_LIB_FILMSTRIP_SELECT_SINGLE,
  DT_LIB_FILMSTRIP_SELECT_TOGGLE,
  DT_LIB_FILMSTRIP_SELECT_RANGE
} dt_lib_filmstrip_select_t;

typedef struct dt_lib_filmstrip_t
{
  GtkWidget *filmstrip;

  /* state vars */
  int32_t last_selected_id;
  int32_t mouse_over_id;
  int32_t offset;
  int32_t collection_count;
  int32_t history_copy_imgid;
  gdouble pointerx, pointery;
  dt_view_image_over_t image_over;

  gboolean size_handle_is_dragging;
  gint size_handle_x, size_handle_y;
  int32_t size_handle_height;

  int32_t activated_image;
  dt_lib_filmstrip_select_t select;
  int32_t select_id;

  float thumb_size;
  float offset_x;
  int last_mouse_over_thumb;
  int32_t last_exposed_id;
  gboolean force_expose_all;
  cairo_surface_t *surface;
  GHashTable *thumbs_table;
  int32_t panel_width;
  int32_t panel_height;

} dt_lib_filmstrip_t;

/* proxy function to center filmstrip on imgid */
static void _lib_filmstrip_scroll_to_image(dt_lib_module_t *self, gint imgid, gboolean activate);
/* proxy function for retrieving last activate request image id */
static int32_t _lib_filmstrip_get_activated_imgid(dt_lib_module_t *self);
static GtkWidget *_lib_filmstrip_get_widget(dt_lib_module_t *self);

/*
static gboolean _lib_filmstrip_motion_notify_callback(GtkWidget *w, GdkEventMotion *e, gpointer user_data);

static gboolean _lib_filmstrip_mouse_leave_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data);

static gboolean _lib_filmstrip_scroll_callback(GtkWidget *w, GdkEventScroll *e, gpointer user_data);
*/
static gboolean _lib_filmstrip_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data);
/*
static gboolean _lib_filmstrip_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                     gpointer user_data);

static gboolean _lib_filmstrip_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                       gpointer user_data);

static void _lib_filmstrip_collection_changed_callback(gpointer instance, gpointer user_data);*/

/* drag'n'drop callbacks */
/*static void _lib_filmstrip_dnd_get_callback(GtkWidget *widget, GdkDragContext *context,
                                            GtkSelectionData *selection_data, guint target_type, guint time,
                                            gpointer user_data);
static void _lib_filmstrip_dnd_begin_callback(GtkWidget *widget, GdkDragContext *context, gpointer user_data);*/

const char *name(dt_lib_module_t *self)
{
  return _("filmstrip");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", "darkroom", "tethering", "map", "print", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_BOTTOM;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

static inline gboolean _is_on_lighttable()
{
  // on lighttable, does nothing and report that it has not been handled
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  return cv->view((dt_view_t *)cv) == DT_VIEW_LIGHTTABLE;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_filmstrip_t *d = (dt_lib_filmstrip_t *)calloc(1, sizeof(dt_lib_filmstrip_t));
  self->data = (void *)d;

  d->last_selected_id = -1;
  d->history_copy_imgid = -1;
  d->activated_image = -1;
  d->mouse_over_id = -1;
  d->pointerx = -1;
  d->pointery = -1;
  d->thumb_size = -1;
  d->last_mouse_over_thumb = -1;
  d->last_exposed_id = -1;
  d->force_expose_all = FALSE;
  d->offset_x = 0;
  d->surface = NULL;
  d->panel_width = -1;
  d->panel_height = -1;
  d->thumbs_table = g_hash_table_new(g_int_hash, g_int_equal);

  /* creating drawing area */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* creating filmstrip box*/
  d->filmstrip = gtk_event_box_new();

  /* allow drag&drop of images from the filmstrip. this has to come before the other callbacks are registered!
   */
  /*gtk_drag_source_set(d->filmstrip, GDK_BUTTON1_MASK, target_list_all, n_targets_all, GDK_ACTION_COPY);
#ifdef HAVE_MAP
  gtk_drag_dest_set(d->filmstrip, GTK_DEST_DEFAULT_ALL, target_list_internal, n_targets_internal,
                    GDK_ACTION_COPY);
#endif

  g_signal_connect_after(d->filmstrip, "drag-begin", G_CALLBACK(_lib_filmstrip_dnd_begin_callback), self);
  g_signal_connect(d->filmstrip, "drag-data-get", G_CALLBACK(_lib_filmstrip_dnd_get_callback), self);*/

  gtk_widget_add_events(d->filmstrip, GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                      | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                      | darktable.gui->scroll_mask
                                      | GDK_LEAVE_NOTIFY_MASK);

  /* connect callbacks */
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(_lib_filmstrip_draw_callback), self);
  /*g_signal_connect(G_OBJECT(d->filmstrip), "button-press-event",
                   G_CALLBACK(_lib_filmstrip_button_press_callback), self);
  g_signal_connect(G_OBJECT(d->filmstrip), "button-release-event",
                   G_CALLBACK(_lib_filmstrip_button_release_callback), self);
  g_signal_connect(G_OBJECT(d->filmstrip), "scroll-event", G_CALLBACK(_lib_filmstrip_scroll_callback), self);
  g_signal_connect(G_OBJECT(d->filmstrip), "motion-notify-event",
                   G_CALLBACK(_lib_filmstrip_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(d->filmstrip), "leave-notify-event",
                   G_CALLBACK(_lib_filmstrip_mouse_leave_callback), self);*/

  gtk_box_pack_start(GTK_BOX(self->widget), d->filmstrip, TRUE, TRUE, 0);

  /* initialize view manager proxy */
  darktable.view_manager->proxy.filmstrip.module          = self;
  darktable.view_manager->proxy.filmstrip.scroll_to_image = _lib_filmstrip_scroll_to_image;
  darktable.view_manager->proxy.filmstrip.activated_image = _lib_filmstrip_get_activated_imgid;
  darktable.view_manager->proxy.filmstrip.widget          = _lib_filmstrip_get_widget;

  /* connect signal handler */
  /*dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_lib_filmstrip_collection_changed_callback), (gpointer)self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_lib_filmstrip_collection_changed_callback), (gpointer)self);*/
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  /* unset viewmanager proxy */
  darktable.view_manager->proxy.filmstrip.module = NULL;

  g_hash_table_destroy(strip->thumbs_table);

  /* cleanup */
  free(self->data);
  self->data = NULL;
}


/*static gboolean _lib_filmstrip_mouse_leave_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  dt_control_set_mouse_over_id(strip->activated_image);

  // suppress mouse over highlight upon leave
  strip->pointery = -1;
  gtk_widget_queue_draw(strip->filmstrip);

  return TRUE;
}

static gboolean _lib_filmstrip_motion_notify_callback(GtkWidget *w, GdkEventMotion *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  // no redraw by default
  gboolean do_redraw = FALSE;

  strip->pointerx = e->x;
  strip->pointery = e->y;

  if(strip->thumb_size == -1)
  {
    do_redraw = TRUE;
  }
  else
  {
    // compute the actual thumb number, this is not at all the thumb id, but a count of the thumb starting from the
    // left and continuing to the right. this goal is to filter out as much as possible the redraw event. when we
    // stay on the same thumb we do not redraw, there is nothing to do. the exception is a small border around the
    // thumb to ensure the stars, reject tag and the dev history sensitive area are reacting.
    const double px = e->x - strip->offset_x;
    const double py = e->y;
    const int mouse_over_thumb = (int)(1.0 + px / strip->thumb_size);
    const float x_offset = fmodf(px, strip->thumb_size);
    const float y_offset = fmodf(py, strip->thumb_size);
    const float end_pos = (strip->thumb_size * 85) / 100;
    const float start_pos = (strip->thumb_size * 10) / 100;

    if (strip->last_mouse_over_thumb == -1 || strip->last_mouse_over_thumb != mouse_over_thumb
        || (y_offset > end_pos || y_offset < start_pos)
        || (x_offset > end_pos || x_offset < start_pos))
    {
      strip->last_mouse_over_thumb = mouse_over_thumb;
      do_redraw = TRUE;
    }
  }

  // redraw
  if(do_redraw) gtk_widget_queue_draw(strip->filmstrip);
  return TRUE;
}

static gboolean _lib_filmstrip_scroll_callback(GtkWidget *w, GdkEventScroll *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  // change the offset
  int delta;
  if(dt_gui_get_scroll_unit_delta(e, &delta))
  {
    strip->offset = CLAMP(strip->offset + delta, 0, strip->collection_count-1);
    strip->force_expose_all = TRUE;
    gtk_widget_queue_draw(strip->filmstrip);
  }

  return TRUE;
}*/

/*static gboolean _lib_filmstrip_imgid_in_collection(const dt_collection_t *collection, const int imgid)
{
  sqlite3_stmt *stmt;
  gboolean image_in_collection = TRUE;
  const char *query = dt_collection_get_query(collection);

  char *count_query = g_strdup_printf("SELECT count(id) FROM (%s) WHERE id = ?3", query);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), count_query, -1, &stmt, NULL);
  if((collection->params.query_flags & COLLECTION_QUERY_USE_LIMIT)
     && !(collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT))
  {
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
  }
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
    image_in_collection = (sqlite3_column_int(stmt, 0) > 0);
  sqlite3_finalize(stmt);
  g_free(count_query);
  return image_in_collection;
}*/

/*static gboolean _lib_filmstrip_button_press_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  const int32_t mouse_over_id = strip->mouse_over_id;
  strip->select = DT_LIB_FILMSTRIP_SELECT_NONE;

  if(e->button == 1)
  {
    if(e->type == GDK_BUTTON_PRESS)
    {
      // let check if any thumb controls was clicked
      switch(strip->image_over)
      {
        case DT_VIEW_DESERT:
          // is this an activation of image
          if((e->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) == 0)
            strip->select = DT_LIB_FILMSTRIP_SELECT_SINGLE;
          else if((e->state & (GDK_CONTROL_MASK)) == GDK_CONTROL_MASK)
            strip->select = DT_LIB_FILMSTRIP_SELECT_TOGGLE;
          else if((e->state & (GDK_SHIFT_MASK)) == GDK_SHIFT_MASK)
            strip->select = DT_LIB_FILMSTRIP_SELECT_RANGE;
          if(strip->select != DT_LIB_FILMSTRIP_SELECT_NONE)
          {
            strip->select_id = mouse_over_id;
            return TRUE;
          }
          break;
        case DT_VIEW_REJECT:
        case DT_VIEW_STAR_1:
        case DT_VIEW_STAR_2:
        case DT_VIEW_STAR_3:
        case DT_VIEW_STAR_4:
        case DT_VIEW_STAR_5:
        {
          int offset = 0;
          if(mouse_over_id == strip->activated_image) offset = dt_collection_image_offset(mouse_over_id);

          dt_ratings_apply(mouse_over_id, strip->image_over, TRUE, TRUE, TRUE);

          if(mouse_over_id == strip->activated_image)
            if(_lib_filmstrip_imgid_in_collection(darktable.collection, mouse_over_id) == 0)
              dt_view_filmstrip_scroll_relative(0, offset);

          dt_collection_update_query(darktable.collection); // update the counter and selection
          dt_collection_hint_message(darktable.collection); // More than this, we need to redraw all

          gtk_widget_queue_draw(strip->filmstrip);
          return TRUE;
        }

        default:
          return FALSE;
      }
    }
    else if(e->type == GDK_2BUTTON_PRESS)
    {
      if(mouse_over_id > 0)
      {
        strip->activated_image = mouse_over_id;
        dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE);
        return TRUE;
      }
    }
  }

  return FALSE;
}

static gboolean _lib_filmstrip_button_release_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  const int32_t mouse_over_id = strip->mouse_over_id;
  const int32_t select_id = strip->select_id;
  gboolean result = FALSE;
  strip->force_expose_all = TRUE;

  if(mouse_over_id == select_id && mouse_over_id > 0)
  {
    result = TRUE;
    if(strip->select == DT_LIB_FILMSTRIP_SELECT_SINGLE)
      dt_selection_select_single(darktable.selection, mouse_over_id);
    else if(strip->select == DT_LIB_FILMSTRIP_SELECT_TOGGLE)
      dt_selection_toggle(darktable.selection, mouse_over_id);
    else if(strip->select == DT_LIB_FILMSTRIP_SELECT_RANGE)
      dt_selection_select_range(darktable.selection, mouse_over_id);
    else
      result = FALSE;
  }

  strip->select = DT_LIB_FILMSTRIP_SELECT_NONE;
  strip->select_id = -1;

  // redraw filmstrip
  gtk_widget_queue_draw(strip->filmstrip);
  return result;
}*/

static gboolean _lib_filmstrip_draw_callback(GtkWidget *widget, cairo_t *wcr, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  // we only ensure that the thumbtable is inside our container
  if(!gtk_bin_get_child(GTK_BIN(strip->filmstrip)))
  {
    dt_thumbtable_t *tt = dt_ui_thumbtable(darktable.gui->ui);
    dt_thumbtable_set_parent(tt, strip->filmstrip, DT_THUMBTABLE_MODE_FILMSTRIP);
    gtk_widget_show_all(strip->filmstrip);
    gtk_widget_queue_draw(tt->widget);
  }
  return FALSE;
}

/*static void _lib_filmstrip_collection_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;
  strip->force_expose_all = TRUE;
  dt_control_queue_redraw_widget(strip->filmstrip);
}*/

static void _lib_filmstrip_scroll_to_image(dt_lib_module_t *self, gint imgid, gboolean activate)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  // if no imgid just bail out
  if(imgid <= 0) return;

  strip->activated_image = imgid;

  strip->offset = dt_collection_image_offset(imgid);

  dt_control_set_mouse_over_id(strip->activated_image);

  /* activate the image if requested */
  if(activate)
  {
    strip->activated_image = imgid;
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, imgid);
  }

  // redraw filmstrip. since this is a proxy function it could be used from another thread
  strip->force_expose_all = TRUE;
  dt_control_queue_redraw_widget(self->widget);
}

static int32_t _lib_filmstrip_get_activated_imgid(dt_lib_module_t *self)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;
  return strip->activated_image;
}

static GtkWidget *_lib_filmstrip_get_widget(dt_lib_module_t *self)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;
  return strip->filmstrip;
}

/*static void _lib_filmstrip_dnd_get_callback(GtkWidget *widget, GdkDragContext *context,
                                            GtkSelectionData *selection_data, guint target_type, guint time,
                                            gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  if(_is_on_lighttable()) return;

  g_assert(selection_data != NULL);

  int mouse_over_id = strip->mouse_over_id;
  int count = dt_collection_get_selected_count(NULL);
  switch(target_type)
  {
    case DND_TARGET_IMGID:
    {
      int id = ((count == 1) ? mouse_over_id : -1);
      gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _DWORD,
                             (guchar *)&id, sizeof(id));
      break;
    }
    default:             // return the location of the file as a last resort
    case DND_TARGET_URI: // TODO: add all images from the selection
    {
      if(count == 1)
      {
        gchar pathname[PATH_MAX] = { 0 };
        gboolean from_cache = TRUE;
        dt_image_full_path(mouse_over_id, pathname, sizeof(pathname), &from_cache);
        gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _BYTE,
                               (guchar *)uri, strlen(uri));
        g_free(uri);
      }
      else
      {
        sqlite3_stmt *stmt;
        GList *images = NULL;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT imgid FROM main.selected_images", -1, &stmt, NULL);
        while(sqlite3_step(stmt) == SQLITE_ROW)
        {
          int id = sqlite3_column_int(stmt, 0);
          gchar pathname[PATH_MAX] = { 0 };
          gboolean from_cache = TRUE;
          dt_image_full_path(id, pathname, sizeof(pathname), &from_cache);
          gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
          images = g_list_append(images, uri);
        }
        sqlite3_finalize(stmt);
        gchar *uri_list = dt_util_glist_to_str("\r\n", images);
        g_list_free_full(images, g_free);
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _BYTE,
                               (guchar *)uri_list, strlen(uri_list));
        g_free(uri_list);
      }
      break;
    }
  }
}

static void _lib_filmstrip_dnd_begin_callback(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  const int ts = DT_PIXEL_APPLY_DPI(64);

  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  if(_is_on_lighttable()) return;

  int imgid = strip->mouse_over_id;

  // imgid part of selection -> do nothing
  // otherwise               -> select the current image
  strip->select = DT_LIB_FILMSTRIP_SELECT_NONE;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM main.selected_images WHERE imgid=?1 LIMIT 1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    dt_selection_select_single(darktable.selection, imgid);
    // redraw filmstrip
    if(darktable.view_manager->proxy.filmstrip.module)
      gtk_widget_queue_draw(darktable.view_manager->proxy.filmstrip.module->widget);
  }
  sqlite3_finalize(stmt);

  // if we are dragging a single image -> use the thumbnail of that image
  // otherwise use the generic d&d icon
  // TODO: have something pretty in the 2nd case, too.
  if(dt_collection_get_selected_count(NULL) == 1)
  {
    dt_mipmap_buffer_t buf;
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, ts, ts);
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BLOCKING, 'r');

    if(buf.buf)
    {
      for(size_t i = 3; i < (size_t)4 * buf.width * buf.height; i += 4) buf.buf[i] = UINT8_MAX;

      int w = ts, h = ts;
      if(buf.width < buf.height)
        w = (buf.width * ts) / buf.height; // portrait
      else
        h = (buf.height * ts) / buf.width; // landscape

      GdkPixbuf *source = gdk_pixbuf_new_from_data(buf.buf, GDK_COLORSPACE_RGB, TRUE, 8, buf.width,
                                                   buf.height, buf.width * 4, NULL, NULL);
      GdkPixbuf *scaled = gdk_pixbuf_scale_simple(source, w, h, GDK_INTERP_HYPER);
      gtk_drag_set_icon_pixbuf(context, scaled, 0, h);

      if(source) g_object_unref(source);
      if(scaled) g_object_unref(scaled);
    }

    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  }
}*/

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
