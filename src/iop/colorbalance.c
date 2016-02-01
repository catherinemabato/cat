/*
http://www.youtube.com/watch?v=JVoUgR6bhBc
 */

/*
    This file is part of darktable,
    copyright (c) 2013 tobias ellinghaus.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
// our includes go first:
#include "develop/imageop.h"
#include "bauhaus/bauhaus.h"
#include "dtgtk/drawingarea.h"
#include "gui/gtk.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "common/sse.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// these are not in a state to be useful. but they look nice. too bad i couldn't map the enhanced mode with
// negative values to the wheels :(
// #define SHOW_COLOR_WHEELS

DT_MODULE_INTROSPECTION(1, dt_iop_colorbalance_params_t)

/*

  Meaning of the values:
   0 --> 100%
  -1 -->   0%
   1 --> 200%
 */


typedef enum _colorbalance_channel_t
{
  CHANNEL_FACTOR = 0,
  CHANNEL_RED,
  CHANNEL_GREEN,
  CHANNEL_BLUE,
  CHANNEL_SIZE
} _colorbalance_channel_t;

typedef struct dt_iop_colorbalance_params_t
{
  float lift[CHANNEL_SIZE], gamma[CHANNEL_SIZE], gain[CHANNEL_SIZE];
} dt_iop_colorbalance_params_t;

typedef struct dt_iop_colorbalance_gui_data_t
{
  GtkWidget *lift_r, *lift_g, *lift_b, *lift_factor;
  GtkWidget *gamma_r, *gamma_g, *gamma_b, *gamma_factor;
  GtkWidget *gain_r, *gain_g, *gain_b, *gain_factor;
} dt_iop_colorbalance_gui_data_t;

typedef struct dt_iop_colorbalance_data_t
{
  float lift[CHANNEL_SIZE], gamma[CHANNEL_SIZE], gain[CHANNEL_SIZE];
} dt_iop_colorbalance_data_t;

typedef struct dt_iop_colorbalance_global_data_t
{
  int kernel_colorbalance;
} dt_iop_colorbalance_global_data_t;

const char *name()
{
  return _("color balance");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int groups()
{
  return IOP_GROUP_COLOR;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorbalance_data_t *d = (dt_iop_colorbalance_data_t *)piece->data;
  const int ch = piece->colors;

  // these are RGB values!
  const __m128 lift = { 2.0 - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                        2.0 - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                        2.0 - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]) },
              gamma = { d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                        d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                        d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR] },
              gamma_inv = { (gamma[0] != 0.0) ? 1.0 / gamma[0] : 1000000.0,
                            (gamma[1] != 0.0) ? 1.0 / gamma[1] : 1000000.0,
                            (gamma[2] != 0.0) ? 1.0 / gamma[2] : 1000000.0 },
              gain = { d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                       d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                       d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR] };

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(i, o, roi_in, roi_out)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = ((float *)i) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)o) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
    {
      // transform the pixel to sRGB:
      // Lab -> XYZ
      __m128 Lab = _mm_load_ps(in);
      __m128 XYZ = dt_Lab_to_XYZ_SSE(Lab);
      // XYZ -> sRGB
      __m128 rgb = dt_XYZ_to_sRGB_SSE(XYZ);

      // do the calculation in RGB space
      __m128 one = _mm_set1_ps(1.0);
      __m128 tmp = (((rgb - one) * lift) + one) * gain;
      tmp = _mm_max_ps(tmp, _mm_setzero_ps());
      rgb = _mm_pow_ps(tmp, gamma_inv);

      // transform the result back to Lab
      // sRGB -> XYZ
      XYZ = dt_sRGB_to_XYZ_SSE(rgb);
      // XYZ -> Lab
      __m128 outv = dt_XYZ_to_Lab_SSE(XYZ);
      _mm_store_ps(out, outv);
    }
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorbalance_data_t *d = (dt_iop_colorbalance_data_t *)piece->data;
  dt_iop_colorbalance_global_data_t *gd = (dt_iop_colorbalance_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float lift[4] = { 2.0f - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                          2.0f - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                          2.0f - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]), 0.0f },
              gamma[4] = { d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                           d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                           d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR], 0.0f },
              gamma_inv[4] = { (gamma[0] != 0.0f) ? 1.0f / gamma[0] : 1000000.0f,
                               (gamma[1] != 0.0f) ? 1.0f / gamma[1] : 1000000.0f,
                               (gamma[2] != 0.0f) ? 1.0f / gamma[2] : 1000000.0f, 0.0f },
              gain[4] = { d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                          d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                          d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR], 0.0f };


  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 4, 4 * sizeof(float), (void *)&lift);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 5, 4 * sizeof(float), (void *)&gain);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 6, 4 * sizeof(float), (void *)&gamma_inv);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorbalance, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorbalance] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colorbalance_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colorbalance_params_t));
  module->default_enabled = 0;
  module->priority = 400; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colorbalance_params_t);
  module->gui_data = NULL;
  dt_iop_colorbalance_params_t tmp = (dt_iop_colorbalance_params_t){ { 1.0f, 1.0f, 1.0f, 1.0f },
                                                                     { 1.0f, 1.0f, 1.0f, 1.0f },
                                                                     { 1.0f, 1.0f, 1.0f, 1.0f } };

  memcpy(module->params, &tmp, sizeof(dt_iop_colorbalance_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorbalance_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_colorbalance_global_data_t *gd
      = (dt_iop_colorbalance_global_data_t *)malloc(sizeof(dt_iop_colorbalance_global_data_t));
  module->data = gd;
  gd->kernel_colorbalance = dt_opencl_create_kernel(program, "colorbalance");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorbalance_global_data_t *gd = (dt_iop_colorbalance_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorbalance);
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalance_data_t *d = (dt_iop_colorbalance_data_t *)(piece->data);
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)p1;

  for(int i = 0; i < 4; i++)
  {
    d->lift[i] = p->lift[i];
    d->gamma[i] = p->gamma[i];
    d->gain[i] = p->gain[i];
  }
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;

  dt_bauhaus_slider_set(g->lift_factor, p->lift[CHANNEL_FACTOR] - 1.0f);
  dt_bauhaus_slider_set(g->lift_r, p->lift[CHANNEL_RED] - 1.0f);
  dt_bauhaus_slider_set(g->lift_g, p->lift[CHANNEL_GREEN] - 1.0f);
  dt_bauhaus_slider_set(g->lift_b, p->lift[CHANNEL_BLUE] - 1.0f);

  dt_bauhaus_slider_set(g->gamma_factor, p->gamma[CHANNEL_FACTOR] - 1.0f);
  dt_bauhaus_slider_set(g->gamma_r, p->gamma[CHANNEL_RED] - 1.0f);
  dt_bauhaus_slider_set(g->gamma_g, p->gamma[CHANNEL_GREEN] - 1.0f);
  dt_bauhaus_slider_set(g->gamma_b, p->gamma[CHANNEL_BLUE] - 1.0f);

  dt_bauhaus_slider_set(g->gain_factor, p->gain[CHANNEL_FACTOR] - 1.0f);
  dt_bauhaus_slider_set(g->gain_r, p->gain[CHANNEL_RED] - 1.0f);
  dt_bauhaus_slider_set(g->gain_g, p->gain[CHANNEL_GREEN] - 1.0f);
  dt_bauhaus_slider_set(g->gain_b, p->gain[CHANNEL_BLUE] - 1.0f);
}

static void lift_factor_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->lift[CHANNEL_FACTOR] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void lift_red_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->lift[CHANNEL_RED] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void lift_green_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->lift[CHANNEL_GREEN] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void lift_blue_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->lift[CHANNEL_BLUE] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void gamma_factor_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gamma[CHANNEL_FACTOR] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gamma_red_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gamma[CHANNEL_RED] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gamma_green_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gamma[CHANNEL_GREEN] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gamma_blue_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gamma[CHANNEL_BLUE] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void gain_factor_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gain[CHANNEL_FACTOR] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gain_red_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gain[CHANNEL_RED] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gain_green_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gain[CHANNEL_GREEN] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gain_blue_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gain[CHANNEL_BLUE] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

#ifdef SHOW_COLOR_WHEELS
static gboolean dt_iop_area_draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  float flt_bg = darktable.bauhaus->bg_normal;
  if(gtk_widget_get_state(GTK_WIDGET(widget)) == GTK_STATE_SELECTED) flt_bg = darktable.bauhaus->bg_focus;
  float flt_dark = flt_bg / 1.5, flt_light = flt_bg * 1.5;

  uint32_t bg = ((255 << 24) | ((int)floor(flt_bg * 255 + 0.5) << 16) | ((int)floor(flt_bg * 255 + 0.5) << 8)
                 | (int)floor(flt_bg * 255 + 0.5));
  // bg = 0xffffffff;
  //   uint32_t dark = ((255 << 24) |
  //                  ((int)floor(flt_dark * 255 + 0.5) << 16) |
  //                  ((int)floor(flt_dark * 255 + 0.5) << 8) |
  //                  (int)floor(flt_dark * 255 + 0.5));
  uint32_t light = ((255 << 24) | ((int)floor(flt_light * 255 + 0.5) << 16)
                    | ((int)floor(flt_light * 255 + 0.5) << 8) | (int)floor(flt_light * 255 + 0.5));

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  if(width % 2 == 0) width--;
  if(height % 2 == 0) height--;
  double center_x = (float)width / 2.0, center_y = (float)height / 2.0;
  double diameter = MIN(width, height) - 4;
  double r_outside = diameter / 2.0, r_inside = r_outside * 0.87;
  double r_outside_2 = r_outside * r_outside, r_inside_2 = r_inside * r_inside;

  // clear the background
  cairo_set_source_rgb(cr, flt_bg, flt_bg, flt_bg);
  cairo_paint(cr);

  /* Create an image initialized with the ring colors */
  gint stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);
  guint32 *buf = (guint32 *)malloc(sizeof(guint32) * height * stride / 4);

  for(int y = 0; y < height; y++)
  {
    guint32 *p = buf + y * width;

    double dy = -(y + 0.5 - center_y);

    for(int x = 0; x < width; x++)
    {
      double dx = x + 0.5 - center_x;
      double dist = dx * dx + dy * dy;
      if(dist < r_inside_2 || dist > r_outside_2)
      {
        uint32_t col = bg;
        if((abs(dx) < 1 && abs(dy) < 3) || (abs(dx) < 3 && abs(dy) < 1)) col = light;
        *p++ = col;
        continue;
      }

      double angle = atan2(dy, dx) - M_PI_2;
      if(angle < 0.0) angle += 2.0 * M_PI;

      double hue = angle / (2.0 * M_PI);

      float rgb[3];
      hsl2rgb(rgb, hue, 1.0, 0.5);

      *p++ = (((int)floor(rgb[0] * 255 + 0.5) << 16) | ((int)floor(rgb[1] * 255 + 0.5) << 8)
              | (int)floor(rgb[2] * 255 + 0.5));
    }
  }

  cairo_surface_t *source
      = cairo_image_surface_create_for_data((unsigned char *)buf, CAIRO_FORMAT_RGB24, width, height, stride);

  cairo_set_source_surface(cr, source, 0.0, 0.0);
  cairo_paint(cr);
  free(buf);

  // draw border
  float line_width = 1;
  cairo_set_line_width(cr, line_width);

  cairo_set_source_rgb(cr, flt_bg, flt_bg, flt_bg);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, r_outside, 0.0, 2.0 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, center_x, center_y, r_inside, 0.0, 2.0 * M_PI);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, flt_dark, flt_dark, flt_dark);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, r_outside, M_PI, 1.5 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, center_x, center_y, r_inside, 0.0, 0.5 * M_PI);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, flt_light, flt_light, flt_light);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, r_outside, 0.0, 0.5 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, center_x, center_y, r_inside, M_PI, 1.5 * M_PI);
  cairo_stroke(cr);

  // draw selector
  double r = 255 / 255.0, g = 155 / 255.0, b = 40 / 255.0;
  double h, s, v;

  gtk_rgb_to_hsv(r, g, b, &h, &s, &v);

  cairo_save(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.7);

  cairo_translate(cr, center_x, center_y);
  cairo_rotate(cr, h * 2.0 * M_PI - M_PI_2);

  cairo_arc(cr, r_inside * v, 0.0, 3.0, 0, 2.0 * M_PI);
  cairo_stroke(cr);

  cairo_restore(cr);

  cairo_surface_destroy(source);

  return TRUE;
}
#endif

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorbalance_gui_data_t));
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);


  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

#ifdef SHOW_COLOR_WHEELS
  GtkWidget *area = dtgtk_drawing_area_new_with_aspect_ratio(1.0);
  gtk_box_pack_start(GTK_BOX(hbox), area, TRUE, TRUE, 0);

  //   gtk_widget_add_events(g->area,
  //                         GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK |
  //                         GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(area), "draw", G_CALLBACK(dt_iop_area_draw), self);
  //   g_signal_connect (G_OBJECT (area), "button-press-event",
  //                     G_CALLBACK (dt_iop_colorbalance_button_press), self);
  //   g_signal_connect (G_OBJECT (area), "motion-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_motion_notify), self);
  //   g_signal_connect (G_OBJECT (area), "leave-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_leave_notify), self);

  area = dtgtk_drawing_area_new_with_aspect_ratio(1.0);
  gtk_box_pack_start(GTK_BOX(hbox), area, TRUE, TRUE, 0);

  //   gtk_widget_add_events(g->area,
  //                         GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK |
  //                         GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(area), "draw", G_CALLBACK(dt_iop_area_draw), self);
  //   g_signal_connect (G_OBJECT (area), "button-press-event",
  //                     G_CALLBACK (dt_iop_colorbalance_button_press), self);
  //   g_signal_connect (G_OBJECT (area), "motion-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_motion_notify), self);
  //   g_signal_connect (G_OBJECT (area), "leave-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_leave_notify), self);

  area = dtgtk_drawing_area_new_with_aspect_ratio(1.0);
  gtk_box_pack_start(GTK_BOX(hbox), area, TRUE, TRUE, 0);

  //   gtk_widget_add_events(g->area,
  //                         GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK |
  //                         GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(area), "draw", G_CALLBACK(dt_iop_area_draw), self);
//   g_signal_connect (G_OBJECT (area), "button-press-event",
//                     G_CALLBACK (dt_iop_colorbalance_button_press), self);
//   g_signal_connect (G_OBJECT (area), "motion-notify-event",
//                     G_CALLBACK (dt_iop_colorbalance_motion_notify), self);
//   g_signal_connect (G_OBJECT (area), "leave-notify-event",
//                     G_CALLBACK (dt_iop_colorbalance_leave_notify), self);
#endif

  /* lift */
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("lift")), FALSE, FALSE, 5);

  g->lift_factor
      = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->lift[CHANNEL_FACTOR] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->lift_factor, 0.0, 0.0, 0.0, 0.0);
  dt_bauhaus_slider_set_stop(g->lift_factor, 1.0, 1.0, 1.0, 1.0);
  g_object_set(g->lift_factor, "tooltip-text", _("factor of lift"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->lift_factor, _("lift"), _("factor"));
  g_signal_connect(G_OBJECT(g->lift_factor), "value-changed", G_CALLBACK(lift_factor_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lift_factor, TRUE, TRUE, 0);

  g->lift_r = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->lift[CHANNEL_RED] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->lift_r, 0.0, 0.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->lift_r, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->lift_r, 1.0, 1.0, 0.0, 0.0);
  g_object_set(g->lift_r, "tooltip-text", _("factor of red for lift"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->lift_r, _("lift"), _("red"));
  g_signal_connect(G_OBJECT(g->lift_r), "value-changed", G_CALLBACK(lift_red_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lift_r, TRUE, TRUE, 0);

  g->lift_g = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->lift[CHANNEL_GREEN] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->lift_g, 0.0, 1.0, 0.0, 1.0);
  dt_bauhaus_slider_set_stop(g->lift_g, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->lift_g, 1.0, 0.0, 1.0, 0.0);
  g_object_set(g->lift_g, "tooltip-text", _("factor of green for lift"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->lift_g, _("lift"), _("green"));
  g_signal_connect(G_OBJECT(g->lift_g), "value-changed", G_CALLBACK(lift_green_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lift_g, TRUE, TRUE, 0);

  g->lift_b = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->lift[CHANNEL_BLUE] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->lift_b, 0.0, 1.0, 1.0, 0.0);
  dt_bauhaus_slider_set_stop(g->lift_b, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->lift_b, 1.0, 0.0, 0.0, 1.0);
  g_object_set(g->lift_b, "tooltip-text", _("factor of blue for lift"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->lift_b, _("lift"), _("blue"));
  g_signal_connect(G_OBJECT(g->lift_b), "value-changed", G_CALLBACK(lift_blue_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lift_b, TRUE, TRUE, 0);

  /* gamma */
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("gamma")), FALSE, FALSE, 5);

  g->gamma_factor
      = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->gamma[CHANNEL_FACTOR] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->gamma_factor, 0.0, 0.0, 0.0, 0.0);
  dt_bauhaus_slider_set_stop(g->gamma_factor, 1.0, 1.0, 1.0, 1.0);
  g_object_set(g->gamma_factor, "tooltip-text", _("factor of gamma"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->gamma_factor, _("gamma"), _("factor"));
  g_signal_connect(G_OBJECT(g->gamma_factor), "value-changed", G_CALLBACK(gamma_factor_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->gamma_factor, TRUE, TRUE, 0);

  g->gamma_r = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->gamma[CHANNEL_RED] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->gamma_r, 0.0, 0.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gamma_r, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gamma_r, 1.0, 1.0, 0.0, 0.0);
  g_object_set(g->gamma_r, "tooltip-text", _("factor of red for gamma"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->gamma_r, _("gamma"), _("red"));
  g_signal_connect(G_OBJECT(g->gamma_r), "value-changed", G_CALLBACK(gamma_red_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->gamma_r, TRUE, TRUE, 0);

  g->gamma_g = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->gamma[CHANNEL_GREEN] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->gamma_g, 0.0, 1.0, 0.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gamma_g, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gamma_g, 1.0, 0.0, 1.0, 0.0);
  g_object_set(g->gamma_g, "tooltip-text", _("factor of green for gamma"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->gamma_g, _("gamma"), _("green"));
  g_signal_connect(G_OBJECT(g->gamma_g), "value-changed", G_CALLBACK(gamma_green_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->gamma_g, TRUE, TRUE, 0);

  g->gamma_b = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->gamma[CHANNEL_BLUE] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->gamma_b, 0.0, 1.0, 1.0, 0.0);
  dt_bauhaus_slider_set_stop(g->gamma_b, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gamma_b, 1.0, 0.0, 0.0, 1.0);
  g_object_set(g->gamma_b, "tooltip-text", _("factor of blue for gamma"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->gamma_b, _("gamma"), _("blue"));
  g_signal_connect(G_OBJECT(g->gamma_b), "value-changed", G_CALLBACK(gamma_blue_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->gamma_b, TRUE, TRUE, 0);

  /* gain */
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("gain")), FALSE, FALSE, 5);

  g->gain_factor
      = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->gain[CHANNEL_FACTOR] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->gain_factor, 0.0, 0.0, 0.0, 0.0);
  dt_bauhaus_slider_set_stop(g->gain_factor, 1.0, 1.0, 1.0, 1.0);
  g_object_set(g->gain_factor, "tooltip-text", _("factor of gain"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->gain_factor, _("gain"), _("factor"));
  g_signal_connect(G_OBJECT(g->gain_factor), "value-changed", G_CALLBACK(gain_factor_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->gain_factor, TRUE, TRUE, 0);

  g->gain_r = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->gain[CHANNEL_RED] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->gain_r, 0.0, 0.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gain_r, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gain_r, 1.0, 1.0, 0.0, 0.0);
  g_object_set(g->gain_r, "tooltip-text", _("factor of red for gain"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->gain_r, _("gain"), _("red"));
  g_signal_connect(G_OBJECT(g->gain_r), "value-changed", G_CALLBACK(gain_red_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->gain_r, TRUE, TRUE, 0);

  g->gain_g = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->gain[CHANNEL_GREEN] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->gain_g, 0.0, 1.0, 0.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gain_g, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gain_g, 1.0, 0.0, 1.0, 0.0);
  g_object_set(g->gain_g, "tooltip-text", _("factor of green for gain"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->gain_g, _("gain"), _("green"));
  g_signal_connect(G_OBJECT(g->gain_g), "value-changed", G_CALLBACK(gain_green_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->gain_g, TRUE, TRUE, 0);

  g->gain_b = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->gain[CHANNEL_BLUE] - 1.0f, 3);
  dt_bauhaus_slider_set_stop(g->gain_b, 0.0, 1.0, 1.0, 0.0);
  dt_bauhaus_slider_set_stop(g->gain_b, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gain_b, 1.0, 0.0, 0.0, 1.0);
  g_object_set(g->gain_b, "tooltip-text", _("factor of blue for gain"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->gain_b, _("gain"), _("blue"));
  g_signal_connect(G_OBJECT(g->gain_b), "value-changed", G_CALLBACK(gain_blue_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->gain_b, TRUE, TRUE, 0);
}

void gui_cleanup(dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

/** additional, optional callbacks to capture darkroom center events. */
// int mouse_moved(dt_iop_module_t *self, double x, double y, double pressure, int which);
// int button_pressed(dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
// uint32_t state);
// int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
// int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
