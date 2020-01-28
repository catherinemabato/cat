/*
    This file is part of darktable,
    copyright (c) 2019--2020 Aldric Renaudin.

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
/** this is the thumbnail class for the lighttable module.  */
#include "dtgtk/thumbnail.h"

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/debug.h"
#include "common/grouping.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/icon.h"
#include "dtgtk/thumbnail_btn.h"
#include "views/view.h"

static void _set_flag(GtkWidget *w, GtkStateFlags flag, gboolean over)
{
  int flags = gtk_widget_get_state_flags(w);
  if(over)
    flags |= flag;
  else
    flags &= ~flag;

  gtk_widget_set_state_flags(w, flags, TRUE);
}

static void _image_get_infos(dt_thumbnail_t *thumb)
{
  if(thumb->imgid <= 0) return;

  // we only get here infos that might change, others(exif, ...) are cached on widget creation

  thumb->rating = 0;
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->imgid, 'r');
  if(img)
  {
    thumb->rating = (img->flags & 0x7);

    thumb->groupid = img->group_id;

    dt_image_cache_read_release(darktable.image_cache, img);
  }

  // colorlabels
  thumb->colorlabels = 0;
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.get_color);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.get_color);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_color, 1, thumb->imgid);
  while(sqlite3_step(darktable.view_manager->statements.get_color) == SQLITE_ROW)
  {
    const int col = sqlite3_column_int(darktable.view_manager->statements.get_color, 0);
    // we reuse CPF_* flags, as we'll pass them to the paint fct after
    if(col == 0)
      thumb->colorlabels |= CPF_DIRECTION_UP;
    else if(col == 1)
      thumb->colorlabels |= CPF_DIRECTION_DOWN;
    else if(col == 2)
      thumb->colorlabels |= CPF_DIRECTION_LEFT;
    else if(col == 3)
      thumb->colorlabels |= CPF_DIRECTION_RIGHT;
    else if(col == 4)
      thumb->colorlabels |= CPF_BG_TRANSPARENT;
  }
  if(thumb->w_color)
  {
    GtkDarktableThumbnailBtn *btn = (GtkDarktableThumbnailBtn *)thumb->w_color;
    btn->icon_flags = thumb->colorlabels;
  }

  // altered
  thumb->is_altered = dt_image_altered(thumb->imgid);

  // grouping
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.get_grouped);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.get_grouped);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 1, thumb->imgid);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 2, thumb->imgid);
  thumb->is_grouped = (sqlite3_step(darktable.view_manager->statements.get_grouped) == SQLITE_ROW);
}

static gboolean _thumb_expose_again(gpointer user_data)
{
  if(!user_data || !GTK_IS_WIDGET(user_data)) return FALSE;

  GtkWidget *widget = (GtkWidget *)user_data;
  gtk_widget_queue_draw(widget);
  return FALSE;
}

static gboolean _event_image_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(thumb->imgid <= 0)
  {
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
    cairo_paint(cr);
    return TRUE;
  }

  // if we don't have it in memory, we want the image surface
  if(!thumb->img_surf)
  {
    const gboolean res
        = dt_view_image_get_surface(thumb->imgid, thumb->width * 0.91, thumb->height * 0.91, &thumb->img_surf);
    if(res)
    {
      // if the image is missing, we reload it again
      g_timeout_add(250, _thumb_expose_again, widget);
      return TRUE;
    }

    // let save thumbnail image size
    thumb->img_width = cairo_image_surface_get_width(thumb->img_surf);
    thumb->img_height = cairo_image_surface_get_height(thumb->img_surf);
    gtk_widget_set_size_request(widget, thumb->img_width, thumb->img_height);

    // now that we know image ratio, we can fill the extension label
    const char *ext = thumb->filename + strlen(thumb->filename);
    gchar *ext2 = NULL;
    while(ext > thumb->filename && *ext != '.') ext--;
    ext++;
    if(thumb->img_width < thumb->img_height)
    {
      // vertical disposition
      for(int i = 0; i < strlen(ext); i++) ext2 = dt_util_dstrcat(ext2, "%.1s\n", &ext[i]);
    }
    else
      ext2 = dt_util_dstrcat(ext2, "%s", ext);
    gchar *upcase_ext = g_ascii_strup(ext2, -1); // extension in capital letters to avoid character descenders
    const int fsize = fminf(DT_PIXEL_APPLY_DPI(20.0), .09 * thumb->width);
    gchar *ext_final = dt_util_dstrcat(NULL, "<span size=\"%d\">%s</span>", fsize * PANGO_SCALE, upcase_ext);
    gtk_label_set_markup(GTK_LABEL(thumb->w_ext), ext_final);
    g_free(upcase_ext);
    g_free(ext_final);
    g_free(ext2);

    return TRUE;
  }

  // we draw the image
  cairo_set_source_surface(cr, thumb->img_surf, 0, 0);
  cairo_paint(cr);

  // and eventually the image border
  GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_image);
  gtk_render_frame(context, cr, 0, 0, thumb->img_width, thumb->img_height);

  return TRUE;
}

static gboolean _event_main_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  dt_control_set_mouse_over_id(thumb->imgid);
  _set_flag(thumb->w_bottom_eb, GTK_STATE_FLAG_PRELIGHT, FALSE);
  return TRUE;
}

static gboolean _event_main_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, thumb->imgid);
    return TRUE;
  }
  return FALSE;
}
static gboolean _event_main_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;

  if(event->button == 1 && !thumb->moved)
  {
    if((event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) == 0)
      dt_selection_select_single(darktable.selection, thumb->imgid);
    else if((event->state & (GDK_CONTROL_MASK)) == GDK_CONTROL_MASK)
      dt_selection_toggle(darktable.selection, thumb->imgid);
    else if((event->state & (GDK_SHIFT_MASK)) == GDK_SHIFT_MASK)
      dt_selection_select_range(darktable.selection, thumb->imgid);
  }
  return FALSE;
}

static gboolean _event_rating_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;

  if(event->button == 1 && !thumb->moved)
  {
    dt_view_image_over_t rating = DT_VIEW_DESERT;
    if(widget == thumb->w_reject)
      rating = DT_VIEW_REJECT;
    else if(widget == thumb->w_stars[0])
      rating = DT_VIEW_STAR_1;
    else if(widget == thumb->w_stars[1])
      rating = DT_VIEW_STAR_2;
    else if(widget == thumb->w_stars[2])
      rating = DT_VIEW_STAR_3;
    else if(widget == thumb->w_stars[3])
      rating = DT_VIEW_STAR_4;
    else if(widget == thumb->w_stars[4])
      rating = DT_VIEW_STAR_5;

    if(rating != DT_VIEW_DESERT)
    {
      dt_ratings_apply(thumb->imgid, rating, TRUE, TRUE, TRUE);
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD);
    }
  }
  return FALSE;
}

static gboolean _event_grouping_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;

  if(event->button == 1 && !thumb->moved)
  {
    if(event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) // just add the whole group to the selection. TODO:
                                                           // make this also work for collapsed groups.
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(
          dt_database_get(darktable.db),
          "INSERT OR IGNORE INTO main.selected_images SELECT id FROM main.images WHERE group_id = ?1", -1, &stmt,
          NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, thumb->groupid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
    else if(!darktable.gui->grouping
            || thumb->groupid == darktable.gui->expanded_group_id) // the group is already expanded, so ...
    {
      if(thumb->imgid == darktable.gui->expanded_group_id && darktable.gui->grouping) // ... collapse it
        darktable.gui->expanded_group_id = -1;
      else // ... make the image the new representative of the group
        darktable.gui->expanded_group_id = dt_grouping_change_representative(thumb->imgid);
    }
    else // expand the group
      darktable.gui->expanded_group_id = thumb->groupid;
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD);
  }
  return FALSE;
}

static gboolean _event_audio_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;

  if(event->button == 1 && !thumb->moved)
  {
    gboolean start_audio = TRUE;
    if(darktable.view_manager->audio.audio_player_id != -1)
    {
      // don't start the audio for the image we just killed it for
      if(darktable.view_manager->audio.audio_player_id == thumb->imgid) start_audio = FALSE;
      dt_view_audio_stop(darktable.view_manager);
    }

    if(start_audio)
    {
      dt_view_audio_start(darktable.view_manager, thumb->imgid);
    }
  }
  return FALSE;
}

static void _thumb_update_icons(dt_thumbnail_t *thumb)
{
  gboolean show = (thumb->mouse_over || darktable.gui->show_overlays);
  gtk_widget_set_visible(thumb->w_bottom_eb, show);
  gtk_widget_set_visible(thumb->w_reject, show);
  for(int i = 0; i < 5; i++) gtk_widget_set_visible(thumb->w_stars[i], show);
  gtk_widget_set_visible(thumb->w_local_copy, show && thumb->has_localcopy);
  gtk_widget_set_visible(thumb->w_altered, show && thumb->is_altered);
  gtk_widget_set_visible(thumb->w_group, show && thumb->is_grouped);
  gtk_widget_set_visible(thumb->w_audio, show && thumb->has_audio);
  gtk_widget_set_visible(thumb->w_color, show && thumb->colorlabels != 0);

  _set_flag(thumb->w_back, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
  _set_flag(thumb->w_ext, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
  _set_flag(thumb->w_image, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
  _set_flag(thumb->w_back, GTK_STATE_FLAG_ACTIVE, thumb->active);
  _set_flag(thumb->w_ext, GTK_STATE_FLAG_ACTIVE, thumb->active);
  _set_flag(thumb->w_image, GTK_STATE_FLAG_ACTIVE, thumb->active);

  _set_flag(thumb->w_reject, GTK_STATE_FLAG_ACTIVE, (thumb->rating == DT_VIEW_REJECT));
  for(int i = 0; i < 5; i++)
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_ACTIVE, (thumb->rating > i && thumb->rating < DT_VIEW_REJECT));
  _set_flag(thumb->w_group, GTK_STATE_FLAG_ACTIVE, (thumb->imgid == thumb->groupid));

  _set_flag(thumb->w_back, GTK_STATE_FLAG_SELECTED, thumb->selected);
  _set_flag(thumb->w_ext, GTK_STATE_FLAG_SELECTED, thumb->selected);
  _set_flag(thumb->w_image, GTK_STATE_FLAG_SELECTED, thumb->selected);
}

static void _dt_selection_changed_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb) return;

  gboolean selected = FALSE;
  /* clear and reset statements */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);
  /* bind imgid to prepared statements */
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, thumb->imgid);
  /* lets check if imgid is selected */
  if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW) selected = TRUE;

  // if there's a change, update the thumb
  if(selected != thumb->selected)
  {
    thumb->selected = selected;
    _thumb_update_icons(thumb);
    gtk_widget_queue_draw(thumb->w_main);
  }
}

static void _dt_active_images_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb) return;

  gboolean active = FALSE;
  GSList *l = darktable.view_manager->active_images;
  while(l)
  {
    int id = GPOINTER_TO_INT(l->data);
    if(id == thumb->imgid)
    {
      active = TRUE;
      break;
    }
    l = g_slist_next(l);
  }

  // if there's a change, update the thumb
  if(active != thumb->active)
  {
    thumb->active = active;
    _thumb_update_icons(thumb);
    gtk_widget_queue_draw(thumb->w_main);
  }
}

static gboolean _event_bottom_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb->mouse_over) dt_control_set_mouse_over_id(thumb->imgid);
  _set_flag(thumb->w_bottom_eb, GTK_STATE_FLAG_PRELIGHT, TRUE);
  return FALSE;
}

static gboolean _event_star_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(!thumb->mouse_over) dt_control_set_mouse_over_id(thumb->imgid);
  _set_flag(thumb->w_bottom_eb, GTK_STATE_FLAG_PRELIGHT, TRUE);

  // we prelight all stars before the current one
  gboolean pre = TRUE;
  for(int i = 0; i < 5; i++)
  {
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_PRELIGHT, pre);
    gtk_widget_queue_draw(thumb->w_stars[i]);
    if(thumb->w_stars[i] == widget) pre = FALSE;
  }
  return TRUE;
}
static gboolean _event_star_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  for(int i = 0; i < 5; i++)
  {
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_PRELIGHT, FALSE);
    gtk_widget_queue_draw(thumb->w_stars[i]);
  }
  return TRUE;
}

GtkWidget *dt_thumbnail_create_widget(dt_thumbnail_t *thumb)
{
  // main widget (overlay)
  thumb->w_main = gtk_overlay_new();
  gtk_widget_set_name(thumb->w_main, "thumb_main");
  gtk_widget_set_size_request(thumb->w_main, thumb->width, thumb->height);

  if(thumb->imgid > 0)
  {
    g_signal_connect(G_OBJECT(thumb->w_main), "button-press-event", G_CALLBACK(_event_main_press), thumb);
    g_signal_connect(G_OBJECT(thumb->w_main), "button-release-event", G_CALLBACK(_event_main_release), thumb);

    g_object_set_data(G_OBJECT(thumb->w_main), "thumb", thumb);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE,
                              G_CALLBACK(_dt_active_images_callback), thumb);
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                              G_CALLBACK(_dt_selection_changed_callback), thumb);

    // the background
    thumb->w_back = gtk_event_box_new();
    gtk_widget_set_name(thumb->w_back, "thumb_back");
    g_signal_connect(G_OBJECT(thumb->w_back), "enter-notify-event", G_CALLBACK(_event_main_enter), thumb);
    gtk_widget_show(thumb->w_back);
    gtk_container_add(GTK_CONTAINER(thumb->w_main), thumb->w_back);

    // the file extension label
    thumb->w_ext = gtk_label_new("");
    gtk_widget_set_name(thumb->w_ext, "thumb_ext");
    gtk_widget_set_valign(thumb->w_ext, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_ext, GTK_ALIGN_START);
    gtk_widget_set_margin_start(thumb->w_ext, 0.045 * thumb->width);
    gtk_widget_set_margin_top(thumb->w_ext, 0.045 * thumb->width);
    gtk_label_set_justify(GTK_LABEL(thumb->w_ext), GTK_JUSTIFY_CENTER);
    gtk_widget_show(thumb->w_ext);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_ext);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(thumb->w_main), thumb->w_ext, TRUE);

    // the image drawing area
    thumb->w_image = gtk_drawing_area_new();
    gtk_widget_set_name(thumb->w_image, "thumb_image");
    gtk_widget_set_size_request(thumb->w_image, thumb->width, thumb->height);
    gtk_widget_set_valign(thumb->w_image, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(thumb->w_image, GTK_ALIGN_CENTER);
    gtk_widget_set_events(thumb->w_image, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                              | GDK_ENTER_NOTIFY_MASK | GDK_POINTER_MOTION_HINT_MASK
                                              | GDK_POINTER_MOTION_MASK);
    g_signal_connect(G_OBJECT(thumb->w_image), "draw", G_CALLBACK(_event_image_draw), thumb);
    g_signal_connect(G_OBJECT(thumb->w_image), "enter-notify-event", G_CALLBACK(_event_main_enter), thumb);
    gtk_widget_show(thumb->w_image);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_image);

    // we need to squeeze 5 stars + 1 reject + 1 colorlabels symbols on a thumbnail width
    // stars + reject having a width of 2 * r1 and spaced by r1 => 18 * r1
    // colorlabels => 3 * r1 + space r1
    // inner margins are 0.045 * width
    const float r1 = fminf(DT_PIXEL_APPLY_DPI(20.0f) / 2.0f, 0.91 * thumb->width / 22.0f);

    // the infos background
    thumb->w_bottom_eb = gtk_event_box_new();
    g_signal_connect(G_OBJECT(thumb->w_bottom_eb), "enter-notify-event", G_CALLBACK(_event_bottom_enter), thumb);
    gtk_widget_set_valign(thumb->w_bottom_eb, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_bottom_eb, GTK_ALIGN_CENTER);
    thumb->w_bottom = gtk_label_new("");
    gtk_widget_set_name(thumb->w_bottom_eb, "thumb_bottom");
    gtk_widget_set_size_request(thumb->w_bottom, thumb->width,
                                0.147125 * thumb->height); // TODO Why this hardcoded ratio ?  prefer something
                                                           // dependent of fontsize ?
    gtk_widget_set_name(thumb->w_bottom, "thumb_bottom_label");
    gtk_widget_show(thumb->w_bottom);
    gtk_container_add(GTK_CONTAINER(thumb->w_bottom_eb), thumb->w_bottom);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_bottom_eb);

    // the reject icon
    thumb->w_reject = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_reject, 0, NULL);
    gtk_widget_set_name(thumb->w_reject, "thumb_reject");
    gtk_widget_set_size_request(thumb->w_reject, 3.0 * r1, 3.0 * r1);
    gtk_widget_set_valign(thumb->w_reject, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_reject, GTK_ALIGN_START);
    gtk_widget_set_margin_start(thumb->w_reject, 0.045 * thumb->width - r1 * 0.75);
    gtk_widget_set_margin_bottom(thumb->w_reject, 0.045 * thumb->width - r1 * 0.75);
    g_signal_connect(G_OBJECT(thumb->w_reject), "enter-notify-event", G_CALLBACK(_event_bottom_enter), thumb);
    g_signal_connect(G_OBJECT(thumb->w_reject), "button-release-event", G_CALLBACK(_event_rating_release), thumb);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_reject);

    // the stars
    for(int i = 0; i < 5; i++)
    {
      thumb->w_stars[i] = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_star, 0, NULL);
      gtk_widget_set_size_request(thumb->w_stars[i], 3.0 * r1, 3.0 * r1);
      g_signal_connect(G_OBJECT(thumb->w_stars[i]), "enter-notify-event", G_CALLBACK(_event_star_enter), thumb);
      g_signal_connect(G_OBJECT(thumb->w_stars[i]), "leave-notify-event", G_CALLBACK(_event_star_leave), thumb);
      g_signal_connect(G_OBJECT(thumb->w_stars[i]), "button-release-event", G_CALLBACK(_event_rating_release),
                       thumb);
      gtk_widget_set_name(thumb->w_stars[i], "thumb_star");
      gtk_widget_set_valign(thumb->w_stars[i], GTK_ALIGN_END);
      gtk_widget_set_halign(thumb->w_stars[i], GTK_ALIGN_START);
      gtk_widget_set_margin_bottom(thumb->w_stars[i], 0.045 * thumb->width - r1 * 0.75);
      gtk_widget_set_margin_start(thumb->w_stars[i], (thumb->width - 15.0 * r1) * 0.5 + i * 3.0 * r1);
      gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_stars[i]);
    }

    // the color labels
    thumb->w_color = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_label_flower,
                                             CPF_DO_NOT_USE_BORDER | thumb->colorlabels, NULL);
    gtk_widget_set_name(thumb->w_color, "thumb_colorlabels");
    gtk_widget_set_size_request(thumb->w_color, 3.0 * r1, 3.0 * r1);
    gtk_widget_set_valign(thumb->w_color, GTK_ALIGN_END);
    gtk_widget_set_halign(thumb->w_color, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(thumb->w_color, 0.045 * thumb->width);
    gtk_widget_set_margin_end(thumb->w_color, 0.045 * thumb->width);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_color);

    // the local copy indicator
    thumb->w_local_copy = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_local_copy, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_local_copy, "thumb_localcopy");
    gtk_widget_set_size_request(thumb->w_local_copy, 2.0 * r1, 2.0 * r1);
    gtk_widget_set_valign(thumb->w_local_copy, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_local_copy, GTK_ALIGN_END);
    gtk_widget_set_margin_top(thumb->w_local_copy, 1);
    gtk_widget_set_margin_end(thumb->w_local_copy, 1); // TODO : extract border size from css
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_local_copy);

    // the altered icon
    thumb->w_altered = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_altered, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_altered, "thumb_altered");
    gtk_widget_set_size_request(thumb->w_altered, 2.0 * r1, 2.0 * r1);
    gtk_widget_set_valign(thumb->w_altered, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_altered, GTK_ALIGN_END);
    gtk_widget_set_margin_top(thumb->w_altered, 0.045 * thumb->width);
    gtk_widget_set_margin_end(thumb->w_altered, 0.045 * thumb->width);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_altered);

    // the group bouton
    thumb->w_group = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_grouping, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_group, "thumb_group");
    g_signal_connect(G_OBJECT(thumb->w_group), "button-release-event", G_CALLBACK(_event_grouping_release), thumb);
    gtk_widget_set_size_request(thumb->w_group, 2.0 * r1, 2.0 * r1);
    gtk_widget_set_valign(thumb->w_group, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_group, GTK_ALIGN_END);
    gtk_widget_set_margin_top(thumb->w_group, 0.045 * thumb->width);
    gtk_widget_set_margin_end(thumb->w_group, 0.045 * thumb->width + 3.0 * r1);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_group);

    // the sound icon
    thumb->w_audio = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_audio, CPF_DO_NOT_USE_BORDER, NULL);
    gtk_widget_set_name(thumb->w_audio, "thumb_audio");
    g_signal_connect(G_OBJECT(thumb->w_audio), "button-release-event", G_CALLBACK(_event_audio_release), thumb);
    gtk_widget_set_size_request(thumb->w_audio, 2.0 * r1, 2.0 * r1);
    gtk_widget_set_valign(thumb->w_audio, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_audio, GTK_ALIGN_END);
    gtk_widget_set_margin_top(thumb->w_audio, 0.045 * thumb->width);
    gtk_widget_set_margin_end(thumb->w_audio, 0.045 * thumb->width + 6.0 * r1);
    gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_audio);
  }
  gtk_widget_show(thumb->w_main);
  g_object_ref(G_OBJECT(thumb->w_main));
  return thumb->w_main;
}

dt_thumbnail_t *dt_thumbnail_new(int width, int height, int imgid, int rowid)
{
  dt_thumbnail_t *thumb = calloc(1, sizeof(dt_thumbnail_t));
  thumb->width = width;
  thumb->height = height;
  thumb->imgid = imgid;
  thumb->rowid = rowid;

  // we read and cache all the infos from dt_image_t that we need
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, thumb->imgid, 'r');
  if(img)
  {
    thumb->filename = g_strdup(img->filename);
    dt_image_print_exif(img, thumb->info_line, sizeof(thumb->info_line));
    thumb->has_audio = (img->flags & DT_IMAGE_HAS_WAV);
    thumb->has_localcopy = (img->flags & DT_IMAGE_LOCAL_COPY);

    dt_image_cache_read_release(darktable.image_cache, img);
  }

  // we read all other infos
  _image_get_infos(thumb);

  // we create the widget
  dt_thumbnail_create_widget(thumb);

  // let's see if the images are selected or active
  _dt_active_images_callback(NULL, thumb);
  _dt_selection_changed_callback(NULL, thumb);

  return thumb;
}

void dt_thumbnail_destroy(dt_thumbnail_t *thumb)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_dt_selection_changed_callback), thumb);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_dt_active_images_callback), thumb);
  if(thumb->img_surf) cairo_surface_destroy(thumb->img_surf);
  if(thumb->w_main) gtk_widget_destroy(thumb->w_main);
  if(thumb->filename) g_free(thumb->filename);

  free(thumb);
}

void dt_thumbnail_resize(dt_thumbnail_t *thumb, int width, int height)
{
  // new size unit
  const float r1 = fminf(DT_PIXEL_APPLY_DPI(20.0f) / 2.0f, 0.91 * width / 20.0f);

  // widget resizing
  gtk_widget_set_size_request(thumb->w_main, width, height);
  gtk_widget_set_size_request(thumb->w_bottom, width - 2 * DT_PIXEL_APPLY_DPI(1.0), 0.147125 * height);
  gtk_widget_set_size_request(thumb->w_reject, 4.0 * r1, 4.0 * r1);
  gtk_widget_set_margin_start(thumb->w_reject, 0.045 * width - r1);
  gtk_widget_set_margin_bottom(thumb->w_reject, 0.045 * width - r1);
  for(int i = 0; i < 4; i++)
  {
    gtk_widget_set_size_request(thumb->w_stars[i], 4.0 * r1, 4.0 * r1);
    gtk_widget_set_margin_bottom(thumb->w_stars[i], 0.045 * width - r1);
    gtk_widget_set_margin_start(thumb->w_stars[i], (thumb->width - 16.0 * r1) * 0.5 + i * 4.0 * r1);
  }

  // update values
  thumb->width = width;
  thumb->height = height;

  // reset surface
  if(thumb->img_surf) cairo_surface_destroy(thumb->img_surf);
  thumb->img_surf = NULL;
}

void dt_thumbnail_set_group_border(dt_thumbnail_t *thumb, dt_thumbnail_border_t border)
{
  GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_back);
  if(border == DT_THUMBNAIL_BORDER_NONE)
  {
    gtk_style_context_remove_class(context, "group_border_left");
    gtk_style_context_remove_class(context, "group_border_top");
    gtk_style_context_remove_class(context, "group_border_right");
    gtk_style_context_remove_class(context, "group_border_bottom");
    thumb->group_borders = DT_THUMBNAIL_BORDER_NONE;
    return;
  }
  else if(border & DT_THUMBNAIL_BORDER_LEFT)
    gtk_style_context_add_class(context, "group_border_left");
  else if(border & DT_THUMBNAIL_BORDER_TOP)
    gtk_style_context_add_class(context, "group_border_top");
  else if(border & DT_THUMBNAIL_BORDER_RIGHT)
    gtk_style_context_add_class(context, "group_border_right");
  else if(border & DT_THUMBNAIL_BORDER_BOTTOM)
    gtk_style_context_add_class(context, "group_border_bottom");

  thumb->group_borders |= border;
}

void dt_thumbnail_set_mouseover(dt_thumbnail_t *thumb, gboolean over)
{
  if(thumb->mouse_over == over) return;
  thumb->mouse_over = over;
  _thumb_update_icons(thumb);

  if(!thumb->mouse_over) _set_flag(thumb->w_bottom_eb, GTK_STATE_FLAG_PRELIGHT, FALSE);
  gtk_widget_queue_draw(thumb->w_main);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;