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

#ifndef THUMBNAIL_H
#define THUMBNAIL_H

#include <glib.h>
#include <gtk/gtk.h>

#define MAX_STARS 5
#define OVERLAY_BLOCK_TIMEOUT 2

typedef enum dt_thumbnail_border_t
{
  DT_THUMBNAIL_BORDER_NONE = 0,
  DT_THUMBNAIL_BORDER_LEFT = 1 << 0,
  DT_THUMBNAIL_BORDER_TOP = 1 << 1,
  DT_THUMBNAIL_BORDER_RIGHT = 1 << 2,
  DT_THUMBNAIL_BORDER_BOTTOM = 1 << 3,
} dt_thumbnail_border_t;

typedef enum dt_thumbnail_overlay_t
{
  DT_THUMBNAIL_OVERLAYS_NONE,
  DT_THUMBNAIL_OVERLAYS_HOVER_NORMAL,
  DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED,
  DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL,
  DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED,
  DT_THUMBNAIL_OVERLAYS_MIXED,
  DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK
} dt_thumbnail_overlay_t;

typedef enum dt_thumbnail_selection_mode_t
{
  DT_THUMBNAIL_SEL_MODE_NORMAL = 0, // user can change selection with normal mouse click (+CTRL or +SHIFT)
  DT_THUMBNAIL_SEL_MODE_DISABLED,   // user can't change selection with mouse
  DT_THUMBNAIL_SEL_MODE_MOD_ONLY    // user can only change selection with mouse AND CTRL or SHIFT
} dt_thumbnail_selection_mode_t;

typedef struct
{
  int imgid, rowid;
  int width, height;         // current thumb size (with the background and the border)
  int x, y;                  // current position at screen
  int img_width, img_height; // current image only size

  gboolean mouse_over;
  gboolean selected;
  gboolean active; // used for filmstrip to mark images worked on

  int rating;
  int colorlabels;
  gchar *filename;
  gchar *info_line;
  gboolean is_altered;
  gboolean has_audio;
  gboolean is_grouped;
  gboolean is_bw;
  gboolean is_hdr;
  gboolean has_localcopy;
  int groupid;

  // all widget components
  GtkWidget *w_main;               // GtkOverlay -- contains all others widgets
  GtkWidget *w_back;               // GtkEventBox -- thumbnail background
  GtkWidget *w_ext;                // GtkLabel -- thumbnail extension

  GtkWidget *w_image_box;
  GtkWidget *w_image;        // GtkDrawingArea -- thumbnail image
  GtkBorder *img_margin;     // in percentage of the main widget size
  cairo_surface_t *img_surf; // cached surface at exact dimensions to speed up redraw
  gboolean img_surf_preview; // if TRUE, the image is originated from preview pipe
  gboolean img_surf_dirty;   // if TRUE, we need to recreate the surface on next drawing code
  gboolean img_surf_pos_changed; // if TRUE, we need to the surface on next drawing code

  GtkWidget *w_bottom_eb; // GtkEventBox -- background of the bottom infos area (contains w_bottom)
  GtkWidget *w_bottom;    // GtkLabel -- text of the bottom infos area, just with #thumb_bottom_ext
  GtkWidget *w_reject;    // GtkDarktableThumbnailBtn -- Reject icon
  GtkWidget *w_stars[MAX_STARS];  // GtkDarktableThumbnailBtn -- Stars icons
  GtkWidget *w_color;     // GtkDarktableThumbnailBtn -- Colorlabels "flower" icon

  GtkWidget *w_local_copy; // GtkDarktableThumbnailBtn -- localcopy triangle
  GtkWidget *w_altered;    // GtkDarktableThumbnailBtn -- Altered icon
  GtkWidget *w_group;      // GtkDarktableThumbnailBtn -- Grouping icon
  GtkWidget *w_audio;      // GtkDarktableThumbnailBtn -- Audio sidecar icon

  gboolean moved; // indicate if the thumb is currently moved (zoomable thumbtable case)

  dt_thumbnail_border_t group_borders; // which group borders should be drawn

  dt_thumbnail_selection_mode_t sel_mode; // do we allow to change selection with mouse ?
  gboolean single_click;                  // do we activate on single or double click ?
  gboolean disable_mouseover;             // do we allow to change mouseoverid by mouse move

  dt_thumbnail_overlay_t over; // type of overlays
  int overlay_timeout_id;

  // specific for culling and preview
  gboolean zoomable;
  double aspect_ratio;

  // global zoom values actually applied to all images in culling table
  float zoom_glob;
  float zx_glob;
  float zy_glob;

  // difference between the global zoom values and the value to apply to this specific thumbnail
  float zoom_delta;
  float zx_delta;
  float zy_delta;

  float zoom_100; // max zoom value

  float w_fit;
  float h_fit;

  gboolean display_focus;
} dt_thumbnail_t;

dt_thumbnail_t *dt_thumbnail_new(int width, int height, int imgid, int rowid, dt_thumbnail_overlay_t over, gboolean zoomable);
void dt_thumbnail_destroy(dt_thumbnail_t *thumb);
GtkWidget *dt_thumbnail_create_widget(dt_thumbnail_t *thumb);
void dt_thumbnail_resize(dt_thumbnail_t *thumb, int width, int height, gboolean force);
void dt_thumbnail_set_group_border(dt_thumbnail_t *thumb, dt_thumbnail_border_t border);
void dt_thumbnail_set_mouseover(dt_thumbnail_t *thumb, gboolean over);

// set if the thumbnail should react (mouse_over) to drag and drop
// note that it's just cosmetic as dropping occurs in thumbtable in any case
void dt_thumbnail_set_drop(dt_thumbnail_t *thumb, gboolean accept_drop);

// update the informations of the image and update icons accordingly
void dt_thumbnail_update_infos(dt_thumbnail_t *thumb);

// force image recomputing
void dt_thumbnail_image_refresh(dt_thumbnail_t *thumb);

// do we need to display simple overlays or extended ?
void dt_thumbnail_set_overlay(dt_thumbnail_t *thumb, dt_thumbnail_overlay_t over);
// force image position refresh (only in the case of zoomed image)
void dt_thumbnail_image_refresh_position(dt_thumbnail_t *thumb);

float dt_thumbnail_get_zoom100(dt_thumbnail_t *thumb);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;