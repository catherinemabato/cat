/*
    This file is part of darktable,
    copyright (c) 2018 Aurélien Pierre.

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

#include "basic.cl"

// In case the OpenCL driver doesn't have a dot method
inline float vdot(const float4 vec1, const float4 vec2)
{
    return vec1.x * vec2.x + vec1.y * vec2.y + vec1.z * vec2.z;
}

typedef enum dt_iop_filmicrgb_methods_type_t
{
  DT_FILMIC_METHOD_NONE = 0,
  DT_FILMIC_METHOD_MAX_RGB = 1,
  DT_FILMIC_METHOD_LUMINANCE = 2,
  DT_FILMIC_METHOD_POWER_NORM = 3,
  DT_FILMIC_METHOD_EUCLIDEAN_NORM = 4
} dt_iop_filmicrgb_methods_type_t;

typedef enum dt_iop_filmicrgb_colorscience_type_t
{
  DT_FILMIC_COLORSCIENCE_V1 = 0,
  DT_FILMIC_COLORSCIENCE_V2 = 1,
} dt_iop_filmicrgb_colorscience_type_t;


kernel void
filmic (read_only image2d_t in, write_only image2d_t out, int width, int height,
        const float dynamic_range, const float shadows_range, const float grey,
        read_only image2d_t table, read_only image2d_t diff,
        const float contrast, const float power, const int preserve_color,
        const float saturation)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 i = read_imagef(in, sampleri, (int2)(x, y));
  const float4 xyz = Lab_to_XYZ(i);
  float4 o = XYZ_to_prophotorgb(xyz);

  const float noise = pow(2.0f, -16.0f);
  const float4 noise4 = noise;
  const float4 dynamic4 = dynamic_range;
  const float4 shadows4 = shadows_range;
  float derivative, luma;

  // Global desaturation
  if (saturation != 1.0f)
  {
    const float4 lum = xyz.y;
    o = lum + (float4)saturation * (o - lum);
  }

  if (preserve_color)
  {

    // Save the ratios
    float maxRGB = max(max(o.x, o.y), o.z);
    const float4 ratios = o / (float4)maxRGB;

    // Log profile
    maxRGB = maxRGB / grey;
    maxRGB = (maxRGB < noise) ? noise : maxRGB;
    maxRGB = (native_log2(maxRGB) - shadows_range) / dynamic_range;
    maxRGB = clamp(maxRGB, 0.0f, 1.0f);

    const float index = maxRGB;

    // Curve S LUT
    maxRGB = lookup(table, (const float)maxRGB);

    // Re-apply the ratios
    o = (float4)maxRGB * ratios;

    // Derivative
    derivative = lookup(diff, (const float)index);
    luma = maxRGB;
  }
  else
  {
    // Log profile
    o = o / grey;
    o = (o < noise) ? noise : o;
    o = (native_log2(o) - shadows4) / dynamic4;
    o = clamp(o, (float4)0.0f, (float4)1.0f);

    const float index = prophotorgb_to_XYZ(o).y;

    // Curve S LUT
    o.x = lookup(table, (const float)o.x);
    o.y = lookup(table, (const float)o.y);
    o.z = lookup(table, (const float)o.z);

    // Get the derivative
    derivative = lookup(diff, (const float)index);
    luma = prophotorgb_to_XYZ(o).y;
  }

  // Desaturate selectively
  o = (float4)luma + (float4)derivative * (o - (float4)luma);
  o = clamp(o, (float4)0.0f, (float4)1.0f);

  // Apply the transfer function of the display
  const float4 power4 = power;
  o = native_powr(o, power4);

  i.xyz = prophotorgb_to_Lab(o).xyz;

  write_imagef(out, (int2)(x, y), i);
}


/* Norms */
inline float pixel_rgb_norm_power(const float4 pixel)
{
  // weird norm sort of perceptual. This is black magic really, but it looks good.
  const float4 RGB = fabs(pixel);
  const float4 RGB_square = RGB * RGB;
  const float4 RGB_cubic = RGB_square * RGB;
  return (RGB_cubic.x + RGB_cubic.y + RGB_cubic.z) / fmax(RGB_square.x + RGB_square.y + RGB_square.z, 1e-12f);
}

inline float pixel_rgb_norm_euclidean(const float4 pixel)
{
  const float4 RGB = pixel;
  const float4 RGB_square = RGB * RGB;
  return native_sqrt(RGB_square.x + RGB_square.y + RGB_square.z);
}

inline float get_pixel_norm(const float4 pixel, const dt_iop_filmicrgb_methods_type_t variant,
                            constant dt_colorspaces_iccprofile_info_cl_t *const profile_info,
                            read_only image2d_t lut, const int use_work_profile)
{
  switch(variant)
  {
    case DT_FILMIC_METHOD_MAX_RGB:
      return fmax(fmax(pixel.x, pixel.y), pixel.z);

    case DT_FILMIC_METHOD_LUMINANCE:
      return (use_work_profile) ? dt_camera_rgb_luminance(pixel): get_rgb_matrix_luminance(pixel, profile_info, profile_info->matrix_in, lut);

    case DT_FILMIC_METHOD_POWER_NORM:
      return pixel_rgb_norm_power(pixel);

    case DT_FILMIC_METHOD_EUCLIDEAN_NORM:
      return pixel_rgb_norm_euclidean(pixel);

    case DT_FILMIC_METHOD_NONE:
    default:
      // path cannot be taken
      return 0.0f;
  }
}

/* Saturation */

inline float filmic_desaturate_v1(const float x, const float sigma_toe, const float sigma_shoulder, const float saturation)
{
  const float radius_toe = x;
  const float radius_shoulder = 1.0f - x;

  const float key_toe = native_exp(-0.5f * radius_toe * radius_toe / sigma_toe);
  const float key_shoulder = native_exp(-0.5f * radius_shoulder * radius_shoulder / sigma_shoulder);

  return 1.0f - clamp((key_toe + key_shoulder) / saturation, 0.0f, 1.0f);
}


inline float filmic_desaturate_v2(const float x, const float sigma_toe, const float sigma_shoulder, const float saturation)
{
  const float radius_toe = x;
  const float radius_shoulder = 1.0f - x;
  const float sat2 = 0.5f / native_sqrt(saturation);
  const float key_toe = native_exp(-radius_toe * radius_toe / sigma_toe * sat2);
  const float key_shoulder = native_exp(-radius_shoulder * radius_shoulder / sigma_shoulder * sat2);

  return (saturation - (key_toe + key_shoulder) * (saturation));
}


inline float4 linear_saturation(const float4 x, const float luminance, const float saturation)
{
  return (float4)luminance + (float4)saturation * (x - (float4)luminance);
}


inline float filmic_spline(const float x,
                           const float4 M1, const float4 M2, const float4 M3, const float4 M4, const float4 M5,
                           const float latitude_min, const float latitude_max)
{
  // y = M5 * x⁴ + M4 * x³ + M3 * x² + M2 * x¹ + M1 * x⁰
  // but we rewrite it using Horner factorisation, to spare ops and enable FMA in available

  return (x < latitude_min) ? M1.x + x * (M2.x + x * (M3.x + x * (M4.x + x * M5.x))) : // toe
         (x > latitude_max) ? M1.y + x * (M2.y + x * (M3.y + x * (M4.y + x * M5.y))) : // shoulder
                              M1.z + x * (M2.z + x * (M3.z + x * (M4.z + x * M5.z)));  // latitude
}

#define NORM_MIN 1.52587890625e-05f // norm can't be < to 2^(-16)


inline float log_tonemapping_v1(const float x,
                                const float grey, const float black,
                                const float dynamic_range)
{
  const float temp = (native_log2(x / grey) - black) / dynamic_range;
  return clamp(temp, NORM_MIN, 1.f);
}

inline float log_tonemapping_v2(const float x,
                                const float grey, const float black,
                                const float dynamic_range)
{
  return clamp((native_log2(x / grey) - black) / dynamic_range, 0.f, 1.f);
}

inline float4 filmic_split_v1(const float4 i,
                              const float dynamic_range, const float black_exposure, const float grey_value,
                              constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                              read_only image2d_t lut, const int use_work_profile,
                              const float sigma_toe, const float sigma_shoulder, const float saturation,
                              const float4 M1, const float4 M2, const float4 M3, const float4 M4, const float4 M5,
                              const float latitude_min, const float latitude_max, const float output_power)
{
  float4 o;

  // Log tonemapping
  o.x = log_tonemapping_v1(fmax(i.x, NORM_MIN), grey_value, black_exposure, dynamic_range);
  o.y = log_tonemapping_v1(fmax(i.y, NORM_MIN), grey_value, black_exposure, dynamic_range);
  o.z = log_tonemapping_v1(fmax(i.z, NORM_MIN), grey_value, black_exposure, dynamic_range);

  // Selective desaturation of extreme luminances
  const float luminance = (use_work_profile) ? get_rgb_matrix_luminance(o,
                                                                        profile_info,
                                                                        profile_info->matrix_in,
                                                                        lut)
                                             : dt_camera_rgb_luminance(o);

  const float desaturation = filmic_desaturate_v1(luminance, sigma_toe, sigma_shoulder, saturation);
  o = linear_saturation(o, luminance, desaturation);

  // Filmic spline
  o.x = filmic_spline(o.x, M1, M2, M3, M4, M5, latitude_min, latitude_max);
  o.y = filmic_spline(o.y, M1, M2, M3, M4, M5, latitude_min, latitude_max);
  o.z = filmic_spline(o.z, M1, M2, M3, M4, M5, latitude_min, latitude_max);

  // Output power
  o = native_powr(clamp(o, (float4)0.0f, (float4)1.0f), output_power);

  return o;
}

inline float4 filmic_split_v2(const float4 i,
                              const float dynamic_range, const float black_exposure, const float grey_value,
                              constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                              read_only image2d_t lut, const int use_work_profile,
                              const float sigma_toe, const float sigma_shoulder, const float saturation,
                              const float4 M1, const float4 M2, const float4 M3, const float4 M4, const float4 M5,
                              const float latitude_min, const float latitude_max, const float output_power)
{
  float4 o;

  // Log tonemapping
  o.x = log_tonemapping_v2(fmax(i.x, NORM_MIN), grey_value, black_exposure, dynamic_range);
  o.y = log_tonemapping_v2(fmax(i.y, NORM_MIN), grey_value, black_exposure, dynamic_range);
  o.z = log_tonemapping_v2(fmax(i.z, NORM_MIN), grey_value, black_exposure, dynamic_range);

  // Selective desaturation of extreme luminances
  const float luminance = (use_work_profile) ? get_rgb_matrix_luminance(o,
                                                                        profile_info,
                                                                        profile_info->matrix_in,
                                                                        lut)
                                             : dt_camera_rgb_luminance(o);

  const float desaturation = filmic_desaturate_v2(luminance, sigma_toe, sigma_shoulder, saturation);
  o = linear_saturation(o, luminance, desaturation);

  // Filmic spline
  o.x = filmic_spline(o.x, M1, M2, M3, M4, M5, latitude_min, latitude_max);
  o.y = filmic_spline(o.y, M1, M2, M3, M4, M5, latitude_min, latitude_max);
  o.z = filmic_spline(o.z, M1, M2, M3, M4, M5, latitude_min, latitude_max);

  // Output power
  o = native_powr(clamp(o, (float4)0.0f, (float4)1.0f), output_power);

  return o;
}

kernel void
filmicrgb_split (read_only image2d_t in, write_only image2d_t out,
                 int width, int height,
                 const float dynamic_range, const float black_exposure, const float grey_value,
                 constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                 read_only image2d_t lut, const int use_work_profile,
                 const float sigma_toe, const float sigma_shoulder, const float saturation,
                 const float4 M1, const float4 M2, const float4 M3, const float4 M4, const float4 M5,
                 const float latitude_min, const float latitude_max, const float output_power,
                 const dt_iop_filmicrgb_colorscience_type_t color_science)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const float4 i = read_imagef(in, sampleri, (int2)(x, y));
  float4 o;

  switch(color_science)
  {
    case DT_FILMIC_COLORSCIENCE_V1:
    {
      o = filmic_split_v1(i, dynamic_range, black_exposure, grey_value,
                          profile_info, lut, use_work_profile,
                          sigma_toe, sigma_shoulder, saturation,
                          M1, M2, M3, M4, M5, latitude_min, latitude_max, output_power);
      break;
    }
    case DT_FILMIC_COLORSCIENCE_V2:
    {
      o = filmic_split_v2(i, dynamic_range, black_exposure, grey_value,
                          profile_info, lut, use_work_profile,
                          sigma_toe, sigma_shoulder, saturation,
                          M1, M2, M3, M4, M5, latitude_min, latitude_max, output_power);
      break;
    }
  }

  // Copy alpha layer and save
  o.w = i.w;
  write_imagef(out, (int2)(x, y), o);
}


inline float4 filmic_chroma_v1(const float4 i,
                               const float dynamic_range, const float black_exposure, const float grey_value,
                               constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                               read_only image2d_t lut, const int use_work_profile,
                               const float sigma_toe, const float sigma_shoulder, const float saturation,
                               const float4 M1, const float4 M2, const float4 M3, const float4 M4, const float4 M5,
                               const float latitude_min, const float latitude_max, const float output_power,
                               const dt_iop_filmicrgb_methods_type_t variant)
{
  float norm = fmax(get_pixel_norm(i, variant, profile_info, lut, use_work_profile), NORM_MIN);

  // Save the ratios
  float4 o = i / (float4)norm;

  // Sanitize the ratios
  const float min_ratios = fmin(fmin(o.x, o.y), o.z);
  if(min_ratios < 0.0f) o -= (float4)min_ratios;

  // Log tonemapping
  norm = log_tonemapping_v1(norm, grey_value, black_exposure, dynamic_range);

  // Selective desaturation of extreme luminances
  o *= (float4)norm;
  const float luminance = (use_work_profile) ? get_rgb_matrix_luminance(o,
                                                                        profile_info,
                                                                        profile_info->matrix_in,
                                                                        lut)
                                             : dt_camera_rgb_luminance(o);
  const float desaturation = filmic_desaturate_v1(norm, sigma_toe, sigma_shoulder, saturation);
  o = linear_saturation(o, luminance, desaturation);
  o /= (float4)norm;

  // Filmic S curve on the max RGB
  // Apply the transfer function of the display
  norm = native_powr(clamp(filmic_spline(norm, M1, M2, M3, M4, M5, latitude_min, latitude_max), 0.0f, 1.0f), output_power);

  return o * norm;
}


inline float4 filmic_chroma_v2(const float4 i,
                               const float dynamic_range, const float black_exposure, const float grey_value,
                               constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                               read_only image2d_t lut, const int use_work_profile,
                               const float sigma_toe, const float sigma_shoulder, const float saturation,
                               const float4 M1, const float4 M2, const float4 M3, const float4 M4, const float4 M5,
                               const float latitude_min, const float latitude_max, const float output_power,
                               const dt_iop_filmicrgb_methods_type_t variant)
{
  float norm = fmax(get_pixel_norm(i, variant, profile_info, lut, use_work_profile), NORM_MIN);

  // Save the ratios
  float4 ratios = i / (float4)norm;

  // Sanitize the ratios
  const float min_ratios = fmin(fmin(ratios.x, ratios.y), ratios.z);
  if(min_ratios < 0.0f) ratios -= (float4)min_ratios;

  // Log tonemapping
  norm = log_tonemapping_v2(norm, grey_value, black_exposure, dynamic_range);

  // Get the desaturation value based on the log value
  const float4 desaturation = (float4)filmic_desaturate_v2(norm, sigma_toe, sigma_shoulder, saturation);

  // Filmic S curve on the max RGB
  // Apply the transfer function of the display
  norm = native_powr(clamp(filmic_spline(norm, M1, M2, M3, M4, M5, latitude_min, latitude_max), 0.0f, 1.0f), output_power);

  // Re-apply ratios with saturation change
  ratios = fmax(ratios + ((float4)1.0f - ratios) * ((float4)1.0f - desaturation), (float4)0.f);

  float4 o = (float4)norm * ratios;

  // Gamut mapping
  const float max_pix = fmax(fmax(o.x, o.y), o.z);
  const int penalize = (max_pix > 1.0f);

  // Penalize the ratios by the amount of clipping
  if(penalize)
  {
    ratios = fmax(ratios + ((float4)1.0f - (float4)max_pix), (float4)0.0f);
    o = clamp((float4)norm * ratios, (float4)0.0f, (float4)1.0f);
  }

  return o;
}

kernel void
filmicrgb_chroma (read_only image2d_t in, write_only image2d_t out,
                 int width, int height,
                 const float dynamic_range, const float black_exposure, const float grey_value,
                 constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                 read_only image2d_t lut, const int use_work_profile,
                 const float sigma_toe, const float sigma_shoulder, const float saturation,
                 const float4 M1, const float4 M2, const float4 M3, const float4 M4, const float4 M5,
                 const float latitude_min, const float latitude_max, const float output_power,
                 const dt_iop_filmicrgb_methods_type_t variant,
                 const dt_iop_filmicrgb_colorscience_type_t color_science)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const float4 i = read_imagef(in, sampleri, (int2)(x, y));
  float4 o;

  switch(color_science)
  {
    case DT_FILMIC_COLORSCIENCE_V1:
    {
      o = filmic_chroma_v1(i, dynamic_range, black_exposure, grey_value,
                           profile_info, lut, use_work_profile,
                           sigma_toe, sigma_shoulder, saturation,
                           M1, M2, M3, M4, M5, latitude_min, latitude_max, output_power, variant);
      break;
    }
    case DT_FILMIC_COLORSCIENCE_V2:
    {
      o = filmic_chroma_v2(i, dynamic_range, black_exposure, grey_value,
                           profile_info, lut, use_work_profile,
                           sigma_toe, sigma_shoulder, saturation,
                           M1, M2, M3, M4, M5, latitude_min, latitude_max, output_power, variant);
      break;
    }
  }

  o.w = i.w;
  write_imagef(out, (int2)(x, y), o);
}
