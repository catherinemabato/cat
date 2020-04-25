/*
    This file is part of darktable,
    copyright (c) 2020 Aldric Renaudin.

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
/** a class to manage a collection of zoomable thumbnails for culling or full preview.  */
#include "dtgtk/culling.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "control/control.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "views/view.h"

#define FULL_PREVIEW_IN_MEMORY_LIMIT 9

static inline float _absmul(float a, float b)
{
  return a > b ? a / b : b / a;
}
static inline int _get_max_in_memory_images()
{
  const int max_in_memory_images = dt_conf_get_int("plugins/lighttable/preview/max_in_memory_images");
  return MIN(max_in_memory_images, FULL_PREVIEW_IN_MEMORY_LIMIT);
}
// specials functions for GList globals actions
static gint _list_compare_by_imgid(gconstpointer a, gconstpointer b)
{
  dt_thumbnail_t *th = (dt_thumbnail_t *)a;
  const int imgid = GPOINTER_TO_INT(b);
  if(th->imgid < 0 || b < 0) return 1;
  return (th->imgid != imgid);
}
static void _list_remove_thumb(gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(thumb->w_main)), thumb->w_main);
  dt_thumbnail_destroy(thumb);
}

static int _get_selection_count()
{
  int nb = 0;
  gchar *query = dt_util_dstrcat(
      NULL,
      "SELECT count(*) FROM main.selected_images AS s, memory.collected_images as m WHERE s.imgid = m.imgid");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(stmt != NULL)
  {
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      nb = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  g_free(query);

  return nb;
}

// get imgid from rowid
static int _thumb_get_imgid(int rowid)
{
  int id = -1;
  sqlite3_stmt *stmt;
  gchar *query = dt_util_dstrcat(NULL, "SELECT imgid FROM memory.collected_images WHERE rowid=%d", rowid);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
  }
  g_free(query);
  sqlite3_finalize(stmt);
  return id;
}
// get rowid from imgid
static int _thumb_get_rowid(int imgid)
{
  int id = -1;
  sqlite3_stmt *stmt;
  gchar *query = dt_util_dstrcat(NULL, "SELECT rowid FROM memory.collected_images WHERE imgid=%d", imgid);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
  }
  g_free(query);
  sqlite3_finalize(stmt);
  return id;
}

// compute thumb_size, thumbs_per_row and rows for the current widget size
// return TRUE if something as changed (or forced) FALSE otherwise
static gboolean _compute_sizes(dt_culling_t *table, gboolean force)
{
  gboolean ret = FALSE; // return value to show if something as changed
  GtkAllocation allocation;
  gtk_widget_get_allocation(table->widget, &allocation);

  if(allocation.width <= 20 || allocation.height <= 20)
  {
    table->view_width = allocation.width;
    table->view_height = allocation.height;
    return FALSE;
  }

  if(table->mode == DT_CULLING_MODE_CULLING)
  {
    const int npr = dt_view_lighttable_get_zoom(darktable.view_manager);

    if(force || allocation.width != table->view_width || allocation.height != table->view_height
       || npr != table->thumbs_count)
    {
      table->thumbs_count = npr;
      table->view_width = allocation.width;
      table->view_height = allocation.height;
      ret = TRUE;
    }
  }
  else if(table->mode == DT_CULLING_MODE_PREVIEW)
  {
    if(force || allocation.width != table->view_width || allocation.height != table->view_height)
    {
      table->thumbs_count = 1;
      table->view_width = allocation.width;
      table->view_height = allocation.height;
      ret = TRUE;
    }
  }
  return ret;
}

static void _thumbs_move(dt_culling_t *table, int move)
{
  if(move == 0) return;
  int new_offset = table->offset;
  // we sanintize the values to be sure to stay in the allowed collection
  if(move < 0)
  {
    if(table->navigate_inside_selection)
    {
      sqlite3_stmt *stmt;
      gchar *query = dt_util_dstrcat(NULL,
                                     "SELECT m.rowid FROM memory.collected_images as m, main.selected_images as s "
                                     "WHERE m.imgid=s.imgid AND m.rowid<=%d "
                                     "ORDER BY m.rowid DESC LIMIT 1 OFFSET %d",
                                     table->offset, -1 * move);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        new_offset = sqlite3_column_int(stmt, 0);
      }
      else
      {
        // if we are here, that means we don't have enought space to move as wanted. So we move to first position
        g_free(query);
        sqlite3_finalize(stmt);
        query
            = dt_util_dstrcat(NULL, "SELECT m.rowid FROM memory.collected_images as m, main.selected_images as s "
                                    "WHERE m.imgid=s.imgid "
                                    "ORDER BY m.rowid LIMIT 1");
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
        if(sqlite3_step(stmt) == SQLITE_ROW)
        {
          new_offset = sqlite3_column_int(stmt, 0);
        }
      }
      g_free(query);
      sqlite3_finalize(stmt);
      if(new_offset == table->offset)
      {
        dt_control_log(_("you have reached the start of your selection"));
        return;
      }
    }
    else
    {
      new_offset = MAX(1, table->offset + move);
      if(new_offset == table->offset)
      {
        dt_control_log(_("you have reached the start of your collection"));
        return;
      }
    }
  }
  else
  {
    if(table->navigate_inside_selection)
    {
      sqlite3_stmt *stmt;
      gchar *query
          = dt_util_dstrcat(NULL,
                            "SELECT COUNT(m.rowid) FROM memory.collected_images as m, main.selected_images as s "
                            "WHERE m.imgid=s.imgid AND m.rowid>%d",
                            table->offset);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      int nb_after = 0;
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        nb_after = sqlite3_column_int(stmt, 0);
      }
      g_free(query);
      sqlite3_finalize(stmt);

      if(nb_after >= table->thumbs_count)
      {
        const int delta = MIN(nb_after + 1 - table->thumbs_count, move);
        query = dt_util_dstrcat(NULL,
                                "SELECT m.rowid FROM memory.collected_images as m, main.selected_images as s "
                                "WHERE m.imgid=s.imgid AND m.rowid>=%d "
                                "ORDER BY m.rowid LIMIT 1 OFFSET %d",
                                table->offset, delta);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
        if(sqlite3_step(stmt) == SQLITE_ROW)
        {
          new_offset = sqlite3_column_int(stmt, 0);
        }
        g_free(query);
        sqlite3_finalize(stmt);
      }

      if(new_offset == table->offset)
      {
        dt_control_log(_("you have reached the end of your selection"));
        return;
      }
    }
    else
    {
      sqlite3_stmt *stmt;
      gchar *query = dt_util_dstrcat(NULL,
                                     "SELECT COUNT(m.rowid) FROM memory.collected_images as m "
                                     "WHERE m.rowid>%d",
                                     table->offset);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const int nb = sqlite3_column_int(stmt, 0);
        if(nb >= table->thumbs_count)
        {
          new_offset = table->offset + MIN(nb + 1 - table->thumbs_count, move);
        }
      }
      g_free(query);
      sqlite3_finalize(stmt);
      if(new_offset == table->offset)
      {
        dt_control_log(_("you have reached the end of your collection"));
        return;
      }
    }
  }

  if(new_offset != table->offset)
  {
    table->offset = new_offset;
    dt_culling_full_redraw(table, TRUE);
  }
}

static gboolean _thumbs_zoom_add(dt_culling_t *table, float val, double posx, double posy, int state)
{
  const int max_in_memory_images = _get_max_in_memory_images();
  if(table->mode == DT_CULLING_MODE_CULLING && table->thumbs_count > max_in_memory_images)
  {
    dt_control_log(_("zooming is limited to %d images"), max_in_memory_images);
    return TRUE;
  }

  // we get the 100% zoom of the largest image
  float zmax = 1.0f;
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    const float z100 = dt_thumbnail_get_zoom100(th);
    if(z100 > zmax) zmax = z100;
    l = g_list_next(l);
  }

  float nz = fminf(zmax, table->full_zoom + val);
  nz = fmaxf(nz, 1.0f);

  // if full preview, we center the zoom at mouse position
  if(g_list_length(table->list) > 0 && table->full_zoom != nz && table->mode == DT_CULLING_MODE_PREVIEW
     && posx >= 0.0f && posy >= 0.0f)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)g_list_nth_data(table->list, 0);
    // we want to zoom "around" the pointer
    float dx = nz / table->full_zoom
                   * (posx - (table->view_width - th->w_fit * table->full_zoom) * 0.5f - table->full_x)
               - posx + (table->view_width - th->w_fit * nz) * 0.5f;
    float dy = nz / table->full_zoom
                   * (posy - (table->view_height - th->h_fit * table->full_zoom) * 0.5f - table->full_y)
               - posy + (table->view_height - th->h_fit * nz) * 0.5f;
    table->full_x = -dx;
    table->full_y = -dy;
  }

  // culling
  if(table->mode == DT_CULLING_MODE_CULLING)
  {
    // if shift+ctrl, we only change the current image
    if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
    {
      int mouseid = dt_control_get_mouse_over_id();
      l = table->list;
      while(l)
      {
        dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
        if(th->imgid == mouseid)
        {
          th->zoom_delta += val;
          break;
        }
        l = g_list_next(l);
      }
    }
    else
    {
      // if global zoom doesn't change (we reach bounds) we may have to move individual values
      if(table->full_zoom == nz && ((nz == 1.0f && val < 0.0f) || (nz == zmax && val > 0.0f)))
      {
        l = table->list;
        while(l)
        {
          dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
          if(th->zoom_delta != 0.0f) th->zoom_delta += val;
          l = g_list_next(l);
        }
      }
      table->full_zoom = nz;
    }
    // sanitize specific zoomming of individual images
    l = table->list;
    while(l)
    {
      dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
      if(table->full_zoom + th->zoom_delta < 1.0f) th->zoom_delta = 1.0f - table->full_zoom;
      if(table->full_zoom + th->zoom_delta > th->zoom_100) th->zoom_delta = th->zoom_100 - table->full_zoom;
      l = g_list_next(l);
    }
  }
  else // full preview
  {
    table->full_zoom = nz;
  }

  // redraw
  l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    th->zoom_glob = table->full_zoom;
    dt_thumbnail_image_refresh(th);
    l = g_list_next(l);
  }

  return TRUE;
}

static gboolean _event_scroll(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GdkEventScroll *e = (GdkEventScroll *)event;
  dt_culling_t *table = (dt_culling_t *)user_data;
  gdouble delta;

  if(dt_gui_get_scroll_delta(e, &delta))
  {
    if((e->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
    {
      const double x = 0; // TODO
      const double y = 0;
      // zooming
      if(delta < 0)
      {
        _thumbs_zoom_add(table, 0.5f, x, y, e->state);
      }
      else
      {
        _thumbs_zoom_add(table, -0.5f, x, y, e->state);
      }
    }
    else
    {
      if(delta < 0)
        _thumbs_move(table, -1);
      else
        _thumbs_move(table, 1);
    }
  }
  return TRUE;
}

static gboolean _event_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  if(!GTK_IS_CONTAINER(gtk_widget_get_parent(widget))) return TRUE;

  // we render the background (can be visible if before first image / after last image)
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_render_background(context, cr, 0, 0, gtk_widget_get_allocated_width(widget),
                        gtk_widget_get_allocated_height(widget));

  // but we don't really want to draw something, this is just to know when the widget is really ready
  dt_culling_t *table = (dt_culling_t *)user_data;
  dt_culling_full_redraw(table, FALSE);
  return FALSE; // let's propagate this event
}

static gboolean _event_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_culling_t *table = (dt_culling_t *)user_data;
  // if we leave thumbtable in favour of an inferior (a thumbnail) it's not a real leave !
  if(event->detail == GDK_NOTIFY_INFERIOR) return FALSE;

  table->mouse_inside = FALSE;
  dt_control_set_mouse_over_id(-1);
  return TRUE;
}

static gboolean _event_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  // we only handle the case where we enter thumbtable from an inferior (a thumbnail)
  // this is when the mouse enter an "empty" area of thumbtable
  if(event->detail != GDK_NOTIFY_INFERIOR) return FALSE;

  dt_control_set_mouse_over_id(-1);
  return TRUE;
}

static gboolean _event_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_culling_t *table = (dt_culling_t *)user_data;
  table->pan_x = event->x_root;
  table->pan_y = event->y_root;
  table->panning = TRUE;
  return TRUE;
}

static gboolean _event_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_culling_t *table = (dt_culling_t *)user_data;
  if(!table->panning) return FALSE;

  GList *l;

  // get the max zoom of all images
  const int max_in_memory_images = _get_max_in_memory_images();
  float fz = table->full_zoom;
  if(table->mode == DT_CULLING_MODE_CULLING && table->thumbs_count <= max_in_memory_images)
  {
    l = table->list;
    while(l)
    {
      dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
      fz = fmaxf(fz, table->full_zoom + th->zoom_delta);
      l = g_list_next(l);
    }
  }

  if(table->panning && fz > 1.0f)
  {
    const double x = event->x_root;
    const double y = event->y_root;
    // we want the images to stay in the screen
    if(table->mode == DT_CULLING_MODE_PREVIEW && g_list_length(table->list) > 0)
    {
      dt_thumbnail_t *th = (dt_thumbnail_t *)g_list_nth_data(table->list, 0);
      table->full_x += x - table->pan_x;
      table->full_y += y - table->pan_y;
      table->full_x = fminf(table->full_x, th->img_width - th->width * 0.97);
      table->full_x = fmaxf(table->full_x, 0);
      table->full_y = fminf(table->full_y, th->img_height - th->height * 0.97);
      table->full_y = fmaxf(table->full_y, 0);
    }
    else if(table->mode == DT_CULLING_MODE_CULLING && table->thumbs_count <= max_in_memory_images)
    {
      const float valx = x - table->pan_x;
      const float valy = y - table->pan_y;

      float xmin = 0.0f;
      float ymin = 0.0f;
      l = table->list;
      while(l)
      {
        dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
        xmin = fminf(xmin, th->width * 0.97 - th->img_width);
        ymin = fminf(ymin, th->height * 0.97 - th->img_height);
        l = g_list_next(l);
      }
      float nx = fmaxf(xmin, table->full_x + valx);
      float ny = fmaxf(ymin, table->full_y + valy);

      if((event->state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
      {
        int mouseid = dt_control_get_mouse_over_id();
        l = table->list;
        while(l)
        {
          dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
          if(th->imgid == mouseid)
          {
            th->zx_delta += valx;
            th->zy_delta += valy;
            break;
          }
          l = g_list_next(l);
        }
      }
      else
      {
        // if global position doesn't change (we reach bounds) we may have to move individual values
        if(table->full_x == nx && ((nx == 0 && valx < 0.0f) || (nx == xmin && valx > 0.0f)))
        {
          l = table->list;
          while(l)
          {
            dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
            if(th->zx_delta != 0.0f) th->zx_delta += valx;
            l = g_list_next(l);
          }
        }
        if(table->full_y == ny && ((ny == 0 && valy < 0.0f) || (ny == ymin && valy > 0.0f)))
        {
          l = table->list;
          while(l)
          {
            dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
            if(th->zy_delta != 0.0f) th->zy_delta += valy;
            l = g_list_next(l);
          }
        }
        table->full_x = nx;
        table->full_y = ny;
      }
      // sanitize specific positions of individual images
      l = table->list;
      while(l)
      {
        dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
        if(table->full_x + th->zx_delta > 0) th->zx_delta = -table->full_x;
        if(table->full_x + th->zx_delta < th->width * 0.97 - th->img_width)
          th->zx_delta = th->width * 0.97 - th->img_width - table->full_x;
        if(table->full_y + th->zy_delta > 0) th->zy_delta = -table->full_y;
        if(table->full_y + th->zy_delta < th->height * 0.97 - th->img_height)
          th->zy_delta = th->height * 0.97 - th->img_height - table->full_y;
        l = g_list_next(l);
      }
    }

    table->pan_x = x;
    table->pan_y = y;
  }

  // redraw TODO : don't reload image, just move it !
  l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    th->zx_glob = table->full_x;
    th->zy_glob = table->full_y;
    dt_thumbnail_image_refresh_position(th);
    l = g_list_next(l);
  }
  return TRUE;
}

static gboolean _event_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_culling_t *table = (dt_culling_t *)user_data;
  table->panning = FALSE;
  return TRUE;
}

// called each time the preference change, to update specific parts
static void _dt_pref_change_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  // dt_culling_t *table = (dt_culling_t *)user_data;

  // dt_thumbtable_full_redraw(table, TRUE);
}

static void _dt_profile_change_callback(gpointer instance, int type, gpointer user_data)
{
  if(!user_data) return;
  dt_culling_t *table = (dt_culling_t *)user_data;

  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    dt_thumbnail_image_refresh(th);
    l = g_list_next(l);
  }
}

// this is called each time mouse_over id change
static void _dt_mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_culling_t *table = (dt_culling_t *)user_data;

  const int imgid = dt_control_get_mouse_over_id();

  if(imgid > 0)
  {
    // let's be absolutely sure that the right widget has the focus
    // otherwise accels don't work...
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  }

  // we crawl over all images to find the right one
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    // if needed, the change mouseover value of the thumb
    if(th->mouse_over != (th->imgid == imgid)) dt_thumbnail_set_mouseover(th, (th->imgid == imgid));
    l = g_list_next(l);
  }
}

// this is called each time collected images change
static void _dt_collection_changed_callback(gpointer instance, dt_collection_change_t query_change, gpointer imgs,
                                            const int next, gpointer user_data)
{
  /*if(!user_data) return;
  dt_culling_t *table = (dt_culling_t *)user_data;
  if(query_change == DT_COLLECTION_CHANGE_RELOAD)
  {*/
  /** Here's how it works
   *
   *          list of change|   | x | x | x | x |
   *  offset inside the list| ? |   | x | x | x |
   * offset rowid as changed| ? | ? |   | x | x |
   *     next imgid is valid| ? | ? | ? |   | x |
   *                        |   |   |   |   |   |
   *                        | S | S | S | S | N |
   * S = same imgid as offset ; N = next imgid as offset
   **/
  /*int newid = table->offset_imgid;
  if(newid <= 0 && table->offset > 0) newid = _thumb_get_imgid(table->offset);

  // is the current offset imgid in the changed list
  gboolean in_list = FALSE;
  GList *l = imgs;
  while(l)
  {
    if(table->offset_imgid == GPOINTER_TO_INT(l->data))
    {
      in_list = TRUE;
      break;
    }
    l = g_list_next(l);
  }

  if(in_list)
  {
    if(next > 0 && _thumb_get_rowid(table->offset_imgid) != table->offset)
    {
      // if offset has changed, that means the offset img has moved. So we use the next untouched image as offset
      // but we have to ensure next is in the selection if we navigate inside sel.
      newid = next;
      if(table->navigate_inside_selection)
      {
        sqlite3_stmt *stmt;
        gchar *query = dt_util_dstrcat(
            NULL,
            "SELECT m.imgid FROM memory.collected_images as m, main.selected_images as s "
            "WHERE m.imgid=s.imgid AND m.rowid>=(SELECT rowid FROM memory.collected_images WHERE imgid=%d) "
            "ORDER BY m.rowid LIMIT 1",
            next);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
        if(sqlite3_step(stmt) == SQLITE_ROW)
        {
          newid = sqlite3_column_int(stmt, 0);
        }
        else
        {
          // no select image after, search before
          g_free(query);
          sqlite3_finalize(stmt);
          query = dt_util_dstrcat(
              NULL,
              "SELECT m.imgid FROM memory.collected_images as m, main.selected_images as s "
              "WHERE m.imgid=s.imgid AND m.rowid<(SELECT rowid FROM memory.collected_images WHERE imgid=%d) "
              "ORDER BY m.rowid DESC LIMIT 1",
              next);
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
          if(sqlite3_step(stmt) == SQLITE_ROW)
          {
            newid = sqlite3_column_int(stmt, 0);
          }
        }
        g_free(query);
        sqlite3_finalize(stmt);
      }
    }
  }

  // get the new rowid of the new offset image
  table->offset_imgid = newid;
  table->offset = _thumb_get_rowid(newid);
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", table->offset);

  dt_thumbtable_full_redraw(table, TRUE);

  dt_view_lighttable_change_offset(darktable.view_manager, FALSE, newid);

  // and for images that have changed but are still in the view, we update datas
  l = imgs;
  while(l)
  {
    dt_thumbnail_t *th = _thumbtable_get_thumb(table, GPOINTER_TO_INT(l->data));
    if(th)
    {
      dt_thumbnail_update_infos(th);
    }
    l = g_list_next(l);
  }
}
else
{
  // otherwise we reset the offset to the beginning
  table->offset = 1;
  table->offset_imgid = _thumb_get_imgid(table->offset);
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", 1);
  // and we reset position of first thumb for zooming
  if(g_list_length(table->list) > 0)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)g_list_nth_data(table->list, 0);
    thumb->x = 0;
    thumb->y = 0;
  }
  dt_thumbtable_full_redraw(table, TRUE);
  dt_view_lighttable_change_offset(darktable.view_manager, TRUE, table->offset_imgid);
}*/
}

static void _dt_filmstrip_change(gpointer instance, int imgid, gpointer user_data)
{
  if(!user_data || imgid <= 0) return;
  dt_culling_t *table = (dt_culling_t *)user_data;

  table->offset = _thumb_get_rowid(imgid);
  dt_culling_full_redraw(table, TRUE);
}

dt_culling_t *dt_culling_new(dt_culling_mode_t mode)
{
  dt_culling_t *table = (dt_culling_t *)calloc(1, sizeof(dt_culling_t));
  table->mode = mode;
  table->full_zoom = 1.0;
  table->widget = gtk_layout_new(NULL, NULL);
  // TODO dt_gui_add_help_link(table->widget, dt_get_help_url("lighttable_filemanager"));

  // set css name and class
  if(mode == DT_CULLING_MODE_PREVIEW)
    gtk_widget_set_name(table->widget, "preview");
  else
    gtk_widget_set_name(table->widget, "culling");
  GtkStyleContext *context = gtk_widget_get_style_context(table->widget);
  if(mode == DT_CULLING_MODE_PREVIEW)
    gtk_style_context_add_class(context, "dt_preview");
  else
    gtk_style_context_add_class(context, "dt_culling");
  gtk_style_context_add_class(context, "dt_overlays_hover_extended");

  // set widget signals
  gtk_widget_set_events(table->widget, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                           | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                           | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  gtk_widget_set_app_paintable(table->widget, TRUE);
  gtk_widget_set_can_focus(table->widget, TRUE);

  // drag and drop : used for interactions with maps, exporting uri to external apps, importing images
  // in filmroll...
  /*gtk_drag_source_set(table->widget, GDK_BUTTON1_MASK, target_list_all, n_targets_all, GDK_ACTION_COPY);
  gtk_drag_dest_set(table->widget, GTK_DEST_DEFAULT_ALL, target_list_all, n_targets_all, GDK_ACTION_COPY);
  g_signal_connect_after(table->widget, "drag-begin", G_CALLBACK(_event_dnd_begin), table);
  g_signal_connect_after(table->widget, "drag-end", G_CALLBACK(_event_dnd_end), table);
  g_signal_connect(table->widget, "drag-data-get", G_CALLBACK(_event_dnd_get), table);
  g_signal_connect(table->widget, "drag-data-received", G_CALLBACK(_event_dnd_received), table);*/

  g_signal_connect(G_OBJECT(table->widget), "scroll-event", G_CALLBACK(_event_scroll), table);
  g_signal_connect(G_OBJECT(table->widget), "draw", G_CALLBACK(_event_draw), table);
  g_signal_connect(G_OBJECT(table->widget), "leave-notify-event", G_CALLBACK(_event_leave_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "enter-notify-event", G_CALLBACK(_event_enter_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "button-press-event", G_CALLBACK(_event_button_press), table);
  g_signal_connect(G_OBJECT(table->widget), "motion-notify-event", G_CALLBACK(_event_motion_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "button-release-event", G_CALLBACK(_event_button_release), table);

  // we register globals signals
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_dt_collection_changed_callback), table);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_dt_mouse_over_image_callback), table);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            G_CALLBACK(_dt_profile_change_callback), table);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(_dt_pref_change_callback),
                            table);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            G_CALLBACK(_dt_filmstrip_change), table);
  gtk_widget_show(table->widget);

  g_object_ref(table->widget);

  return table;
}

// initialize offset, ... values
// to be used when reentering culling
void dt_culling_init(dt_culling_t *table, int offset)
{
  /** HOW it works :
   *
   * For the first image :
   *  image_over OR first selected OR first OR -1
   *
   * For the navigation in selection :
   *  culling dynamic mode                       => OFF
   *  first image in selection AND selection > 1 => ON
   *  otherwise                                  => OFF
   *
   * For the selection following :
   *  culling dynamic mode                                    => OFF
   *  first image(s) == selection && selection is continuous  => ON
   */

  // init values
  table->navigate_inside_selection = FALSE;
  table->selection_sync = FALSE;
  table->full_zoom = 1.0;
  table->full_x = 0;
  table->full_y = 0;

  // get first id
  sqlite3_stmt *stmt;
  int first_id = -1;

  if(offset > 0)
    first_id = _thumb_get_imgid(offset);
  else
    first_id = dt_control_get_mouse_over_id();

  if(first_id < 1)
  {
    // search the first selected image
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT col.imgid "
                                "FROM memory.collected_images AS col, main.selected_images as sel "
                                "WHERE col.imgid=sel.imgid "
                                "ORDER BY col.rowid "
                                "LIMIT 1",
                                -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) first_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  if(first_id < 1)
  {
    // search the first image shown in view (this is the offset of thumbtable)
    first_id = _thumb_get_imgid(1);
  }
  if(first_id < 1)
  {
    // Arrrghh
    return;
  }

  // special culling dynamic mode
  if(table->mode == DT_CULLING_MODE_CULLING
     && dt_view_lighttable_get_culling_zoom_mode(darktable.view_manager) == DT_LIGHTTABLE_ZOOM_DYNAMIC)
  {
    table->navigate_inside_selection = TRUE;
    table->offset = _thumb_get_rowid(first_id);
    return;
  }

  // selection count
  int sel_count = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT count(*) "
                              "FROM memory.collected_images AS col, main.selected_images as sel "
                              "WHERE col.imgid=sel.imgid",
                              -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) sel_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  // is first_id inside selection ?
  gboolean inside = FALSE;
  gchar *query = dt_util_dstrcat(NULL,
                                 "SELECT col.imgid "
                                 "FROM memory.collected_images AS col, main.selected_images AS sel "
                                 "WHERE col.imgid=sel.imgid AND col.imgid=%d",
                                 first_id);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) inside = TRUE;
  sqlite3_finalize(stmt);
  g_free(query);

  if(table->mode == DT_CULLING_MODE_PREVIEW)
  {
    table->navigate_inside_selection = (sel_count > 1 && inside);
    table->selection_sync = (sel_count == 1 && inside);
  }
  else if(table->mode == DT_CULLING_MODE_CULLING)
  {
    const int zoom = dt_view_lighttable_get_zoom(darktable.view_manager);
    // we first determine if we synchronize the selection with culling images
    table->selection_sync = FALSE;
    if(sel_count == 1 && inside)
      table->selection_sync = TRUE;
    else if(sel_count == zoom && inside)
    {
      // we ensure that the selection is continuous
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT MIN(rowid), MAX(rowid) "
                                  "FROM memory.collected_images AS col, main.selected_images as sel "
                                  "WHERE col.imgid=sel.imgid ",
                                  -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        if(sqlite3_column_int(stmt, 0) + sel_count - 1 == sqlite3_column_int(stmt, 1))
        {
          table->selection_sync = TRUE;
        }
      }
      sqlite3_finalize(stmt);
    }

    // we now determine if we limit culling images to the selection
    table->navigate_inside_selection = (!table->selection_sync && inside);
  }

  table->offset = _thumb_get_rowid(first_id);
}

static gboolean _thumbs_recreate_list_at(dt_culling_t *table, const int offset)
{
  gchar *query = NULL;
  sqlite3_stmt *stmt;

  if(table->navigate_inside_selection)
  {
    query = dt_util_dstrcat(NULL,
                            "SELECT m.rowid, m.imgid, b.aspect_ratio "
                            "FROM memory.collected_images AS m, main.selected_images AS s, images AS b "
                            "WHERE m.imgid = b.id AND m.imgid = s.imgid AND m.rowid >= %d "
                            "ORDER BY m.rowid "
                            "LIMIT %d",
                            offset, table->thumbs_count);
  }
  else
  {
    query = dt_util_dstrcat(NULL,
                            "SELECT m.rowid, m.imgid, b.aspect_ratio "
                            "FROM (SELECT rowid, imgid "
                            "FROM memory.collected_images "
                            "WHERE rowid < %d + %d "
                            "ORDER BY rowid DESC "
                            "LIMIT %d) AS m, "
                            "images AS b "
                            "WHERE m.imgid = b.id "
                            "ORDER BY m.rowid",
                            offset, table->thumbs_count, table->thumbs_count);
  }

  GList *newlist = NULL;
  int nbnew = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW && g_list_length(newlist) <= table->thumbs_count)
  {
    const int nrow = sqlite3_column_int(stmt, 0);
    const int nid = sqlite3_column_int(stmt, 1);
    // first, we search if the thumb is already here
    GList *tl = g_list_find_custom(table->list, GINT_TO_POINTER(nid), _list_compare_by_imgid);
    if(tl)
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)tl->data;
      thumb->rowid = nrow; // this may have changed
      newlist = g_list_append(newlist, thumb);
      // and we remove the thumb from the old list
      table->list = g_list_remove(table->list, thumb);
    }
    else
    {
      // we create a completly new thumb
      dt_thumbnail_t *thumb = dt_thumbnail_new(10, 10, nid, nrow, DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED, TRUE);
      thumb->display_focus = table->focus;
      double aspect_ratio = sqlite3_column_double(stmt, 2);
      if(!aspect_ratio || aspect_ratio < 0.0001)
      {
        aspect_ratio = dt_image_set_aspect_ratio(nid, FALSE);
        // if an error occurs, let's use 1:1 value
        if(aspect_ratio < 0.0001) aspect_ratio = 1.0;
      }
      thumb->aspect_ratio = aspect_ratio;
      newlist = g_list_append(newlist, thumb);
      nbnew++;
    }
    // if it's the offset, we record the imgid
    if(nrow == table->offset) table->offset_imgid = nid;
  }

  // in rare cases, we can have less images than wanted
  // although there's images before
  if(table->navigate_inside_selection && g_list_length(newlist) < table->thumbs_count
     && g_list_length(newlist) < _get_selection_count())
  {
    const int nb = table->thumbs_count - g_list_length(newlist);
    query = dt_util_dstrcat(NULL,
                            "SELECT m.rowid, m.imgid, b.aspect_ratio "
                            "FROM memory.collected_images AS m, main.selected_images AS s, images AS b "
                            "WHERE m.imgid = b.id AND m.imgid = s.imgid AND m.rowid < %d "
                            "ORDER BY m.rowid DESC "
                            "LIMIT %d",
                            offset, nb);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    if(stmt != NULL)
    {
      while(sqlite3_step(stmt) == SQLITE_ROW && g_list_length(newlist) <= table->thumbs_count)
      {
        const int nrow = sqlite3_column_int(stmt, 0);
        const int nid = sqlite3_column_int(stmt, 1);
        // first, we search if the thumb is already here
        GList *tl = g_list_find_custom(table->list, GINT_TO_POINTER(nid), _list_compare_by_imgid);
        if(tl)
        {
          dt_thumbnail_t *thumb = (dt_thumbnail_t *)tl->data;
          thumb->rowid = nrow; // this may have changed
          newlist = g_list_prepend(newlist, thumb);
          // and we remove the thumb from the old list
          table->list = g_list_remove(table->list, thumb);
        }
        else
        {
          // we create a completly new thumb
          dt_thumbnail_t *thumb = dt_thumbnail_new(10, 10, nid, nrow, DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED, TRUE);
          thumb->display_focus = table->focus;
          double aspect_ratio = sqlite3_column_double(stmt, 2);
          if(!aspect_ratio || aspect_ratio < 0.0001)
          {
            aspect_ratio = dt_image_set_aspect_ratio(nid, FALSE);
            // if an error occurs, let's use 1:1 value
            if(aspect_ratio < 0.0001) aspect_ratio = 1.0;
          }
          thumb->aspect_ratio = aspect_ratio;
          newlist = g_list_prepend(newlist, thumb);
          nbnew++;
        }
        // if it's the offset, we record the imgid
        if(nrow == table->offset) table->offset_imgid = nid;
      }
      sqlite3_finalize(stmt);
    }
    g_free(query);
  }

  // now we cleanup all remaining thumbs from old table->list and set it again
  g_list_free_full(table->list, _list_remove_thumb);
  table->list = newlist;

  return TRUE;
}

static gboolean _thumbs_compute_positions(dt_culling_t *table)
{
  if(!table->list || g_list_length(table->list) == 0) return FALSE;

  // if we have only 1 image, it should take the entire screen
  if(g_list_length(table->list) == 1)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)g_list_nth_data(table->list, 0);
    thumb->width = table->view_width;
    thumb->height = table->view_height;
    thumb->x = 0;
    thumb->y = 0;
    return TRUE;
  }

  int sum_w = 0, max_h = 0, max_w = 0;

  GList *slots = NULL;

  unsigned int total_width = 0, total_height = 0;
  int distance = 1;
  float avg_ratio = 0;

  // reinit size and positions and get max values
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    const double aspect_ratio = thumb->aspect_ratio;
    thumb->width = (gint)(sqrt(aspect_ratio) * 100);
    thumb->height = (gint)(1 / sqrt(aspect_ratio) * 100);
    thumb->x = thumb->y = 0;

    sum_w += thumb->width;
    max_w = MAX(max_w, thumb->width);
    max_h = MAX(max_h, thumb->height);
    avg_ratio += thumb->width / (float)thumb->height;
    l = g_list_next(l);
  }

  avg_ratio /= g_list_length(table->list);

  int per_row, tmp_per_row, per_col, tmp_per_col;
  per_row = tmp_per_row = ceil(sqrt(g_list_length(table->list)));
  per_col = tmp_per_col = (g_list_length(table->list) + per_row - 1) / per_row;

  float tmp_slot_ratio, slot_ratio;
  tmp_slot_ratio = slot_ratio = (table->view_width / (float)per_row) / (table->view_height / (float)per_col);

  do
  {
    per_row = tmp_per_row;
    per_col = tmp_per_col;
    slot_ratio = tmp_slot_ratio;

    if(avg_ratio > slot_ratio)
    {
      tmp_per_row = per_row - 1;
    }
    else
    {
      tmp_per_row = per_row + 1;
    }

    if(tmp_per_row == 0) break;

    tmp_per_col = (g_list_length(table->list) + tmp_per_row - 1) / tmp_per_row;

    tmp_slot_ratio = (table->view_width / (float)tmp_per_row) / (table->view_height / (float)tmp_per_col);

  } while(per_row > 0 && per_row <= g_list_length(table->list)
          && _absmul(tmp_slot_ratio, avg_ratio) < _absmul(slot_ratio, avg_ratio));


  // Vertical layout
  l = table->list;
  while(l)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    GList *slot_iter = g_list_first(slots);
    for(; slot_iter; slot_iter = slot_iter->next)
    {
      GList *slot = (GList *)slot_iter->data;
      // Calculate current total height of slot
      int slot_h = distance;
      GList *slot_cw_iter = slot;
      while(slot_cw_iter != NULL)
      {
        dt_thumbnail_t *slot_cw = (dt_thumbnail_t *)slot_cw_iter->data;
        slot_h = slot_h + slot_cw->height + distance;
        slot_cw_iter = slot_cw_iter->next;
      }
      // Add window to slot if the slot height after adding the window
      // doesn't exceed max window height
      if(slot_h + distance + thumb->height < max_h)
      {
        slot_iter->data = g_list_append(slot, thumb);
        break;
      }
    }
    // Otherwise, create a new slot with only this window
    if(!slot_iter) slots = g_list_append(slots, g_list_append(NULL, thumb));
    l = g_list_next(l);
  }

  GList *rows = g_list_append(NULL, NULL);
  {
    int row_y = 0, x = 0, row_h = 0;
    int max_row_w = sum_w / per_col;
    for(GList *slot_iter = slots; slot_iter != NULL; slot_iter = slot_iter->next)
    {
      GList *slot = (GList *)slot_iter->data;

      // Max width of windows in the slot
      int slot_max_w = 0;
      for(GList *slot_cw_iter = slot; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
      {
        dt_thumbnail_t *cw = (dt_thumbnail_t *)slot_cw_iter->data;
        slot_max_w = MAX(slot_max_w, cw->width);
      }

      int y = row_y;
      for(GList *slot_cw_iter = slot; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
      {
        dt_thumbnail_t *cw = (dt_thumbnail_t *)slot_cw_iter->data;
        cw->x = x + (slot_max_w - cw->width) / 2;
        cw->y = y;
        y += cw->height + distance;
        rows->data = g_list_append(rows->data, cw);
      }

      row_h = MAX(row_h, y - row_y);
      total_height = MAX(total_height, y);
      x += slot_max_w + distance;
      total_width = MAX(total_width, x);

      if(x > max_row_w)
      {
        x = 0;
        row_y += row_h;
        row_h = 0;
        rows = g_list_append(rows, 0);
        rows = rows->next;
      }
      g_list_free(slot);
    }
    g_list_free(slots);
    slots = NULL;
  }

  total_width -= distance;
  total_height -= distance;

  for(GList *iter = g_list_first(rows); iter != NULL; iter = iter->next)
  {
    GList *row = (GList *)iter->data;
    int row_w = 0, xoff;
    int max_rh = 0;

    for(GList *slot_cw_iter = row; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
    {
      dt_thumbnail_t *cw = (dt_thumbnail_t *)slot_cw_iter->data;
      row_w = MAX(row_w, cw->x + cw->width);
      max_rh = MAX(max_rh, cw->height);
    }

    xoff = (total_width - row_w) / 2;

    for(GList *cw_iter = row; cw_iter != NULL; cw_iter = cw_iter->next)
    {
      dt_thumbnail_t *cw = (dt_thumbnail_t *)cw_iter->data;
      cw->x += xoff;
      cw->height = max_rh;
    }
    g_list_free(row);
  }

  g_list_free(rows);

  float factor;
  factor = (float)(table->view_width - 1) / total_width;
  if(factor * total_height > table->view_height - 1) factor = (float)(table->view_height - 1) / total_height;

  int xoff = (table->view_width - (float)total_width * factor) / 2;
  int yoff = (table->view_height - (float)total_height * factor) / 2;

  l = table->list;
  while(l)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    thumb->width = thumb->width * factor;
    thumb->height = thumb->height * factor;
    thumb->x = thumb->x * factor + xoff;
    thumb->y = thumb->y * factor + yoff;
    l = g_list_next(l);
  }

  // we save the current first id
  dt_conf_set_int("plugins/lighttable/culling_last_id", table->offset_imgid);

  return TRUE;
}

// recreate the list of thumb if needed and recomputes sizes and positions if needed
void dt_culling_full_redraw(dt_culling_t *table, gboolean force)
{
  const double start = dt_get_wtime();
  // first, we see if we need to do something
  if(!_compute_sizes(table, force)) return;

  // we recreate the list of images
  _thumbs_recreate_list_at(table, table->offset);

  // we compute the sizes and positions of thumbs
  _thumbs_compute_positions(table);

  // we erase the list of active images
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = NULL;

  // and we effectively move and resize thumbs
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    // we add or move the thumb at the right position
    if(!gtk_widget_get_parent(thumb->w_main))
    {
      gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, thumb->x, thumb->y);
    }
    else
    {
      gtk_layout_move(GTK_LAYOUT(table->widget), thumb->w_main, thumb->x, thumb->y);
    }
    // and we resize the thumb
    dt_thumbnail_resize(thumb, thumb->width, thumb->height, FALSE);

    // we update the active images list
    darktable.view_manager->active_images
        = g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(thumb->imgid));

    l = g_list_next(l);
  }

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);

  // if the selection should follow active images
  if(table->selection_sync)
  {
    // deactivate selection_change event
    table->select_desactivate = TRUE;
    // deselect all
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
    // select all active images
    GList *ls = NULL;
    l = table->list;
    while(l)
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
      ls = g_list_append(ls, GINT_TO_POINTER(thumb->imgid));
      l = g_list_next(l);
    }
    dt_selection_select_list(darktable.selection, ls);
    g_list_free(ls);
    // reactivate selection_change event
    table->select_desactivate = FALSE;
  }

  // be sure the focus is in the right widget (needed for accels)
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  dt_print(DT_DEBUG_LIGHTTABLE, "done in %0.04f sec\n", dt_get_wtime() - start);

  if(darktable.unmuted & DT_DEBUG_CACHE) dt_mipmap_cache_print(darktable.mipmap_cache);
}

gboolean dt_culling_key_move(dt_culling_t *table, dt_culling_move_t move)
{
  int val = 0;
  switch(move)
  {
    case DT_CULLING_MOVE_LEFT:
    case DT_CULLING_MOVE_UP:
      val = -1;
      break;
    case DT_CULLING_MOVE_RIGHT:
    case DT_CULLING_MOVE_DOWN:
      val = 1;
      break;
    case DT_CULLING_MOVE_PAGEUP:
      val = -1 * table->thumbs_count;
      break;
    case DT_CULLING_MOVE_PAGEDOWN:
      val = table->thumbs_count;
      break;
    case DT_CULLING_MOVE_START:
      val = -1 * INT_MAX;
      break;
    case DT_CULLING_MOVE_END:
      val = INT_MAX;
      break;
    default:
      val = 0;
      break;
  }
  _thumbs_move(table, val);
  return TRUE;
}

void dt_culling_change_offset_image(dt_culling_t *table, int imgid)
{
  table->offset = _thumb_get_rowid(imgid);
  dt_culling_full_redraw(table, TRUE);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
