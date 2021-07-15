/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/rgb_norms.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/openmp_maths.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

#define INVERSE_SQRT_3 0.5773502691896258f

// To have your module compile and appear in darkroom, add it to CMakeLists.txt, with
//  add_iop(sigmoid "sigmoid.c")
// and to iop_order.c, in the initialisation of legacy_order & v30_order with:
//  { {XX.0f }, "sigmoid", 0},

// Module version number
DT_MODULE_INTROSPECTION(1, dt_iop_sigmoid_params_t)


// Enums used in params_t can have $DESCRIPTIONs that will be used to
// automatically populate a combobox with dt_bauhaus_combobox_from_params.
// They are also used in the history changes tooltip.
// Combobox options will be presented in the same order as defined here.
// These numbers must not be changed when a new version is introduced.

typedef enum dt_iop_sigmoid_methods_type_t
{
  DT_SIGMOID_METHOD_CROSSTALK = 0,     // $DESCRIPTION: "crosstalk"
  DT_SIGMOID_METHOD_HUE = 1,           // $DESCRIPTION: "preserve hue"
  DT_SIGMOID_METHOD_RGB_RATIO = 2      // $DESCRIPTION: "rgb ratio"
} dt_iop_sigmoid_methods_type_t;

typedef enum dt_iop_sigmoid_negative_values_type_t
{
  DT_SIGMOID_NEGATIVE_CLIP = 0,        // $DESCRIPTION: "clip"
  DT_SIGMOID_NEGATIVE_DESATURATE = 1,  // $DESCRIPTION: "desaturate"
  DT_SIGMOID_NEGATIVE_BRIGHTEN = 2     // $DESCRIPTION: "brighten"
} dt_iop_sigmoid_negative_values_type_t;

typedef enum dt_iop_sigmoid_norm_type_t
{
  DT_SIGMOID_METHOD_LUMINANCE = 0,      // $DESCRIPTION: "luminance Y"
  DT_SIGMOID_METHOD_AVERAGE = 1,        // $DESCRIPTION: "average"
  DT_SIGMOID_METHOD_EUCLIDEAN_NORM = 2, // $DESCRIPTION: "RGB euclidean norm"
  DT_SIGMOID_METHOD_POWER_NORM = 3,     // $DESCRIPTION: "RGB power norm"
  DT_SIGMOID_METHOD_MAX_RGB = 4,        // $DESCRIPTION: "max RGB"
} dt_iop_sigmoid_norm_type_t;

#define MIDDLE_GREY 0.1845f

typedef struct dt_iop_sigmoid_params_t
{
  // The parameters defined here fully record the state of the module and are stored
  // (as a serialized binary blob) into the db.
  // Make sure everything is in here does not depend on temporary memory (pointers etc).
  // This struct defines the layout of self->params and self->default_params.
  // You should keep changes to this struct to a minimum.
  // If you have to change this struct, it will break
  // user data bases, and you have to increment the version
  // of DT_MODULE_INTROSPECTION(VERSION) above and provide a legacy_params upgrade path!
  //
  // Tags in the comments get picked up by the introspection framework and are
  // used in gui_init to set range and labels (for widgets and history)
  // and value checks before commit_params.
  // If no explicit init() is specified, the default implementation uses $DEFAULT tags
  // to initialise self->default_params, which is then used in gui_init to set widget defaults.

  float middle_grey_contrast;  // $MIN: 0.1  $MAX: 4.0 $DEFAULT: 1.6 $DESCRIPTION: "contrast"
  float contrast_skewness;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "skew"
  float display_white_target;  // $MIN: 20.0  $MAX: 1600.0 $DEFAULT: 100.0 $DESCRIPTION: "target white"
  float display_grey_target;   // $MIN: 0.1  $MAX: 0.2 $DEFAULT: 0.1845 $DESCRIPTION: "target grey"
  float display_black_target;  // $MIN: 0.0  $MAX: 10.0 $DEFAULT: 0.0152 $DESCRIPTION: "target black"
  dt_iop_sigmoid_methods_type_t color_processing;  // $DEFAULT: DT_SIGMOID_METHOD_CROSSTALK $DESCRIPTION: "color processing"
  float crosstalk_amount;                          // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 4.0 $DESCRIPTION: "crosstalk amount"
  dt_iop_sigmoid_norm_type_t rgb_norm_method;      // $DEFAULT: DT_SIGMOID_METHOD_AVERAGE $DESCRIPTION: "luminance norm"
  dt_iop_sigmoid_negative_values_type_t negative_values_method; // $DEFAULT: DT_SIGMOID_NEGATIVE_DESATURATE $DESCRIPTION: "negative values"
} dt_iop_sigmoid_params_t;

typedef struct dt_iop_sigmoid_data_t
{
  float white_target;
  float black_target;
  float paper_exposure;
  float film_fog;
  float contrast_power;
  float skew_power;
  dt_iop_sigmoid_methods_type_t color_processing;
  dt_iop_sigmoid_negative_values_type_t negative_values_method;
  float crosstalk_amount;
  dt_iop_sigmoid_norm_type_t rgb_norm_method;
} dt_iop_sigmoid_data_t;

typedef struct dt_iop_sigmoid_global_data_t
{} dt_iop_sigmoid_global_data_t;

typedef struct dt_iop_sigmoid_gui_data_t
{
  // Whatever you need to make your gui happy and provide access to widgets between gui_init, gui_update etc.
  // Stored in self->gui_data while in darkroom.
  // To permanently store per-user gui configuration settings, you could use dt_conf_set/_get.
  GtkWidget *contrast_slider, *skewness_slider, *color_processing_list, *crosstalk_slider, *rgb_norm_method_list,
            *display_black_slider, *display_white_slider, *negative_values_list;
} dt_iop_sigmoid_gui_data_t;


// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("sigmoid");
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

// where does it appear in the gui?
int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

// Whenever new fields are added to (or removed from) dt_iop_..._params_t or when their meaning
// changes, a translation from the old to the new version must be added here.
// A verbatim copy of the old struct definition should be copied into the routine with a _v?_t ending.
// Since this will get very little future testing (because few developers still have very
// old versions lying around) existing code should be changed as little as possible, if at all.
//
// Upgrading from an older version than the previous one should always go through all in between versions
// (unless there was a bug) so that the end result will always be the same.
//
// Be careful with changes to structs that are included in _params_t
//
// Individually copy each existing field that is still in the new version. This is robust even if reordered.
// If only new fields were added at the end, one call can be used:
//   memcpy(n, o, sizeof *o);
//
// Hardcode the default values for new fields that were added, rather than referring to default_params;
// in future, the field may not exist anymore or the default may change. The best default for a new version
// to replicate a previous version might not be the optimal default for a fresh image.
//
// FIXME: the calling logic needs to be improved to call upgrades from consecutive version in sequence.
int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  return 1;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_sigmoid_params_t *params = (dt_iop_sigmoid_params_t *)p1;
  dt_iop_sigmoid_data_t *module_data = (dt_iop_sigmoid_data_t *)piece->data;
  /* Calculate actual skew log logistic parameters to fulfill the following:
   * f(scene_zero) = display_black_target 
   * f(scene_grey) = display_grey_target
   * f(scene_inf)  = display_white_target
   * Slope at scene_grey independet of skewness i.e. only changed by the contrast parameter.
   */

  module_data->skew_power = powf(5.0f, -params->contrast_skewness);
  module_data->contrast_power = powf(params->middle_grey_contrast, 1.0f / module_data->skew_power);
  module_data->white_target = 0.01f * params->display_white_target;
  module_data->black_target = 0.01f * params->display_black_target;
  const float white_grey_relation = powf(module_data->white_target / params->display_grey_target, 1.0f / module_data->skew_power) - 1.0f;
  module_data->film_fog = 0.0f;
  if (module_data->black_target > 0.0f) {
    const float white_black_relation = powf(module_data->white_target / module_data->black_target, 1.0f / module_data->skew_power) - 1.0f;
    module_data->film_fog = MIDDLE_GREY * powf(white_grey_relation, 1.0f / module_data->contrast_power) / (powf(white_black_relation, 1.0f / module_data->contrast_power) - powf(white_grey_relation, 1.0f / module_data->contrast_power));
  }
  module_data->paper_exposure = powf(module_data->film_fog + MIDDLE_GREY, module_data->contrast_power) * white_grey_relation;

  module_data->color_processing = params->color_processing;
  module_data->negative_values_method = params->negative_values_method;
  module_data->crosstalk_amount = fmaxf(1.0f - params->crosstalk_amount / 100.0f, 0.0f);
  module_data->rgb_norm_method = params->rgb_norm_method;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float pixel_rgb_norm_power(const float pixel[4])
{
  // weird norm sort of perceptual. This is black magic really, but it looks good.
  // the full norm is (R^3 + G^3 + B^3) / (R^2 + G^2 + B^2) and it should be in ]0; +infinity[

  float numerator = 0.0f;
  float denominator = 0.0f;

  for(int c = 0; c < 3; c++)
  {
    const float value = fabsf(pixel[c]);
    const float RGB_square = value * value;
    const float RGB_cubic = RGB_square * value;
    numerator += RGB_cubic;
    denominator += RGB_square;
  }

  return numerator / fmaxf(denominator, 1e-12f); // prevent from division-by-0 (note: (1e-6)^2 = 1e-12
}

static inline float rgb_luma(const float pixel[4], const dt_iop_order_iccprofile_info_t *const work_profile)
{
  return (work_profile)
            ? dt_workprofile_rgb_luminance(pixel, work_profile->matrix_in)
            : dt_camera_rgb_luminance(pixel);
}

#ifdef _OPENMP
#pragma omp declare simd uniform(variant, work_profile)
#endif
static inline float get_pixel_norm(const float pixel[4], const dt_iop_sigmoid_norm_type_t variant,
                                   const dt_iop_order_iccprofile_info_t *const work_profile)
{
  switch(variant)
  {
    case(DT_SIGMOID_METHOD_MAX_RGB):
      return fmaxf(fmaxf(pixel[0], pixel[1]), pixel[2]);

    case(DT_SIGMOID_METHOD_POWER_NORM):
      return pixel_rgb_norm_power(pixel);

    case(DT_SIGMOID_METHOD_EUCLIDEAN_NORM):
      return sqrtf(sqf(pixel[0]) + sqf(pixel[1]) + sqf(pixel[2])) * INVERSE_SQRT_3;

    case(DT_SIGMOID_METHOD_AVERAGE):
      return (pixel[0] + pixel[1] + pixel[2]) / 3.0f;

    case(DT_SIGMOID_METHOD_LUMINANCE):
    default:
      return rgb_luma(pixel, work_profile);
  }
}

#ifdef _OPENMP
#pragma omp declare simd uniform(method)
#endif
static inline void negative_values(const float pix_in[4], float pix_out[4], const dt_iop_sigmoid_negative_values_type_t method)
{
  float average;
  float min_value;
  float saturation_factor;

  switch(method)
  {
    case(DT_SIGMOID_NEGATIVE_BRIGHTEN):
      min_value = fminf(fminf(pix_in[0], pix_in[1]), fminf(pix_in[2], 0.0f));
      for(size_t c = 0; c < 3; c++)
      {
        pix_out[c] = pix_in[c] - min_value;
      }
      break;

    case(DT_SIGMOID_NEGATIVE_DESATURATE):
      average = fmaxf((pix_in[0] + pix_in[1] + pix_in[2]) / 3.0f, 0.0f);
      min_value = fminf(fminf(pix_in[0], pix_in[1]), pix_in[2]);
      saturation_factor = min_value < 0.0f ? -average / (min_value - average) : 1.0f;
      for(size_t c = 0; c < 3; c++)
      {
        pix_out[c] = average + saturation_factor * (pix_in[c] - average);
      }
      break;

    case(DT_SIGMOID_NEGATIVE_CLIP):
    default:
      for(size_t c = 0; c < 3; c++)
      {
        pix_out[c] = fmaxf(pix_in[c], 0.0);
      }
      break;
  }
}

// Return the middle value hue compensated such that the new color is only exposure and linear saturation change.
static inline float preserve_hue(const float maxval, const float maxvalold,
    const float medvalold, const float minval, const float minvalold)
{
  return minval + ((maxval - minval) * (medvalold - minvalold) / (maxvalold - minvalold));
}

#ifdef _OPENMP
#pragma omp declare simd uniform(magnitude, paper_exp, film_fog, film_power, paper_power)
#endif
static inline float generalized_loglogistic_sigmoid(const float value, const float magnitude, const float paper_exp,
                                                    const float film_fog, const float film_power, const float paper_power)
{
  // The following equation can be derived as a model for film + paper but it has a pole at 0
  // magnitude * powf(1.0 + paper_exp * powf(film_fog + value, -film_power), -paper_power);
  // Rewritten on a stable and with a check for negative values.
  const float film = value > 0.0f ? pow(value, film_power) : 0.0f;
  return magnitude * pow(film / (paper_exp + film), paper_power);
}

void process_loglogistic_crosstalk(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                                   const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_sigmoid_data_t *module_data = (dt_iop_sigmoid_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  const float white_target = module_data->white_target;
  const float paper_exp = module_data->paper_exposure;
  const float film_fog = module_data->film_fog;
  const float contrast_power = module_data->contrast_power;
  const float skew_power = module_data->skew_power;
  const float saturation_factor = module_data->crosstalk_amount;
  const dt_iop_sigmoid_negative_values_type_t negative_values_method = module_data->negative_values_method;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, work_profile, saturation_factor, negative_values_method, white_target, paper_exp, film_fog, contrast_power, skew_power) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4 * npixels; k += 4)
  { 
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    float DT_ALIGNED_ARRAY pix_in_strict_positive[4];

    // Force negative values to zero
    negative_values(pix_in, pix_in_strict_positive, negative_values_method);

    // Desature a bit to get proper roll off to white in highlights
    // This is taken from the ACES RRT implementation
    const float luma = rgb_luma(pix_in_strict_positive, work_profile);
    for(size_t c = 0; c < 3; c++)
    {
      const float desaturated_value = luma + saturation_factor * (pix_in_strict_positive[c] - luma);
      pix_out[c] = generalized_loglogistic_sigmoid(desaturated_value, white_target, paper_exp, film_fog, contrast_power, skew_power);
    }
    // Copy over the alpha channel
    pix_out[3] = pix_in[3];
  }
}

void process_loglogistic_ratio(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_sigmoid_data_t *module_data = (dt_iop_sigmoid_data_t *)piece->data;
  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  const float white_target = module_data->white_target;
  const float black_target = module_data->black_target;
  const float paper_exp = module_data->paper_exposure;
  const float film_fog = module_data->film_fog;
  const float contrast_power = module_data->contrast_power;
  const float skew_power = module_data->skew_power;

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  const dt_iop_sigmoid_norm_type_t rgb_norm_method = module_data->rgb_norm_method;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, work_profile, white_target, rgb_norm_method, black_target, paper_exp, film_fog, contrast_power, skew_power) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4 * npixels; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    float DT_ALIGNED_ARRAY pre_out[4];

    // Preserve color ratios by applying the tone curve on a luma estimate and then scale the RGB tripplet uniformly
    const float luma = get_pixel_norm(pix_in, rgb_norm_method, work_profile);
    const float mapped_luma = generalized_loglogistic_sigmoid(luma, white_target, paper_exp, film_fog, contrast_power, skew_power); 

    const float scaling_factor = mapped_luma / luma;
    for(size_t c = 0; c < 3; c++)
    {
      pre_out[c] = scaling_factor * pix_in[c];
    }
    
    // Some pixels will get out of gamut values, scale these into gamut again using desaturation.
    // Check for values larger then the white target
    const float max_pre_out = fmaxf(fmaxf(pre_out[0], pre_out[1]), pre_out[2]);
    const float sat_max = max_pre_out > white_target ? (white_target - mapped_luma) / (max_pre_out - mapped_luma) : 1.0f;

    // Check for values smaller then the black target
    const float min_pre_out = fminf(fminf(pre_out[0], pre_out[1]), pre_out[2]);
    const float sat_min = min_pre_out < black_target ? (black_target - mapped_luma) / (min_pre_out - mapped_luma) : 1.0f;

    // Use the smallest saturation factor of the two to gurantee inside of gamut, and never add saturation
    const float saturation_factor = fminf(fminf(sat_max, sat_min), 1.0f);

    for(size_t c = 0; c < 3; c++)
    {
      pix_out[c] = mapped_luma + saturation_factor * (pre_out[c] - mapped_luma);
    }
    // Copy over the alpha channel
    pix_out[3] = pix_in[3];
  }
}

void process_loglogistic_hue(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_sigmoid_data_t *module_data = (dt_iop_sigmoid_data_t *)piece->data;

  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  const float white_target = module_data->white_target;
  const float paper_exp = module_data->paper_exposure;
  const float film_fog = module_data->film_fog;
  const float contrast_power = module_data->contrast_power;
  const float skew_power = module_data->skew_power;
  const float saturation_factor = module_data->crosstalk_amount;
  const dt_iop_sigmoid_negative_values_type_t negative_values_method = module_data->negative_values_method;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, saturation_factor, negative_values_method, white_target, paper_exp, film_fog, contrast_power, skew_power) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4 * npixels; k += 4)
  { 
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    float DT_ALIGNED_ARRAY pix_in_strict_positive[4];

    // Force negative values to zero
    negative_values(pix_in, pix_in_strict_positive, negative_values_method);

    // Desature a bit to get proper roll off to white in highlights
    const float luma = (pix_in_strict_positive[0] + pix_in_strict_positive[1] + pix_in_strict_positive[2]) / 3.0f;
    for(size_t c = 0; c < 3; c++)
    {
      const float desaturated_value = luma + saturation_factor * (pix_in_strict_positive[c] - luma);
      pix_out[c] = generalized_loglogistic_sigmoid(desaturated_value, white_target, paper_exp, film_fog, contrast_power, skew_power);
    }

    // Hue correction by scaling the middle value relative to the max and min values.
    if (pix_in[0] >= pix_in[1])
    {
      if (pix_in[1] > pix_in[2])
      {  // Case 1: r >= g >  b
        pix_out[1] = preserve_hue(pix_out[0], pix_in[0], pix_in[1], pix_out[2], pix_in[2]);
      }
      else if (pix_in[2] > pix_in[0])
      {  // Case 2: b >  r >= g
        pix_out[0] = preserve_hue(pix_out[2], pix_in[2], pix_in[0], pix_out[1], pix_in[1]);
      }
      else if (pix_in[2] > pix_in[1])
      {  // Case 3: r >= b >  g
        pix_out[2] = preserve_hue(pix_out[0], pix_in[0], pix_in[2], pix_out[1], pix_in[1]);
      } else
      {  // Case 4: r == g == b
         // No change of the middle value.
      }
    }
    else
    {
      if (pix_in[0] >= pix_in[2])
      {  // Case 5: g >  r >= b
        pix_out[0] = preserve_hue(pix_out[1], pix_in[1], pix_in[0], pix_out[2], pix_in[2]);
      }
      else if (pix_in[2] > pix_in[1])
      {  // Case 6: b >  g >  r
        pix_out[1] = preserve_hue(pix_out[2], pix_in[2], pix_in[1], pix_out[0], pix_in[0]);
      }
      else
      {  // Case 7: g >= b >  r
        pix_out[2] = preserve_hue(pix_out[1], pix_in[1], pix_in[2], pix_out[0], pix_in[0]);
      }
    }

    // Copy over the alpha channel
    pix_out[3] = pix_in[3];
  }
}

/** process, all real work is done here. */
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  dt_iop_sigmoid_data_t *module_data = (dt_iop_sigmoid_data_t *)piece->data;

  if (module_data->color_processing == DT_SIGMOID_METHOD_CROSSTALK)
  {
    process_loglogistic_crosstalk(piece, ivoid, ovoid, roi_in, roi_out);
  }
  else if (module_data->color_processing == DT_SIGMOID_METHOD_RGB_RATIO)
  {
    process_loglogistic_ratio(piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_loglogistic_hue(piece, ivoid, ovoid, roi_in, roi_out);
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_sigmoid_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  module->data = malloc(sizeof(dt_iop_sigmoid_global_data_t));
}

void cleanup(dt_iop_module_t *module)
{
  // Releases any memory allocated in init(module)
  // Implement this function explicitly if the module allocates additional memory besides (default_)params.
  // this is rare.
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  free(module->data);
  module->data = NULL;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_sigmoid_gui_data_t *g = (dt_iop_sigmoid_gui_data_t *)self->gui_data;
  dt_iop_sigmoid_params_t *p = (dt_iop_sigmoid_params_t *)self->params;

  gtk_widget_set_visible(g->crosstalk_slider, p->color_processing != DT_SIGMOID_METHOD_RGB_RATIO);
  gtk_widget_set_visible(g->negative_values_list, p->color_processing != DT_SIGMOID_METHOD_RGB_RATIO);
  gtk_widget_set_visible(g->rgb_norm_method_list, p->color_processing == DT_SIGMOID_METHOD_RGB_RATIO);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_sigmoid_gui_data_t *g = (dt_iop_sigmoid_gui_data_t *)self->gui_data;
  dt_iop_sigmoid_params_t *p = (dt_iop_sigmoid_params_t *)self->params;

  dt_bauhaus_slider_set(g->contrast_slider, p->middle_grey_contrast);
  dt_bauhaus_slider_set(g->skewness_slider, p->contrast_skewness);

  dt_bauhaus_combobox_set_from_value(g->color_processing_list, p->color_processing);
  dt_bauhaus_slider_set(g->crosstalk_slider, p->crosstalk_amount);
  dt_bauhaus_combobox_set_from_value(g->negative_values_list, p->negative_values_method);
  dt_bauhaus_combobox_set_from_value(g->rgb_norm_method_list, p->rgb_norm_method);

  dt_bauhaus_slider_set(g->display_black_slider, p->display_black_target);
  dt_bauhaus_slider_set(g->display_white_slider, p->display_white_target);

  gui_changed(self, NULL, NULL);
}

void gui_init(dt_iop_module_t *self)
{
  // Allocates memory for the module's user interface in the darkroom and
  // sets up the widgets in it.
  //
  // self->widget needs to be set to the top level widget.
  // This can be a (vertical) box, a grid or even a notebook. Modules that are
  // disabled for certain types of images (for example non-raw) may use a stack
  // where one of the pages contains just a label explaining why it is disabled.
  //
  // Widgets that are directly linked to a field in params_t may be set up using the
  // dt_bauhaus_..._from_params family. They take a string with the field
  // name in the params_t struct definition. The $MIN, $MAX and $DEFAULT tags will be
  // used to set up the widget (slider) ranges and default values and the $DESCRIPTION
  // is used as the widget label.
  //
  // The _from_params calls also set up an automatic callback that updates the field in params
  // whenever the widget is changed. In addition, gui_changed is called, if it exists,
  // so that any other required changes, to dependent fields or to gui widgets, can be made.
  //
  // Whenever self->params changes (switching images or history) the widget values have to
  // be updated in gui_update.
  //
  // Do not set the value of widgets or configure them depending on field values here;
  // this should be done in gui_update (or gui_changed or individual widget callbacks)
  //
  // If any default values for (slider) widgets or options (in comboboxes) depend on the
  // type of image, then the widgets have to be updated in reload_params.
  dt_iop_sigmoid_gui_data_t *g = IOP_GUI_ALLOC(sigmoid);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // Look controls
  g->contrast_slider = dt_bauhaus_slider_from_params(self, "middle_grey_contrast");
  dt_bauhaus_slider_set_digits(g->contrast_slider, 3);
  g->skewness_slider = dt_bauhaus_slider_from_params(self, "contrast_skewness");

  // Color handling
  g->color_processing_list = dt_bauhaus_combobox_from_params(self, "color_processing");
  // Cross talk option
  g->crosstalk_slider = dt_bauhaus_slider_from_params(self, "crosstalk_amount");
  dt_bauhaus_slider_set_soft_range(g->crosstalk_slider, 0.0f, 10.0f);
  dt_bauhaus_slider_set_format(g->crosstalk_slider, "%.2f %%");
  g->negative_values_list = dt_bauhaus_combobox_from_params(self, "negative_values_method");
  // Constant RGB ratio option
  g->rgb_norm_method_list = dt_bauhaus_combobox_from_params(self, "rgb_norm_method");

  // Target display
  GtkWidget *label = dt_ui_section_label_new(_("display luminance"));
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(label));
  gtk_style_context_add_class(context, "section_label_top");
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  g->display_black_slider = dt_bauhaus_slider_from_params(self, "display_black_target");
  dt_bauhaus_slider_set_soft_range(g->display_black_slider, 0.0f, 1.0f);
  dt_bauhaus_slider_set_step(g->display_black_slider, .001);
  dt_bauhaus_slider_set_digits(g->display_black_slider, 4);
  dt_bauhaus_slider_set_format(g->display_black_slider, "%.3f %%");
  g->display_white_slider = dt_bauhaus_slider_from_params(self, "display_white_target");
  dt_bauhaus_slider_set_soft_range(g->display_white_slider, 50.0f, 100.0f);
  dt_bauhaus_slider_set_format(g->display_white_slider, "%.1f %%");
}

void gui_cleanup(dt_iop_module_t *self)
{
  // This only needs to be provided if gui_init allocates any memory or resources besides
  // self->widget and gui_data_t. The default function (if an explicit one isn't provided here)
  // takes care of gui_data_t (and gtk destroys the widget anyway). If you override the default,
  // you have to do whatever you have to do, and also call IOP_GUI_FREE to clean up gui_data_t.

  IOP_GUI_FREE;
}

/** additional, optional callbacks to capture darkroom center events. */
// void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
// int32_t pointery);
// int mouse_moved(dt_iop_module_t *self, double x, double y, double pressure, int which);
// int button_pressed(dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
// uint32_t state);
// int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
// int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
