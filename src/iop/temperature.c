/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <assert.h>
#include <lcms2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/opencl.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "dtgtk/expander.h"
#include "external/wb_presets.c"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"

// for Kelvin temperature and bogus WB
#include "common/colorspaces.h"
#include "external/cie_colorimetric_tables.c"

DT_MODULE_INTROSPECTION(3, dt_iop_temperature_params_t)

#define INITIALBLACKBODYTEMPERATURE 4000

#define DT_IOP_LOWEST_TEMPERATURE 1901
#define DT_IOP_HIGHEST_TEMPERATURE 25000

#define DT_IOP_LOWEST_TINT 0.135
#define DT_IOP_HIGHEST_TINT 2.326

#define DT_IOP_NUM_OF_STD_TEMP_PRESETS 4

//storing the last picked color (if any)
static float old[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

static void gui_sliders_update(struct dt_iop_module_t *self);

typedef struct dt_iop_temperature_params_t
{
  float coeffs[4];
} dt_iop_temperature_params_t;

typedef struct dt_iop_temperature_gui_data_t
{
  GtkWidget *scale_k, *scale_tint, *coeff_widgets, *scale_r, *scale_g, *scale_b, *scale_g2;
  GtkWidget *presets;
  GtkWidget *finetune;
  GtkWidget *box_enabled;
  GtkWidget *label_disabled;
  GtkWidget *stack;
  GtkWidget *colorpicker;
  GtkWidget *btn_asshot; //As Shot
  GtkWidget *btn_user;
  GtkWidget *btn_d65;
  GtkWidget *coeffs_expander;
  GtkWidget *coeffs_toggle;
  int preset_cnt;
  int preset_num[50];
  double daylight_wb[4];
  double mod_coeff[4];
  double XYZ_to_CAM[4][3], CAM_to_XYZ[3][4];
  int colored_sliders;
  int blackbody_is_confusing;
  int expand_coeffs;
} dt_iop_temperature_gui_data_t;

typedef struct dt_iop_temperature_data_t
{
  float coeffs[4];
} dt_iop_temperature_data_t;

typedef struct dt_iop_temperature_global_data_t
{
  int kernel_whitebalance_4f;
  int kernel_whitebalance_1f;
  int kernel_whitebalance_1f_xtrans;
} dt_iop_temperature_global_data_t;

typedef struct dt_iop_temperature_preset_data_t
{
  int no_ft_pos;
  int min_ft_pos;
  int max_ft_pos;
} dt_iop_temperature_preset_data_t;

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 2 && new_version == 3)
  {
    typedef struct dt_iop_temperature_params_v2_t
    {
      float temp_out;
      float coeffs[3];
    } dt_iop_temperature_params_v2_t;

    dt_iop_temperature_params_v2_t *o = (dt_iop_temperature_params_v2_t *)old_params;
    dt_iop_temperature_params_t *n = (dt_iop_temperature_params_t *)new_params;

    n->coeffs[0] = o->coeffs[0];
    n->coeffs[1] = o->coeffs[1];
    n->coeffs[2] = o->coeffs[2];
    n->coeffs[3] = NAN;

    return 0;
  }
  return 1;
}

static int ignore_missing_wb(dt_image_t *img)
{
  // Ignore files that end with "-hdr.dng" since these are broken files we
  // generated without any proper WB tagged
  if(g_str_has_suffix(img->filename,"-hdr.dng"))
    return TRUE;

  static const char *const ignored_cameras[] = {
    "Canon PowerShot A610",
    "Canon PowerShot S3 IS",
    "Canon PowerShot A620",
    "Canon PowerShot A720 IS",
    "Canon PowerShot A630",
    "Canon PowerShot A640",
    "Canon PowerShot A650",
    "Canon PowerShot SX110 IS",
    "Mamiya ZD",
    "Canon EOS D2000C",
    "Kodak EOS DCS 1",
    "Kodak DCS560C",
    "Kodak DCS460D",
    "Nikon E5700",
    "Sony DSC-F828",
    "GITUP GIT2",
  };

  for(int i=0; i < sizeof(ignored_cameras)/sizeof(ignored_cameras[1]); i++)
    if(!strcmp(img->camera_makermodel, ignored_cameras[i]))
      return TRUE;

  return FALSE;
}


const char *name()
{
  return C_("modulename", "white balance");
}

int default_group()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_RAW;
}

static gboolean _set_preset_camera(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                   GdkModifierType modifier, gpointer data)
{
  dt_iop_module_t *self = data;
  dt_iop_temperature_gui_data_t *g = self->gui_data;
  dt_bauhaus_combobox_set(g->presets, 0);
  return TRUE;
}

static gboolean _set_preset_camera_neutral(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  dt_iop_module_t *self = data;
  dt_iop_temperature_gui_data_t *g = self->gui_data;
  dt_bauhaus_combobox_set(g->presets, 1);
  return TRUE;
}

static gboolean _set_preset_spot(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                 GdkModifierType modifier, gpointer data)
{
  dt_iop_module_t *self = data;
  dt_iop_temperature_gui_data_t *g = self->gui_data;
  dt_bauhaus_combobox_set(g->presets, 2);
  return TRUE;
}

static gboolean _set_preset_user(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                 GdkModifierType modifier, gpointer data)
{
  dt_iop_module_t *self = data;
  dt_iop_temperature_gui_data_t *g = self->gui_data;
  dt_bauhaus_combobox_set(g->presets, 3);
  return TRUE;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "tint"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "temperature"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "red"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "green"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "blue"));
  dt_accel_register_combobox_iop(self, FALSE, NC_("accel", "presets"));

  dt_accel_register_iop(self, TRUE, NC_("accel", "preset/as shot"), 0, 0);
  dt_accel_register_iop(self, TRUE, NC_("accel", "preset/camera standard D65"), 0, 0);
  dt_accel_register_iop(self, TRUE, NC_("accel", "preset/from image area"), 0, 0);
  dt_accel_register_iop(self, TRUE, NC_("accel", "preset/user modified"), 0, 0);
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "tint", GTK_WIDGET(g->scale_tint));
  dt_accel_connect_slider_iop(self, "temperature", GTK_WIDGET(g->scale_k));
  dt_accel_connect_slider_iop(self, "red", GTK_WIDGET(g->scale_r));
  dt_accel_connect_slider_iop(self, "green", GTK_WIDGET(g->scale_g));
  dt_accel_connect_slider_iop(self, "blue", GTK_WIDGET(g->scale_b));
  dt_accel_connect_slider_iop(self, "green2", GTK_WIDGET(g->scale_g2));
  dt_accel_connect_combobox_iop(self, "presets", GTK_WIDGET(g->presets));

  GClosure *closure;

  closure = g_cclosure_new(G_CALLBACK(_set_preset_camera), (gpointer)self, NULL);
  dt_accel_connect_iop(self, "preset/as shot", closure);

  closure = g_cclosure_new(G_CALLBACK(_set_preset_camera_neutral), (gpointer)self, NULL);
  dt_accel_connect_iop(self, "preset/camera standard D65", closure);

  closure = g_cclosure_new(G_CALLBACK(_set_preset_spot), (gpointer)self, NULL);
  dt_accel_connect_iop(self, "preset/from image area", closure);

  closure = g_cclosure_new(G_CALLBACK(_set_preset_user), (gpointer)self, NULL);
  dt_accel_connect_iop(self, "preset/user modified", closure);
}

/*
 * Spectral power distribution functions
 * https://en.wikipedia.org/wiki/Spectral_power_distribution
 */
typedef double((*spd)(unsigned long int wavelength, double TempK));

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a Blackbody Radiator"
 * http://www.brucelindbloom.com/Eqn_Blackbody.html
 */
static double spd_blackbody(unsigned long int wavelength, double TempK)
{
  // convert wavelength from nm to m
  const long double lambda = (double)wavelength * 1e-9;

/*
 * these 2 constants were computed using following Sage code:
 *
 * (from http://physics.nist.gov/cgi-bin/cuu/Value?h)
 * h = 6.62606957 * 10^-34 # Planck
 * c= 299792458 # speed of light in vacuum
 * k = 1.3806488 * 10^-23 # Boltzmann
 *
 * c_1 = 2 * pi * h * c^2
 * c_2 = h * c / k
 *
 * print 'c_1 = ', c_1, ' ~= ', RealField(128)(c_1)
 * print 'c_2 = ', c_2, ' ~= ', RealField(128)(c_2)
 */

#define c1 3.7417715246641281639549488324352159753e-16L
#define c2 0.014387769599838156481252937624049081933L

  return (double)(c1 / (powl(lambda, 5) * (expl(c2 / (lambda * TempK)) - 1.0L)));

#undef c2
#undef c1
}

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a CIE D-Illuminant"
 * http://www.brucelindbloom.com/Eqn_DIlluminant.html
 * and https://en.wikipedia.org/wiki/Standard_illuminant#Illuminant_series_D
 */
static double spd_daylight(unsigned long int wavelength, double TempK)
{
  cmsCIExyY WhitePoint = { 0.3127, 0.3290, 1.0 };

  /*
   * Bruce Lindbloom, "TempK to xy"
   * http://www.brucelindbloom.com/Eqn_T_to_xy.html
   */
  cmsWhitePointFromTemp(&WhitePoint, TempK);

  const double M = (0.0241 + 0.2562 * WhitePoint.x - 0.7341 * WhitePoint.y),
               m1 = (-1.3515 - 1.7703 * WhitePoint.x + 5.9114 * WhitePoint.y) / M,
               m2 = (0.0300 - 31.4424 * WhitePoint.x + 30.0717 * WhitePoint.y) / M;

  const unsigned long int j
      = ((wavelength - cie_daylight_components[0].wavelength)
         / (cie_daylight_components[1].wavelength - cie_daylight_components[0].wavelength));

  return (cie_daylight_components[j].S[0] + m1 * cie_daylight_components[j].S[1]
          + m2 * cie_daylight_components[j].S[2]);
}

/*
 * Bruce Lindbloom, "Computing XYZ From Spectral Data (Emissive Case)"
 * http://www.brucelindbloom.com/Eqn_Spect_to_XYZ.html
 */
static cmsCIEXYZ spectrum_to_XYZ(double TempK, spd I)
{
  cmsCIEXYZ Source = {.X = 0.0, .Y = 0.0, .Z = 0.0 };

  /*
   * Color matching functions
   * https://en.wikipedia.org/wiki/CIE_1931_color_space#Color_matching_functions
   */
  for(size_t i = 0; i < cie_1931_std_colorimetric_observer_count; i++)
  {
    const unsigned long int lambda = cie_1931_std_colorimetric_observer[0].wavelength
                                     + (cie_1931_std_colorimetric_observer[1].wavelength
                                        - cie_1931_std_colorimetric_observer[0].wavelength) * i;
    const double P = I(lambda, TempK);
    Source.X += P * cie_1931_std_colorimetric_observer[i].xyz.X;
    Source.Y += P * cie_1931_std_colorimetric_observer[i].xyz.Y;
    Source.Z += P * cie_1931_std_colorimetric_observer[i].xyz.Z;
  }

  // normalize so that each component is in [0.0, 1.0] range
  const double _max = MAX(MAX(Source.X, Source.Y), Source.Z);
  Source.X /= _max;
  Source.Y /= _max;
  Source.Z /= _max;

  return Source;
}

// TODO: temperature and tint cannot be disjoined! (here it assumes no tint)
static cmsCIEXYZ temperature_to_XYZ(double TempK)
{
  if(TempK < DT_IOP_LOWEST_TEMPERATURE) TempK = DT_IOP_LOWEST_TEMPERATURE;
  if(TempK > DT_IOP_HIGHEST_TEMPERATURE) TempK = DT_IOP_HIGHEST_TEMPERATURE;

  if(TempK < INITIALBLACKBODYTEMPERATURE)
  {
    // if temperature is less than 4000K we use blackbody,
    // because there will be no Daylight reference below 4000K...
    return spectrum_to_XYZ(TempK, spd_blackbody);
  }
  else
  {
    return spectrum_to_XYZ(TempK, spd_daylight);
  }
}

static cmsCIEXYZ temperature_tint_to_XYZ(double TempK, double tint)
{
  cmsCIEXYZ xyz = temperature_to_XYZ(TempK);

  xyz.Y /= tint; // TODO: This is baaad!

  return xyz;
}

// binary search inversion
static void XYZ_to_temperature(cmsCIEXYZ XYZ, double *TempK, double *tint)
{
  double maxtemp = DT_IOP_HIGHEST_TEMPERATURE, mintemp = DT_IOP_LOWEST_TEMPERATURE;
  cmsCIEXYZ _xyz;

  for(*TempK = (maxtemp + mintemp) / 2.0; (maxtemp - mintemp) > 1.0; *TempK = (maxtemp + mintemp) / 2.0)
  {
    _xyz = temperature_to_XYZ(*TempK);
    if(_xyz.Z / _xyz.X > XYZ.Z / XYZ.X)
      maxtemp = *TempK;
    else
      mintemp = *TempK;
  }

  *tint = (_xyz.Y / _xyz.X) / (XYZ.Y / XYZ.X); // TODO: Fix this to move orthoigonally to planckian locus


  if(*TempK < DT_IOP_LOWEST_TEMPERATURE) *TempK = DT_IOP_LOWEST_TEMPERATURE;
  if(*TempK > DT_IOP_HIGHEST_TEMPERATURE) *TempK = DT_IOP_HIGHEST_TEMPERATURE;
  if(*tint < DT_IOP_LOWEST_TINT) *tint = DT_IOP_LOWEST_TINT;
  if(*tint > DT_IOP_HIGHEST_TINT) *tint = DT_IOP_HIGHEST_TINT;
}

static void xyz2mul(dt_iop_module_t *self, cmsCIEXYZ xyz, double mul[4])
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  double XYZ[3] = { xyz.X, xyz.Y, xyz.Z };

  double CAM[4];
  for(int k = 0; k < 4; k++)
  {
    CAM[k] = 0.0;
    for(int i = 0; i < 3; i++)
    {
      CAM[k] += g->XYZ_to_CAM[k][i] * XYZ[i];
    }
  }

  for(int k = 0; k < 4; k++) mul[k] = 1.0 / CAM[k];
}

static void temp2mul(dt_iop_module_t *self, double TempK, double tint, double mul[4])
{
  cmsCIEXYZ xyz = temperature_to_XYZ(TempK);

  xyz.Y /= tint; // TODO: This is baaad!
  /**
   * TODO:
   * problem here is that tint as it is is just a nasty hack modyfying Y component
   * and therefore changing RGB coefficients in wrong way,
   * because modifying only Y in that way doesn’t move XYZ point orthogonally
   * to planckian locus. That means it actually changes temperature and thus it lies!
   */

  xyz2mul(self, xyz, mul);
}

static cmsCIEXYZ mul2xyz(dt_iop_module_t *self, const float coeffs[4])
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  double CAM[4];
  for(int k = 0; k < 4; k++) CAM[k] = coeffs[k] > 0.0f ? 1.0 / coeffs[k] : 0.0f;

  double XYZ[3];
  for(int k = 0; k < 3; k++)
  {
    XYZ[k] = 0.0;
    for(int i = 0; i < 4; i++)
    {
      XYZ[k] += g->CAM_to_XYZ[k][i] * CAM[i];
    }
  }

  return (cmsCIEXYZ){ XYZ[0], XYZ[1], XYZ[2] };
}

static void mul2temp(dt_iop_module_t *self, float coeffs[4], double *TempK, double *tint)
{
  XYZ_to_temperature(mul2xyz(self, coeffs), TempK, tint);
}

/*
 * interpolate values from p1 and p2 into out.
 */
static void dt_wb_preset_interpolate(const wb_data *const p1, // the smaller tuning
                                     const wb_data *const p2, // the larger tuning (can't be == p1)
                                     wb_data *out)            // has tuning initialized
{
  const double t = CLAMP((double)(out->tuning - p1->tuning) / (double)(p2->tuning - p1->tuning), 0.0, 1.0);
  for(int k = 0; k < 3; k++)
  {
    out->channel[k] = 1.0 / (((1.0 - t) / p1->channel[k]) + (t / p2->channel[k]));
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const uint32_t filters = piece->pipe->dsc.filters;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  const dt_iop_temperature_data_t *const d = (dt_iop_temperature_data_t *)piece->data;

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  if(filters == 9u)
  { // xtrans float mosaiced
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(d, in, out, roi_out, xtrans) \
    schedule(static) \
    collapse(2)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        const size_t p = (size_t)j * roi_out->width + i;
        out[p] = in[p] * d->coeffs[FCxtrans(j, i, roi_out, xtrans)];
      }
    }
  }
  else if(filters)
  { // bayer float mosaiced
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(d, filters, in, out, roi_out) \
    schedule(static) \
    collapse(2)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        const size_t p = (size_t)j * roi_out->width + i;
        out[p] = in[p] * d->coeffs[FC(j + roi_out->y, i + roi_out->x, filters)];
      }
    }
  }
  else
  { // non-mosaiced
    const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(ch, d, in, out, roi_out) \
    schedule(static) \
    collapse(2)
#endif
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      for(int c = 0; c < 3; c++)
      {
        const size_t p = (size_t)k + c;
        out[p] = in[p] * d->coeffs[c];
      }
    }

    if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  }

  piece->pipe->dsc.temperature.enabled = 1;
  for(int k = 0; k < 4; k++)
  {
    piece->pipe->dsc.temperature.coeffs[k] = d->coeffs[k];
    piece->pipe->dsc.processed_maximum[k] = d->coeffs[k] * piece->pipe->dsc.processed_maximum[k];
  }
}

#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                  void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const uint32_t filters = piece->pipe->dsc.filters;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  if(filters == 9u)
  { // xtrans float mosaiced
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ivoid, ovoid, roi_out, xtrans) \
    shared(d) \
    schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)j * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)j * roi_out->width;

      int i = 0;
      const int alignment = ((4 - (j * roi_out->width & (4 - 1))) & (4 - 1));

      // process unaligned pixels
      for(; i < alignment && i < roi_out->width; i++, out++, in++)
        *out = *in * d->coeffs[FCxtrans(j, i, roi_out, xtrans)];

      const __m128 coeffs[3] = {
        _mm_set_ps(d->coeffs[FCxtrans(j, i + 3, roi_out, xtrans)], d->coeffs[FCxtrans(j, i + 2, roi_out, xtrans)],
                   d->coeffs[FCxtrans(j, i + 1, roi_out, xtrans)], d->coeffs[FCxtrans(j, i + 0, roi_out, xtrans)]),
        _mm_set_ps(d->coeffs[FCxtrans(j, i + 7, roi_out, xtrans)], d->coeffs[FCxtrans(j, i + 6, roi_out, xtrans)],
                   d->coeffs[FCxtrans(j, i + 5, roi_out, xtrans)], d->coeffs[FCxtrans(j, i + 4, roi_out, xtrans)]),
        _mm_set_ps(d->coeffs[FCxtrans(j, i + 11, roi_out, xtrans)], d->coeffs[FCxtrans(j, i + 10, roi_out, xtrans)],
                   d->coeffs[FCxtrans(j, i + 9, roi_out, xtrans)], d->coeffs[FCxtrans(j, i + 8, roi_out, xtrans)])
      };

      // process aligned pixels with SSE
      for(int c = 0; c < 3 && i < roi_out->width - (4 - 1); c++, i += 4, in += 4, out += 4)
      {
        __m128 v;

        v = _mm_load_ps(in);
        v = _mm_mul_ps(v, coeffs[c]);
        _mm_stream_ps(out, v);
      }

      // process the rest
      for(; i < roi_out->width; i++, out++, in++) *out = *in * d->coeffs[FCxtrans(j, i, roi_out, xtrans)];
    }
    _mm_sfence();
  }
  else if(filters)
  { // bayer float mosaiced
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(filters, ivoid, ovoid, roi_out) \
    shared(d) \
    schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)j * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)j * roi_out->width;

      int i = 0;
      const int alignment = ((4 - (j * roi_out->width & (4 - 1))) & (4 - 1));

      // process unaligned pixels
      for(; i < alignment && i < roi_out->width; i++, out++, in++)
        *out = *in * d->coeffs[FC(j + roi_out->y, i + roi_out->x, filters)];

      const __m128 coeffs = _mm_set_ps(d->coeffs[FC(j + roi_out->y, roi_out->x + i + 3, filters)],
                                       d->coeffs[FC(j + roi_out->y, roi_out->x + i + 2, filters)],
                                       d->coeffs[FC(j + roi_out->y, roi_out->x + i + 1, filters)],
                                       d->coeffs[FC(j + roi_out->y, roi_out->x + i, filters)]);

      // process aligned pixels with SSE
      for(; i < roi_out->width - (4 - 1); i += 4, in += 4, out += 4)
      {
        const __m128 input = _mm_load_ps(in);

        const __m128 multiplied = _mm_mul_ps(input, coeffs);

        _mm_stream_ps(out, multiplied);
      }

      // process the rest
      for(; i < roi_out->width; i++, out++, in++)
        *out = *in * d->coeffs[FC(j + roi_out->y, i + roi_out->x, filters)];
    }
    _mm_sfence();
  }
  else
  { // non-mosaiced
    const int ch = piece->colors;

    const __m128 coeffs = _mm_set_ps(1.0f, d->coeffs[2], d->coeffs[1], d->coeffs[0]);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ch, coeffs, ivoid, ovoid, roi_out) \
    shared(d) \
    schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;
      for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
      {
        const __m128 input = _mm_load_ps(in);
        const __m128 multiplied = _mm_mul_ps(input, coeffs);
        _mm_stream_ps(out, multiplied);
      }
    }
    _mm_sfence();

    if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  }

  piece->pipe->dsc.temperature.enabled = 1;
  for(int k = 0; k < 4; k++)
  {
    piece->pipe->dsc.temperature.coeffs[k] = d->coeffs[k];
    piece->pipe->dsc.processed_maximum[k] = d->coeffs[k] * piece->pipe->dsc.processed_maximum[k];
  }
}
#endif

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  dt_iop_temperature_global_data_t *gd = (dt_iop_temperature_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  const uint32_t filters = piece->pipe->dsc.filters;
  cl_mem dev_coeffs = NULL;
  cl_mem dev_xtrans = NULL;
  cl_int err = -999;
  int kernel = -1;

  if(filters == 9u)
  {
    kernel = gd->kernel_whitebalance_1f_xtrans;
  }
  else if(filters)
  {
    kernel = gd->kernel_whitebalance_1f;
  }
  else
  {
    kernel = gd->kernel_whitebalance_4f;
  }

  if(filters == 9u)
  {
    dev_xtrans
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
    if(dev_xtrans == NULL) goto error;
  }

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->coeffs);
  if(dev_coeffs == NULL) goto error;

  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&dev_coeffs);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(uint32_t), (void *)&filters);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(uint32_t), (void *)&roi_out->x);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(uint32_t), (void *)&roi_out->y);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), (void *)&dev_xtrans);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_coeffs);
  dt_opencl_release_mem_object(dev_xtrans);

  piece->pipe->dsc.temperature.enabled = 1;
  for(int k = 0; k < 4; k++)
  {
    piece->pipe->dsc.temperature.coeffs[k] = d->coeffs[k];
    piece->pipe->dsc.processed_maximum[k] = d->coeffs[k] * piece->pipe->dsc.processed_maximum[k];
  }
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_coeffs);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_print(DT_DEBUG_OPENCL, "[opencl_white_balance] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)p1;
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;

  if(self->hide_enable_button)
  {
    piece->enabled = 0;
    return;
  }

  for(int k = 0; k < 4; k++) d->coeffs[k] = p->coeffs[k];

  // 4Bayer images not implemented in OpenCL yet
  if(self->dev->image_storage.flags & DT_IMAGE_4BAYER) piece->process_cl_ready = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_temperature_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

int generate_preset_combo(struct dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  int presets_found = 0;

  const char *wb_name = NULL;
  if(!dt_image_is_ldr(&self->dev->image_storage))
    for(int i = 0; i < wb_preset_count; i++)
    {
      if(presets_found >= 50) break;
      if(!strcmp(wb_preset[i].make, self->dev->image_storage.camera_maker)
         && !strcmp(wb_preset[i].model, self->dev->image_storage.camera_model))
      {
        if(!wb_name) // This is first found preset for maker/model. add section.
        {
          char *section = g_strdup_printf("%s %s", self->dev->image_storage.camera_maker, self->dev->image_storage.camera_model);
          dt_bauhaus_combobox_add_section(g->presets, section);
          g_free(section);
          g->preset_cnt++;
        }
        if(!wb_name || strcmp(wb_name, wb_preset[i].name))
        {
          // new preset found
          dt_iop_temperature_preset_data_t *preset = malloc(sizeof(dt_iop_temperature_preset_data_t));
          wb_name = wb_preset[i].name;
          preset->no_ft_pos = i;
          preset->max_ft_pos = i;
          preset->min_ft_pos = i;
          if(wb_preset[i].tuning != 0)
          {
            // finetuning found.
            // min finetuning is always first, since wb_preset is ordered.
            int ft_pos = i;
            int last_ft = wb_preset[i].tuning;
            preset->min_ft_pos = ft_pos++;
            while (strcmp(wb_name, wb_preset[ft_pos].name) == 0)
            {
              if(wb_preset[ft_pos].tuning == 0)
              {
                preset->no_ft_pos = ft_pos;
              }
              if(wb_preset[ft_pos].tuning > last_ft)
              {
                preset->max_ft_pos = ft_pos;
                last_ft = wb_preset[ft_pos].tuning;
              }
              ft_pos++;
            }
            
          }
          dt_bauhaus_combobox_add_full(g->presets, _(wb_preset[i].name), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, preset, free, TRUE);
          g->preset_num[g->preset_cnt] = i;
          g->preset_cnt++;
          presets_found++;
        }
      }
    }

  return presets_found;
}

void color_finetuning_slider(struct dt_iop_module_t *self)
{

  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  if(!g->colored_sliders) return;

  dt_iop_temperature_preset_data_t *preset = dt_bauhaus_combobox_get_data(g->presets);
  if(preset != NULL)
  {
    //we can do realistic/exagerated.

    double min_tune[3] = {0.0};
    double no_tune[3] = {0.0};
    double max_tune[3] = {0.0};
    if(!g->blackbody_is_confusing) {
      //realistic
      const double neutral[3] = {
          1 / wb_preset[preset->no_ft_pos].channel[0],
          1 / wb_preset[preset->no_ft_pos].channel[1],
          1 / wb_preset[preset->no_ft_pos].channel[2],
      };
      for(int ch=0; ch<3; ch++) {
        min_tune[ch] = neutral[ch] * wb_preset[preset->min_ft_pos].channel[ch];
        no_tune[ch]  = neutral[ch] * wb_preset[preset->no_ft_pos].channel[ch];
        max_tune[ch] = neutral[ch] * wb_preset[preset->max_ft_pos].channel[ch];
      }

      const float maxsRGBmin_tune = MAX(MAX(min_tune[0], min_tune[1]), min_tune[2]);
      const float maxsRGBmax_tune = MAX(MAX(max_tune[0], max_tune[1]), max_tune[2]);

      for(int ch=0; ch<3; ch++) {
        min_tune[ch] = min_tune[ch] / maxsRGBmin_tune;
        no_tune[ch]  = 1.0;
        max_tune[ch] = max_tune[ch] / maxsRGBmax_tune;
      }
    } else {
      //exagerated

      for(int ch=0; ch<3; ch++) {
        min_tune[ch] = 0.5;
        no_tune[ch]  = 0.9;
        max_tune[ch] = 0.5;
      }

      if(wb_preset[preset->min_ft_pos].channel[0] < wb_preset[preset->max_ft_pos].channel[0]) {
        // from blue to red
        min_tune[0] = 0.1;
        min_tune[2] = 0.9;
        max_tune[0] = 0.9;
        max_tune[2] = 0.1;
      } else {
        //from red to blue
        min_tune[0] = 0.9;
        min_tune[2] = 0.1;
        max_tune[0] = 0.1;
        max_tune[2] = 0.9;
      }
    }

    dt_bauhaus_slider_clear_stops(g->finetune);

    dt_bauhaus_slider_set_stop(g->finetune, 0.0, min_tune[0], min_tune[1], min_tune[2]);
    dt_bauhaus_slider_set_stop(g->finetune, 0.5, no_tune[0],  no_tune[1],  no_tune[2]);
    dt_bauhaus_slider_set_stop(g->finetune, 1.0, max_tune[0], max_tune[1], max_tune[2]);
  }
  if(gtk_widget_get_visible(GTK_WIDGET(g->finetune))) {
    gtk_widget_queue_draw(GTK_WIDGET(g->finetune));
  }
}

void color_rgb_sliders(struct dt_iop_module_t *self)
{

  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  if(!g->colored_sliders) return;

  dt_bauhaus_slider_clear_stops(g->scale_r);
  dt_bauhaus_slider_clear_stops(g->scale_g);
  dt_bauhaus_slider_clear_stops(g->scale_b);
  dt_bauhaus_slider_clear_stops(g->scale_g2);
  // there are 3 ways to do colored sliders: naive (independed 0->1), smart(er) (dependent 0->1) and real (coeff)

  if(FALSE)
  {
  //naive:
    dt_bauhaus_slider_set_stop(g->scale_r, 0.0, 0.0, 0.0, 0.0);
    dt_bauhaus_slider_set_stop(g->scale_r, 1.0, 1.0, 0.0, 0.0);

    dt_bauhaus_slider_set_stop(g->scale_g, 0.0, 0.0, 0.0, 0.0);
    dt_bauhaus_slider_set_stop(g->scale_g, 1.0, 0.0, 1.0, 0.0);

    dt_bauhaus_slider_set_stop(g->scale_b, 0.0, 0.0, 0.0, 0.0);
    dt_bauhaus_slider_set_stop(g->scale_b, 1.0, 0.0, 0.0, 1.0);

    dt_bauhaus_slider_set_stop(g->scale_g2, 0.0, 0.0, 0.0, 0.0);
    dt_bauhaus_slider_set_stop(g->scale_g2, 1.0, 0.0, 1.0, 0.0);
  }
  if(!g->blackbody_is_confusing)
  {
    //smart(er) than naive
    const float rchan = dt_bauhaus_slider_get(g->scale_r) / dt_bauhaus_slider_get_hard_max(g->scale_r);
    const float gchan = dt_bauhaus_slider_get(g->scale_g) / dt_bauhaus_slider_get_hard_max(g->scale_g);
    const float bchan = dt_bauhaus_slider_get(g->scale_b) / dt_bauhaus_slider_get_hard_max(g->scale_b);

    dt_bauhaus_slider_set_stop(g->scale_r, 0.0, 0.0, gchan, bchan);
    dt_bauhaus_slider_set_stop(g->scale_r, 1.0, 1.0, gchan, bchan);

    dt_bauhaus_slider_set_stop(g->scale_g, 0.0, rchan, 0.0, bchan);
    dt_bauhaus_slider_set_stop(g->scale_g, 1.0, rchan, 1.0, bchan);

    dt_bauhaus_slider_set_stop(g->scale_b, 0.0, rchan, gchan, 0.0);
    dt_bauhaus_slider_set_stop(g->scale_b, 1.0, rchan, gchan, 1.0);
  } else {
    //real (ish)
    //we consider dalight wb to be "reference white"
    const double white[3] = {
      1.0/g->daylight_wb[0],
      1.0/g->daylight_wb[1],
      1.0/g->daylight_wb[2],
    };

    const float rchanmul = dt_bauhaus_slider_get(g->scale_r);
    const float rchanmulmax = dt_bauhaus_slider_get_hard_max(g->scale_r);
    const float gchanmul = dt_bauhaus_slider_get(g->scale_g);
    const float gchanmulmax = dt_bauhaus_slider_get_hard_max(g->scale_g);
    const float bchanmul = dt_bauhaus_slider_get(g->scale_b);
    const float bchanmulmax = dt_bauhaus_slider_get_hard_max(g->scale_g);

    dt_bauhaus_slider_set_stop(g->scale_r, 0.0, white[0]*0.0, white[1]*gchanmul, white[2]*bchanmul);
    dt_bauhaus_slider_set_stop(g->scale_r, g->daylight_wb[0]/rchanmulmax, white[0]*g->daylight_wb[0], white[1]*gchanmul, white[2]*bchanmul);
    dt_bauhaus_slider_set_stop(g->scale_r, 1.0, white[0]*1.0, white[1]*(gchanmul/gchanmulmax), white[2]*(bchanmul/bchanmulmax));

    dt_bauhaus_slider_set_stop(g->scale_g, 0.0, white[0]*rchanmul, white[1]*0.0, white[2]*bchanmul);
    dt_bauhaus_slider_set_stop(g->scale_g, g->daylight_wb[1]/bchanmulmax, white[0]*rchanmul, white[1]*g->daylight_wb[1], white[2]*bchanmul);
    dt_bauhaus_slider_set_stop(g->scale_g, 1.0, white[0]*(rchanmul/rchanmulmax), white[1]*1.0, white[2]*(bchanmul/bchanmulmax));

    dt_bauhaus_slider_set_stop(g->scale_b, 0.0, white[0]*rchanmul, white[1]*gchanmul, white[2]*0.0);
    dt_bauhaus_slider_set_stop(g->scale_b, g->daylight_wb[2]/bchanmulmax, white[0]*rchanmul, white[1]*gchanmul, white[2]*g->daylight_wb[2]);
    dt_bauhaus_slider_set_stop(g->scale_b, 1.0, white[0]*(rchanmul/rchanmulmax), white[1]*(gchanmul/gchanmulmax), white[2]*1.0);
  }

  if(gtk_widget_get_visible(GTK_WIDGET(g->scale_r))) {
    gtk_widget_queue_draw(GTK_WIDGET(g->scale_r));
    gtk_widget_queue_draw(GTK_WIDGET(g->scale_g));
    gtk_widget_queue_draw(GTK_WIDGET(g->scale_b));
  }
}

void color_temptint_sliders(struct dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  if(!g->colored_sliders) return;

  dt_bauhaus_slider_clear_stops(g->scale_k);
  dt_bauhaus_slider_clear_stops(g->scale_tint);

  const double temp_step = (double)(DT_IOP_HIGHEST_TEMPERATURE - DT_IOP_LOWEST_TEMPERATURE) / (DT_BAUHAUS_SLIDER_MAX_STOPS - 1.0);
  const double tint_step = (double)(DT_IOP_HIGHEST_TINT - DT_IOP_LOWEST_TINT) / (DT_BAUHAUS_SLIDER_MAX_STOPS - 1.0);
  const int blackbody_is_confusing = g->blackbody_is_confusing;

  const float cur_temp = dt_bauhaus_slider_get(g->scale_k);
  const float cur_tint = dt_bauhaus_slider_get(g->scale_tint);

  //we consider dalight wb to be "reference white"
  const double dayligh_white[3] = {
    1.0/g->daylight_wb[0],
    1.0/g->daylight_wb[1],
    1.0/g->daylight_wb[2],
  };

  double cur_coeffs[4] = {0.0};
  temp2mul(self, cur_temp, 1.0, cur_coeffs);
  const double cur_white[3] = {
    1.0/cur_coeffs[0],
    1.0/cur_coeffs[1],
    1.0/cur_coeffs[2],
  };

  // reflect actual black body colors for the temperature slider (or not)
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = i / (DT_BAUHAUS_SLIDER_MAX_STOPS - 1.0);
    const double K = DT_IOP_LOWEST_TEMPERATURE + i * temp_step;
    const double tint = DT_IOP_LOWEST_TINT + i * tint_step;

    if(!blackbody_is_confusing)
    {
      // it isn't!
      const cmsCIEXYZ cmsXYZ_temp = temperature_tint_to_XYZ(K,cur_tint);
      const cmsCIEXYZ cmsXYZ_tint = temperature_tint_to_XYZ(cur_temp, tint);
      float sRGB_temp[3], XYZ_temp[3] = {cmsXYZ_temp.X, cmsXYZ_temp.Y, cmsXYZ_temp.Z};
      float sRGB_tint[3], XYZ_tint[3] = {cmsXYZ_tint.X, cmsXYZ_tint.Y, cmsXYZ_tint.Z};

      dt_XYZ_to_sRGB(XYZ_temp, sRGB_temp);
      dt_XYZ_to_sRGB(XYZ_tint, sRGB_tint);

      const float maxsRGB_temp = MAX(MAX(sRGB_temp[0], sRGB_temp[1]), sRGB_temp[2]);
      const float maxsRGB_tint = MAX(MAX(sRGB_tint[0], sRGB_tint[1]), sRGB_tint[2]);

      if(maxsRGB_temp > 0.99999999) {
        for(int ch=0; ch<3; ch++){
          sRGB_temp[ch] = sRGB_temp[ch] > 0? sRGB_temp[ch] / maxsRGB_temp : 0.0;
        }
      }

      if(maxsRGB_tint > 0.99999999) {
        for(int ch=0; ch<3; ch++){
          sRGB_tint[ch] = sRGB_tint[ch] > 0? sRGB_tint[ch] / maxsRGB_tint : 0.0;
        }
      }

      dt_bauhaus_slider_set_stop(g->scale_k, stop, sRGB_temp[0], sRGB_temp[1], sRGB_temp[2]);
      dt_bauhaus_slider_set_stop(g->scale_tint, stop, sRGB_tint[0], sRGB_tint[1], sRGB_tint[2]);
    }
    else
    {
      // i think lightroom-ish look is ok-ish
      //dt_bauhaus_slider_set_stop(g->scale_k, stop, sRGB[2], sRGB[1], sRGB[0]);
      double coeffs_K[4];
      double coeffs_tint[4];
      temp2mul(self, K, cur_tint, coeffs_K);
      temp2mul(self, cur_temp, tint, coeffs_tint);
      coeffs_K[0] /= coeffs_K[1];
      coeffs_K[2] /= coeffs_K[1];
      coeffs_K[3] /= coeffs_K[1];
      coeffs_K[1] = 1.0;
      coeffs_tint[0] /= coeffs_tint[1];
      coeffs_tint[2] /= coeffs_tint[1];
      coeffs_tint[3] /= coeffs_tint[1];
      coeffs_tint[1] = 1.0;

      float sRGB_K[3] = { dayligh_white[0]*coeffs_K[0], dayligh_white[1]*coeffs_K[1], dayligh_white[2]*coeffs_K[2] };
      float sRGB_tint[3] = {cur_white[0]*coeffs_tint[0], cur_white[1]*coeffs_tint[1], cur_white[2]*coeffs_tint[2]};
      const float maxsRGB_K = MAX(MAX(sRGB_K[0], sRGB_K[1]), sRGB_K[2]);
      const float maxsRGB_tint = MAX(MAX(sRGB_tint[0], sRGB_tint[1]),sRGB_tint[2]);

      if(maxsRGB_K > 0.99999999) {
        for(int ch=0; ch<3; ch++){
          sRGB_K[ch] = sRGB_K[ch] > 0? sRGB_K[ch] / maxsRGB_K : 0.0;
        }
      }
      if(maxsRGB_tint > 0.99999999) {
        for(int ch=0; ch<3; ch++){
          sRGB_tint[ch] = sRGB_tint[ch] > 0? sRGB_tint[ch] / maxsRGB_tint : 0.0;
        }
      }
      dt_bauhaus_slider_set_stop(g->scale_k, stop, sRGB_K[0], sRGB_K[1], sRGB_K[2]);
      dt_bauhaus_slider_set_stop(g->scale_tint, stop, sRGB_tint[0], sRGB_tint[1], sRGB_tint[2]);
    }
  }

  if(gtk_widget_get_visible(GTK_WIDGET(g->scale_k))) 
  {
    gtk_widget_queue_draw(GTK_WIDGET(g->scale_k));
    gtk_widget_queue_draw(GTK_WIDGET(g->scale_tint));
  }
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)module->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)module->default_params;

  if(self->hide_enable_button)
  {
    gtk_stack_set_visible_child_name(GTK_STACK(g->stack), "disabled");
    return;
  }
  gtk_stack_set_visible_child_name(GTK_STACK(g->stack), "enabled");

  dt_iop_color_picker_reset(self, TRUE);

  double TempK, tint;
  mul2temp(self, p->coeffs, &TempK, &tint);

  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  dt_bauhaus_slider_set(g->scale_g2, p->coeffs[3]);
  dt_bauhaus_slider_set(g->scale_k, TempK);
  dt_bauhaus_slider_set(g->scale_tint, tint);

  gui_sliders_update(self);
  for(int k = 0; k < 4; k++) g->mod_coeff[k] = p->coeffs[k];

  dt_bauhaus_combobox_clear(g->presets);
  dt_bauhaus_combobox_add(g->presets, C_("white balance", "as shot")); // old "camera". reason for change: all other RAW development tools use "As Shot" or "shot"
  dt_bauhaus_combobox_add(g->presets, C_("white balance", "camera standard D65")); // old "camera neutral", reason: better matches intent
  dt_bauhaus_combobox_add(g->presets, C_("white balance", "from image area")); // old "spot", reason: describes exactly what'll happen
  dt_bauhaus_combobox_add(g->presets, C_("white balance", "user modified"));

  g->preset_cnt = DT_IOP_NUM_OF_STD_TEMP_PRESETS;
  memset(g->preset_num, 0, sizeof(g->preset_num));

  dt_bauhaus_combobox_set(g->presets, -1);
  dt_bauhaus_slider_set(g->finetune, 0);
  gtk_widget_set_sensitive(g->finetune, 0);

  generate_preset_combo(self);

  gboolean found = FALSE;
  // is this a "as shot" white balance?
  if(memcmp(p->coeffs, fp->coeffs, 3 * sizeof(float)) == 0)
  {
    dt_bauhaus_combobox_set(g->presets, 0);
    found = TRUE;
  }
  else
  {
    // is this a "D65 white balance"?
    if((p->coeffs[0] == (float)g->daylight_wb[0]) && (p->coeffs[1] == (float)g->daylight_wb[1])
       && (p->coeffs[2] == (float)g->daylight_wb[2]))
    {
      dt_bauhaus_combobox_set(g->presets, 1);
      found = TRUE;
    }
  }

  if(!found)
  {
    // look through all added presets
    for(int j = DT_IOP_NUM_OF_STD_TEMP_PRESETS; !found && (j < g->preset_cnt); j++)
    {
      // look through all variants of this preset, with different tuning
      for(int i = g->preset_num[j]; !found && (i < wb_preset_count)
                                    && !strcmp(wb_preset[i].make, self->dev->image_storage.camera_maker)
                                    && !strcmp(wb_preset[i].model, self->dev->image_storage.camera_model)
                                    && !strcmp(wb_preset[i].name, wb_preset[g->preset_num[j]].name);
          i++)
      {
        float coeffs[3];
        for(int k = 0; k < 3; k++) coeffs[k] = wb_preset[i].channel[k];

        if(memcmp(coeffs, p->coeffs, 3 * sizeof(float)) == 0)
        {
          // got exact match!
          dt_bauhaus_combobox_set(g->presets, j);
          dt_iop_temperature_preset_data_t *preset = dt_bauhaus_combobox_get_data(g->presets);
          if(preset != NULL)
          {
            gtk_widget_set_sensitive(g->finetune, (preset->min_ft_pos != preset->max_ft_pos));
            dt_bauhaus_slider_set_hard_min(g->finetune, wb_preset[preset->min_ft_pos].tuning);
            dt_bauhaus_slider_set_hard_max(g->finetune, wb_preset[preset->max_ft_pos].tuning);
            dt_bauhaus_slider_set_default(g->finetune, wb_preset[preset->no_ft_pos].tuning);
          }
          
          dt_bauhaus_slider_set(g->finetune, wb_preset[i].tuning);
          found = TRUE;
          break;
        }
      }
    }

    if(!found)
    {
      // ok, we haven't found exact match, maybe this was interpolated?

      // look through all added presets
      for(int j = DT_IOP_NUM_OF_STD_TEMP_PRESETS; !found && (j < g->preset_cnt); j++)
      {
        // look through all variants of this preset, with different tuning
        int i = g->preset_num[j] + 1;
        while(!found && (i < wb_preset_count) && !strcmp(wb_preset[i].make, self->dev->image_storage.camera_maker)
              && !strcmp(wb_preset[i].model, self->dev->image_storage.camera_maker)
              && !strcmp(wb_preset[i].name, wb_preset[g->preset_num[j]].name))
        {
          // let's find gaps
          if(wb_preset[i - 1].tuning + 1 == wb_preset[i].tuning)
          {
            i++;
            continue;
          }

          // we have a gap!

          // we do not know what finetuning value was set, we need to bruteforce to find it
          for(int tune = wb_preset[i - 1].tuning + 1; !found && (tune < wb_preset[i].tuning); tune++)
          {
            wb_data interpolated = {.tuning = tune };
            dt_wb_preset_interpolate(&wb_preset[i - 1], &wb_preset[i], &interpolated);

            float coeffs[3];
            for(int k = 0; k < 3; k++) coeffs[k] = interpolated.channel[k];

            if(memcmp(coeffs, p->coeffs, 3 * sizeof(float)) == 0)
            {
              // got exact match!

              dt_bauhaus_combobox_set(g->presets, j);
              dt_iop_temperature_preset_data_t *preset = dt_bauhaus_combobox_get_data(g->presets);
              if(preset != NULL)
              {
                gtk_widget_set_sensitive(g->finetune, (preset->min_ft_pos != preset->max_ft_pos));
                dt_bauhaus_slider_set_hard_min(g->finetune, wb_preset[preset->min_ft_pos].tuning);
                dt_bauhaus_slider_set_hard_max(g->finetune, wb_preset[preset->max_ft_pos].tuning);
                dt_bauhaus_slider_set_default(g->finetune, wb_preset[preset->no_ft_pos].tuning);
              }
              dt_bauhaus_slider_set(g->finetune, tune);
              found = TRUE;
              break;
            }
          }
          i++;
        }
      }
    }
    if (!found) //since we haven't got a match - it's user-set
      dt_bauhaus_combobox_set(g->presets, 3);
  }

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->coeffs_toggle));
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(g->coeffs_expander), active);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->coeffs_toggle), dtgtk_cairo_paint_solid_arrow,
                               CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX | (active?CPF_DIRECTION_DOWN:CPF_DIRECTION_LEFT), NULL);

  gtk_widget_set_visible(GTK_WIDGET(g->finetune), (found && gtk_widget_get_sensitive(g->finetune)));

  const int preset = dt_bauhaus_combobox_get(g->presets);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_asshot), preset == 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_user), preset == 3);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_d65), preset == 1);

  color_temptint_sliders(self);
  color_rgb_sliders(self);
  color_finetuning_slider(self);

  gtk_widget_queue_draw(self->widget);
}

static int calculate_bogus_daylight_wb(dt_iop_module_t *module, double bwb[4])
{
  if(!dt_image_is_raw(&module->dev->image_storage))
  {
    bwb[0] = 1.0;
    bwb[2] = 1.0;
    bwb[1] = 1.0;
    bwb[3] = 1.0;

    return 0;
  }

  double mul[4];
  if (dt_colorspaces_conversion_matrices_rgb(module->dev->image_storage.camera_makermodel, NULL, NULL, mul))
  {
    // normalize green:
    bwb[0] = mul[0] / mul[1];
    bwb[2] = mul[2] / mul[1];
    bwb[1] = 1.0;
    bwb[3] = mul[3] / mul[1];

    return 0;
  }

  return 1;
}

static void prepare_matrices(dt_iop_module_t *module)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)module->gui_data;

  // sRGB D65
  const double RGB_to_XYZ[3][4] = { { 0.4124564, 0.3575761, 0.1804375, 0 },
                                    { 0.2126729, 0.7151522, 0.0721750, 0 },
                                    { 0.0193339, 0.1191920, 0.9503041, 0 } };

  // sRGB D65
  const double XYZ_to_RGB[4][3] = { { 3.2404542, -1.5371385, -0.4985314 },
                                    { -0.9692660, 1.8760108, 0.0415560 },
                                    { 0.0556434, -0.2040259, 1.0572252 },
                                    { 0, 0, 0 } };

  if(!dt_image_is_raw(&module->dev->image_storage))
  {
    // let's just assume for now(TM) that if it is not raw, it is sRGB
    memcpy(g->XYZ_to_CAM, XYZ_to_RGB, sizeof(g->XYZ_to_CAM));
    memcpy(g->CAM_to_XYZ, RGB_to_XYZ, sizeof(g->CAM_to_XYZ));
    return;
  }

  char *camera = module->dev->image_storage.camera_makermodel;
  if (!dt_colorspaces_conversion_matrices_xyz(camera, module->dev->image_storage.d65_color_matrix,
                                                      g->XYZ_to_CAM, g->CAM_to_XYZ))
  {
    fprintf(stderr, "[temperature] `%s' color matrix not found for image\n", camera);
    dt_control_log(_("`%s' color matrix not found for image"), camera);
  }
}

static void find_coeffs(dt_iop_module_t *module, float coeffs[4])
{
  const dt_image_t *img = &module->dev->image_storage;

  // the raw should provide wb coeffs:
  int ok = 1;
  // Only check the first three values, the fourth is usually NAN for RGB
  const int num_coeffs = (img->flags & DT_IMAGE_4BAYER) ? 4 : 3;
  for(int k = 0; ok && k < num_coeffs; k++)
  {
    if(!isnormal(img->wb_coeffs[k]) || img->wb_coeffs[k] == 0.0f) ok = 0;
  }
  if(ok)
  {
    for(int k = 0; k < 4; k++) coeffs[k] = img->wb_coeffs[k];
    return;
  }

  if(!ignore_missing_wb(&(module->dev->image_storage)))
  {
    dt_control_log(_("failed to read camera white balance information from `%s'!"),
                   img->filename);
    fprintf(stderr, "[temperature] failed to read camera white balance information from `%s'!\n",
            img->filename);
  }

  double bwb[4];
  if(!calculate_bogus_daylight_wb(module, bwb))
  {
    // found camera matrix and used it to calculate bogus daylight wb
    for(int c = 0; c < 4; c++) coeffs[c] = bwb[c];
    return;
  }

  // no cam matrix??? try presets:
  for(int i = 0; i < wb_preset_count; i++)
  {
    if(!strcmp(wb_preset[i].make, img->camera_maker)
       && !strcmp(wb_preset[i].model, img->camera_model))
    {
      // just take the first preset we find for this camera
      for(int k = 0; k < 3; k++) coeffs[k] = wb_preset[i].channel[k];
      return;
    }
  }

  // did not find preset either?
  // final security net: hardcoded default that fits most cams.
  coeffs[0] = 2.0f;
  coeffs[1] = 1.0f;
  coeffs[2] = 1.5f;
  coeffs[3] = 1.0f;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_temperature_params_t tmp
      = (dt_iop_temperature_params_t){.coeffs = { 1.0, 1.0, 1.0, 1.0 } };

  // we might be called from presets update infrastructure => there is no image
  if(!module->dev || module->dev->image_storage.id == -1) goto end;

  const int is_raw = dt_image_is_matrix_correction_supported(&module->dev->image_storage);

  module->default_enabled = 0;
  module->hide_enable_button = 0;

  // White balance module doesn't need to be enabled for monochrome raws (like
  // for leica monochrom cameras). prepare_matrices is a noop as well, as there
  // isn't a color matrix, so we can skip that as well.
  if(dt_image_is_monochrome(&(module->dev->image_storage)))
  {
    module->hide_enable_button = 1;
  }
  else
  {
    if(module->gui_data) prepare_matrices(module);

    /* check if file is raw / hdr */
    if(is_raw)
    {
      // raw images need wb:
      module->default_enabled = 1;

      // do best to find starting coeffs
      find_coeffs(module, tmp.coeffs);
      tmp.coeffs[0] /= tmp.coeffs[1];
      tmp.coeffs[2] /= tmp.coeffs[1];
      tmp.coeffs[3] /= tmp.coeffs[1];
      tmp.coeffs[1] = 1.0f;
    }
  }

  // remember daylight wb used for temperature/tint conversion,
  // assuming it corresponds to CIE daylight (D65)
  if(module->gui_data)
  {
    dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)module->gui_data;

    dt_bauhaus_slider_set_default(g->scale_r, tmp.coeffs[0]);
    dt_bauhaus_slider_set_default(g->scale_g, tmp.coeffs[1]);
    dt_bauhaus_slider_set_default(g->scale_b, tmp.coeffs[2]);
    dt_bauhaus_slider_set_default(g->scale_g2, tmp.coeffs[3]);

    // to have at least something and definitely not crash
    for(int c = 0; c < 4; c++) g->daylight_wb[c] = tmp.coeffs[c];

    if(!calculate_bogus_daylight_wb(module, g->daylight_wb))
    {
      // found camera matrix and used it to calculate bogus daylight wb
    }
    else
    {
      // if we didn't find anything for daylight wb, look for a wb preset with appropriate name.
      // we're normalizing that to be D65
      for(int i = 0; i < wb_preset_count; i++)
      {
        if(!strcmp(wb_preset[i].make, module->dev->image_storage.camera_maker)
           && !strcmp(wb_preset[i].model, module->dev->image_storage.camera_model)
           && (!strcmp(wb_preset[i].name, Daylight) || !strcmp(wb_preset[i].name, DirectSunlight))
           && wb_preset[i].tuning == 0)
        {

          for(int k = 0; k < 4; k++) g->daylight_wb[k] = wb_preset[i].channel[k];
          break;
        }
      }
    }

    double TempK, tint;
    mul2temp(module, tmp.coeffs, &TempK, &tint);

    dt_bauhaus_slider_set_default(g->scale_k, TempK);
    dt_bauhaus_slider_set_default(g->scale_tint, tint);
  }

end:
  memcpy(module->params, &tmp, sizeof(dt_iop_temperature_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_temperature_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_temperature_global_data_t *gd
      = (dt_iop_temperature_global_data_t *)malloc(sizeof(dt_iop_temperature_global_data_t));
  module->data = gd;
  gd->kernel_whitebalance_4f = dt_opencl_create_kernel(program, "whitebalance_4f");
  gd->kernel_whitebalance_1f = dt_opencl_create_kernel(program, "whitebalance_1f");
  gd->kernel_whitebalance_1f_xtrans = dt_opencl_create_kernel(program, "whitebalance_1f_xtrans");
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_temperature_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_temperature_params_t));
  module->params_size = sizeof(dt_iop_temperature_params_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_temperature_global_data_t *gd = (dt_iop_temperature_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_whitebalance_4f);
  dt_opencl_free_kernel(gd->kernel_whitebalance_1f);
  dt_opencl_free_kernel(gd->kernel_whitebalance_1f_xtrans);
  free(module->data);
  module->data = NULL;
}

static void gui_update_from_coeffs(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;

  double TempK, tint;
  mul2temp(self, p->coeffs, &TempK, &tint);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->scale_k, TempK);
  dt_bauhaus_slider_set(g->scale_tint, tint);
  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  dt_bauhaus_slider_set(g->scale_g2, p->coeffs[3]);
  --darktable.gui->reset;

  color_temptint_sliders(self);
  color_rgb_sliders(self);
  color_finetuning_slider(self);
}

static void temp_changed(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  const double TempK = dt_bauhaus_slider_get(g->scale_k);
  const double tint = dt_bauhaus_slider_get(g->scale_tint);

  double coeffs[4];
  temp2mul(self, TempK, tint, coeffs);

  // normalize
  coeffs[0] /= coeffs[1];
  coeffs[2] /= coeffs[1];
  coeffs[3] /= coeffs[1];
  coeffs[1] = 1.0;
  for(int c = 0; c < 4; c++) p->coeffs[c] = g->mod_coeff[c] = coeffs[c];

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  dt_bauhaus_slider_set(g->scale_g2, p->coeffs[3]);
  --darktable.gui->reset;
  dt_dev_add_history_item(darktable.develop, self, TRUE);

  color_temptint_sliders(self);
  color_rgb_sliders(self);
  color_finetuning_slider(self);
}

static void tint_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, 3);
}

static void temp_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, 3);
}

static void rgb_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  dt_iop_color_picker_reset(self, TRUE);
  const float value = dt_bauhaus_slider_get(slider);
  if(slider == g->scale_r)
    p->coeffs[0] = g->mod_coeff[0] = value;
  else if(slider == g->scale_g)
    p->coeffs[1] = g->mod_coeff[1] = value;
  else if(slider == g->scale_b)
    p->coeffs[2] = g->mod_coeff[2] = value;
  else if(slider == g->scale_g2)
    p->coeffs[3] = g->mod_coeff[3] = value;

  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_bauhaus_combobox_set(g->presets, 3);
}

static void btn_asshot_toggled(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_temperature_gui_data_t *g = self->gui_data;

  if(gtk_toggle_button_get_active(togglebutton) && dt_bauhaus_combobox_get(g->presets) != 0)
    dt_bauhaus_combobox_set(g->presets, 0);
}

static void btn_d65_toggled(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_temperature_gui_data_t *g = self->gui_data;

  if(gtk_toggle_button_get_active(togglebutton) && dt_bauhaus_combobox_get(g->presets) != 1)
    dt_bauhaus_combobox_set(g->presets, 1);
}

static void btn_user_toggled(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_temperature_gui_data_t *g = self->gui_data;

  if(gtk_toggle_button_get_active(togglebutton) && dt_bauhaus_combobox_get(g->presets) != 3)
    dt_bauhaus_combobox_set(g->presets, 3);
}

static void apply_preset(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_color_picker_reset(self, TRUE);
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)self->default_params;
  const int tune = dt_bauhaus_slider_get(g->finetune);
  const int pos = dt_bauhaus_combobox_get(g->presets);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_asshot), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_user), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_d65), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->colorpicker), FALSE);
  switch(pos)
  {
    case -1: // just un-setting.
      return;
    case 0: // as shot wb
      for(int k = 0; k < 4; k++) p->coeffs[k] = fp->coeffs[k];
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_asshot), TRUE);
      break;
    case 1: // camera reference d65
      for(int k = 0; k < 4; k++) p->coeffs[k] = g->daylight_wb[k];
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_d65), TRUE);
      break;
    case 2: // from image area wb, expose callback will set p->coeffs.
      //reset previously stored color picker information
      for(int k = 0; k < 4; k++) old[k] = 0.0f;
      g_signal_emit_by_name(G_OBJECT(g->colorpicker), "quad-pressed");

      break;
    case 3: // directly changing one of the coeff sliders also changes the mod_coeff so it can be read here
      for(int k = 0; k < 4; k++) p->coeffs[k] = g->mod_coeff[k];
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_user), TRUE);
      break;      
    default: // camera WB presets
    {
      gboolean found = FALSE;
      dt_iop_temperature_preset_data_t *preset = dt_bauhaus_combobox_get_data(g->presets);
      // look through all variants of this preset, with different tuning
      for(int i = preset->min_ft_pos; (i < (preset->max_ft_pos + 1)) // we can limit search spread thanks to knowing where to look!
                                      && !strcmp(wb_preset[i].make, self->dev->image_storage.camera_maker)
                                      && !strcmp(wb_preset[i].model, self->dev->image_storage.camera_model)
                                      && !strcmp(wb_preset[i].name, wb_preset[preset->no_ft_pos].name);
          i++)
      {
        if(wb_preset[i].tuning == tune)
        {
          // got exact match!
          for(int k = 0; k < 4; k++) p->coeffs[k] = wb_preset[i].channel[k];
          found = TRUE;
          break;
        }
      }

      if(!found)
      {
        // ok, we haven't found exact match, need to interpolate

        // let's find 2 most closest tunings with needed_tuning in-between
        int min_id = INT_MIN, max_id = INT_MIN;

        // look through all variants of this preset, with different tuning, starting from second entry (if
        // any)
        int i = preset->min_ft_pos + 1;
        while((i < preset->max_ft_pos+1) && !strcmp(wb_preset[i].make, self->dev->image_storage.camera_maker)
              && !strcmp(wb_preset[i].model, self->dev->image_storage.camera_model)
              && !strcmp(wb_preset[i].name, wb_preset[preset->no_ft_pos].name))
        {
          if(wb_preset[i - 1].tuning < tune && wb_preset[i].tuning > tune)
          {
            min_id = i - 1;
            max_id = i;
            break;
          }

          i++;
        }

        // have we found enough good data?
        if(min_id == INT_MIN || max_id == INT_MIN || min_id == max_id) break; // hysteresis

        found = TRUE;
        wb_data interpolated = {.tuning = tune };
        dt_wb_preset_interpolate(&wb_preset[min_id], &wb_preset[max_id], &interpolated);
        for(int k = 0; k < 4; k++) p->coeffs[k] = interpolated.channel[k];
      }
    }
    break;
  }
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void presets_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  apply_preset(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  const dt_iop_temperature_preset_data_t *const preset = dt_bauhaus_combobox_get_data(widget);
  if(preset != NULL)
  {
    const int32_t old_reset = darktable.gui->reset;
    darktable.gui->reset = 1;
    gtk_widget_set_sensitive(g->finetune, (preset->min_ft_pos != preset->max_ft_pos));
    dt_bauhaus_slider_set_hard_min(g->finetune, wb_preset[preset->min_ft_pos].tuning);
    dt_bauhaus_slider_set_hard_max(g->finetune, wb_preset[preset->max_ft_pos].tuning);
    dt_bauhaus_slider_set_default(g->finetune, wb_preset[preset->no_ft_pos].tuning);
    darktable.gui->reset = old_reset;
  } else {
    gtk_widget_set_sensitive(g->finetune, FALSE);
  }
  gtk_widget_set_visible(GTK_WIDGET(g->finetune), gtk_widget_get_sensitive(g->finetune));

  color_temptint_sliders(self);
  color_rgb_sliders(self);
  color_finetuning_slider(self);
}

static void finetune_changed(GtkWidget *widget, gpointer user_data)
{
  apply_preset((dt_iop_module_t *)user_data);
  color_temptint_sliders((dt_iop_module_t *)user_data);
  color_rgb_sliders((dt_iop_module_t *)user_data);
  color_finetuning_slider((dt_iop_module_t *)user_data);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  if(darktable.gui->reset) return;

  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, 2);

  // capture gui color picked event.
  if(self->picked_color_max[0] < self->picked_color_min[0]) return;
  const float *grayrgb = self->picked_color;
  //test the newly picked color: if the same as the last, do not process further
  if(grayrgb[0] == old[0] && grayrgb[1] == old[1] && grayrgb[2] == old[2] && grayrgb[3] == old[3]) return;
  for(int k = 0; k < 4; k++) old[k] = grayrgb[k];
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  for(int k = 0; k < 4; k++) p->coeffs[k] = (grayrgb[k] > 0.001f) ? 1.0f / grayrgb[k] : 1.0f;
  // normalize green:
  p->coeffs[0] /= p->coeffs[1];
  p->coeffs[2] /= p->coeffs[1];
  p->coeffs[3] /= p->coeffs[1];
  p->coeffs[1] = 1.0;
  // clamp
  for(int k = 0; k < 4; k++) p->coeffs[k] = fmaxf(0.0f, fminf(8.0f, p->coeffs[k]));

  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);

  color_rgb_sliders(self);
  color_temptint_sliders(self);
}

static void _coeffs_button_changed(GtkDarktableToggleButton *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->coeffs_toggle));
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(g->coeffs_expander), active);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->coeffs_toggle), dtgtk_cairo_paint_solid_arrow,
                               CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX | (active?CPF_DIRECTION_DOWN:CPF_DIRECTION_LEFT), NULL);
  g->expand_coeffs = active;
  dt_conf_set_bool("plugins/darkroom/temperature/expand_coefficients", active);
}

static void gui_sliders_update(struct dt_iop_module_t *self)
{
  const dt_image_t *img = &self->dev->image_storage;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  if(FILTERS_ARE_CYGM(img->buf_dsc.filters))
  {
    dt_bauhaus_widget_set_label(g->scale_r, NULL, _("green"));
    gtk_widget_set_tooltip_text(g->scale_r, _("green channel coefficient"));
    dt_bauhaus_widget_set_label(g->scale_g, NULL, _("magenta"));
    gtk_widget_set_tooltip_text(g->scale_g, _("magenta channel coefficient"));
    dt_bauhaus_widget_set_label(g->scale_b, NULL, _("cyan"));
    gtk_widget_set_tooltip_text(g->scale_b, _("cyan channel coefficient"));
    dt_bauhaus_widget_set_label(g->scale_g2, NULL, _("yellow"));
    gtk_widget_set_tooltip_text(g->scale_g2, _("yellow channel coefficient"));

    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_b, 0);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_g2, 1);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_g, 2);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_r, 3);
  }
  else
  {
    dt_bauhaus_widget_set_label(g->scale_r, NULL, _("red"));
    gtk_widget_set_tooltip_text(g->scale_r, _("red channel coefficient"));
    dt_bauhaus_widget_set_label(g->scale_g, NULL, _("green"));
    gtk_widget_set_tooltip_text(g->scale_g, _("green channel coefficient"));
    dt_bauhaus_widget_set_label(g->scale_b, NULL, _("blue"));
    gtk_widget_set_tooltip_text(g->scale_b, _("blue channel coefficient"));
    dt_bauhaus_widget_set_label(g->scale_g2, NULL, _("emerald"));
    gtk_widget_set_tooltip_text(g->scale_g2, _("emerald channel coefficient"));
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_r, 0);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_g, 1);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_b, 2);
    gtk_box_reorder_child(GTK_BOX(g->coeff_widgets), g->scale_g2, 3);
  }

  gtk_widget_set_visible(GTK_WIDGET(g->scale_g2), (img->flags & DT_IMAGE_4BAYER));
}

static void temp_label_click(GtkWidget *label, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  gchar *old_config = dt_conf_get_string("plugins/darkroom/temperature/colored_sliders");
  gboolean reset_feedback = FALSE;

  if(!g_strcmp0(old_config, "no color")) {
    dt_conf_set_string("plugins/darkroom/temperature/colored_sliders", "blackbody");
    reset_feedback = TRUE;
    g->colored_sliders = TRUE;
    g->blackbody_is_confusing = FALSE;
  } else if (!g_strcmp0(old_config, "blackbody"))
  {
    dt_conf_set_string("plugins/darkroom/temperature/colored_sliders", "effect emulation");
    g->colored_sliders = TRUE;
    g->blackbody_is_confusing = TRUE;
  } else
  {
    dt_conf_set_string("plugins/darkroom/temperature/colored_sliders", "no color");
    reset_feedback = TRUE;
    g->colored_sliders = FALSE;
    g->blackbody_is_confusing = FALSE;
  }

  g_free(old_config);

  if(reset_feedback)
  {
    if(!g->colored_sliders){
      dt_bauhaus_slider_clear_stops(g->scale_k);
      dt_bauhaus_slider_clear_stops(g->scale_tint);
      dt_bauhaus_slider_clear_stops(g->scale_r);
      dt_bauhaus_slider_clear_stops(g->scale_g);
      dt_bauhaus_slider_clear_stops(g->scale_b);
      dt_bauhaus_slider_clear_stops(g->scale_g2);
      dt_bauhaus_slider_clear_stops(g->finetune);
    }

    const int feedback = g->colored_sliders ? 0 : 1;
    dt_bauhaus_slider_set_feedback(g->scale_k, feedback);
    dt_bauhaus_slider_set_feedback(g->scale_tint, feedback);
    dt_bauhaus_slider_set_feedback(g->scale_r, feedback);
    dt_bauhaus_slider_set_feedback(g->scale_g, feedback);
    dt_bauhaus_slider_set_feedback(g->scale_b, feedback);
    dt_bauhaus_slider_set_feedback(g->scale_g2, feedback);
    dt_bauhaus_slider_set_feedback(g->finetune, feedback);
  }

  color_temptint_sliders(self);
  color_rgb_sliders(self);
  color_finetuning_slider(self);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = calloc(1, sizeof(dt_iop_temperature_gui_data_t));
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->default_params;
  
  gchar *config = dt_conf_get_string("plugins/darkroom/temperature/colored_sliders");
  g->colored_sliders = g_strcmp0(config, "no color"); // true if config != "no color"
  g->blackbody_is_confusing = g->colored_sliders && g_strcmp0(config, "blackbody"); // true if config != "blackbody"
  g->expand_coeffs = dt_conf_get_bool("plugins/darkroom/temperature/expand_coefficients");
  g_free(config);

  const int feedback = g->colored_sliders ? 0 : 1;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  g->stack = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(g->stack), FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->stack, TRUE, TRUE, 0);

  g->box_enabled = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  for(int k = 0; k < 4; k++) g->daylight_wb[k] = 1.0;

  GtkWidget *temp_label_box = gtk_event_box_new();
  GtkWidget *temp_label = dt_ui_section_label_new(_("scene illuminant temp"));
  gtk_container_add(GTK_CONTAINER(temp_label_box), temp_label);

  g_signal_connect(G_OBJECT(temp_label_box), "button-release-event", G_CALLBACK(temp_label_click), self);

  gtk_box_pack_start(GTK_BOX(g->box_enabled), temp_label_box, TRUE, TRUE, 0);

  //Match UI order: temp first, then tint (like every other app ever)
  g->scale_k = dt_bauhaus_slider_new_with_range_and_feedback(self, DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE,
                                                  10., 5000.0, 0, feedback);
  g->scale_tint
      = dt_bauhaus_slider_new_with_range_and_feedback(self, DT_IOP_LOWEST_TINT, DT_IOP_HIGHEST_TINT, .01, 1.0, 3, feedback);

  dt_bauhaus_slider_set_format(g->scale_k, "%.0f K");
  dt_bauhaus_widget_set_label(g->scale_k, NULL, _("temperature"));
  gtk_widget_set_tooltip_text(g->scale_k, _("color temperature (in Kelvin)"));

  dt_bauhaus_widget_set_label(g->scale_tint, NULL, _("tint"));
  gtk_widget_set_tooltip_text(g->scale_tint, _("color tint of the image, from magenta (value < 1) to green (value > 1)"));

  GtkWidget *gridw = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(gridw);
  gtk_grid_set_row_spacing(grid, DT_BAUHAUS_SPACE);
  gtk_grid_set_column_spacing(grid, DT_BAUHAUS_SPACE);
  gtk_grid_set_column_homogeneous(grid, FALSE);

  gtk_grid_attach(grid, g->scale_k, 0, 1, 1, 1);
  gtk_widget_set_hexpand(g->scale_k, TRUE);
  gtk_grid_attach(grid, g->scale_tint, 0, 2, 1, 1);
  gtk_widget_set_hexpand(g->scale_tint, TRUE);

  g->colorpicker = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g->btn_asshot = dtgtk_togglebutton_new(dtgtk_cairo_paint_eye, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g->btn_user = dtgtk_togglebutton_new(dtgtk_cairo_paint_star, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g->btn_d65 = dtgtk_togglebutton_new(dtgtk_cairo_paint_bulb, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  gtk_grid_attach(grid, g->colorpicker, 1, 1, 1, 1);
  gtk_grid_attach(grid, g->btn_asshot, 2, 1, 1, 1);
  gtk_grid_attach(grid, g->btn_user, 1, 2, 1, 1);
  gtk_grid_attach(grid, g->btn_d65, 2, 2, 1, 1);

  g_signal_connect(G_OBJECT(g->colorpicker), "toggled", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);
  g_signal_connect(G_OBJECT(g->btn_asshot), "toggled", G_CALLBACK(btn_asshot_toggled),  (gpointer)self);
  g_signal_connect(G_OBJECT(g->btn_user), "toggled", G_CALLBACK(btn_user_toggled),  (gpointer)self);
  g_signal_connect(G_OBJECT(g->btn_d65), "toggled", G_CALLBACK(btn_d65_toggled),  (gpointer)self);

  dt_iop_init_single_picker(&g->color_picker,
                     self,
                     GTK_WIDGET(g->colorpicker),
                     DT_COLOR_PICKER_AREA,
                     _iop_color_picker_apply);

  gtk_box_pack_start(GTK_BOX(g->box_enabled), gridw, TRUE, TRUE, 0);

  // collapsible section for coeffs that are generally not to be used

  GtkWidget *destdisp_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_BAUHAUS_SPACE);
  GtkWidget *destdisp = dt_ui_section_label_new(_("rgb coefficients"));

  g->coeffs_toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow, CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX | CPF_DIRECTION_LEFT, NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->coeffs_toggle), g->expand_coeffs);
  gtk_widget_set_name(GTK_WIDGET(g->coeffs_toggle), "control-button");

  g->coeff_widgets = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(destdisp_head), destdisp, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(destdisp_head), g->coeffs_toggle, FALSE, FALSE, 0);

  g->coeffs_expander = dtgtk_expander_new(destdisp_head, g->coeff_widgets);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(g->coeffs_expander), TRUE);
  gtk_box_pack_start(GTK_BOX(g->box_enabled), g->coeffs_expander, FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(g->coeffs_toggle), "toggled", G_CALLBACK(_coeffs_button_changed),  (gpointer)self);

  g->scale_r = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0, 8.0, .001, p->coeffs[0], 3, feedback);
  g->scale_g = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0, 8.0, .001, p->coeffs[1], 3, feedback);
  g->scale_b = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0, 8.0, .001, p->coeffs[2], 3, feedback);
  g->scale_g2 = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0, 8.0, .001, p->coeffs[3], 3, feedback);

  gtk_box_pack_start(GTK_BOX(g->coeff_widgets), g->scale_r, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->coeff_widgets), g->scale_g, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->coeff_widgets), g->scale_b, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->coeff_widgets), g->scale_g2, TRUE, TRUE, 0);

  gtk_widget_set_no_show_all(g->scale_g2, TRUE);

  gui_sliders_update(self);

  GtkWidget *cam_preset_label = dt_ui_section_label_new(_("camera presets"));
  gtk_box_pack_start(GTK_BOX(g->box_enabled), cam_preset_label, TRUE, TRUE, 0);

  g->presets = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->presets, NULL, _("setting")); // relabel to setting to remove confusion between module presets and white balance settings
  gtk_box_pack_start(GTK_BOX(g->box_enabled), g->presets, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->presets, _("choose white balance preset from camera"));
  // create hidden color picker to be able to send its signal when spot selected
  g->colorpicker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_combobox_new(self));
  gtk_stack_add_named(GTK_STACK(g->stack), g->colorpicker, "hidden");

  g->finetune = dt_bauhaus_slider_new_with_range_and_feedback(self, -9.0, 9.0, 1.0, 0.0, 0, feedback);
  dt_bauhaus_widget_set_label(g->finetune, NULL, _("finetune"));
  dt_bauhaus_slider_set_format(g->finetune, _("%.0f mired"));
  // initially doesn't have fine tuning stuff (camera wb)
  gtk_widget_set_sensitive(g->finetune, FALSE);
  gtk_box_pack_start(GTK_BOX(g->box_enabled), g->finetune, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->finetune, _("fine tune white balance preset"));

  gtk_widget_show_all(g->box_enabled);
  gtk_stack_add_named(GTK_STACK(g->stack), g->box_enabled, "enabled");

  g->label_disabled = gtk_label_new(_("white balance disabled for camera"));
  gtk_widget_set_halign(g->label_disabled, GTK_ALIGN_START);

  gtk_widget_show_all(g->label_disabled);
  gtk_stack_add_named(GTK_STACK(g->stack), g->label_disabled, "disabled");

  gtk_stack_set_visible_child_name(GTK_STACK(g->stack), self->hide_enable_button ? "disabled" : "enabled");

  //call to gui_update in gui_init is NOT needed.
  //self->gui_update(self);

  g_signal_connect(G_OBJECT(g->scale_k), "value-changed", G_CALLBACK(temp_callback), self);
  g_signal_connect(G_OBJECT(g->scale_tint), "value-changed", G_CALLBACK(tint_callback), self);

  g_signal_connect(G_OBJECT(g->scale_r), "value-changed", G_CALLBACK(rgb_callback), self);
  g_signal_connect(G_OBJECT(g->scale_g), "value-changed", G_CALLBACK(rgb_callback), self);
  g_signal_connect(G_OBJECT(g->scale_b), "value-changed", G_CALLBACK(rgb_callback), self);
  g_signal_connect(G_OBJECT(g->scale_g2), "value-changed", G_CALLBACK(rgb_callback), self);

  g_signal_connect(G_OBJECT(g->presets), "value-changed", G_CALLBACK(presets_changed), self);
  g_signal_connect(G_OBJECT(g->finetune), "value-changed", G_CALLBACK(finetune_changed), self);
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  const int preset = dt_bauhaus_combobox_get(g->presets);
  dt_iop_color_picker_reset(self, TRUE);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_asshot), preset == 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_user), preset == 3);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->btn_d65), preset == 1);

  dtgtk_expander_set_expanded(DTGTK_EXPANDER(g->coeffs_expander), g->expand_coeffs);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->coeffs_toggle), dtgtk_cairo_paint_solid_arrow,
                               CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX | (g->expand_coeffs?CPF_DIRECTION_DOWN:CPF_DIRECTION_LEFT), NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->coeffs_toggle), g->expand_coeffs);

  gui_sliders_update(self);

  color_finetuning_slider(self);
  color_rgb_sliders(self);
  color_temptint_sliders(self);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
