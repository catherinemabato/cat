/*
    This file is part of darktable,
    copyright (c) 2021 darktable developers.

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

#include "common.h"
#include "noise_generator.h"


// Normalization scaling of the wavelet to approximate a laplacian
// from the function above for sigma = B_SPLINE_SIGMA as a constant
#define B_SPLINE_TO_LAPLACIAN 3.182727439285017f
#define B_SPLINE_TO_LAPLACIAN_2 10.129753952777762f // square


typedef enum dt_isotropy_t
{
  DT_ISOTROPY_ISOTROPE = 0, // diffuse in all directions with same intensity
  DT_ISOTROPY_ISOPHOTE = 1, // diffuse more in the isophote direction (orthogonal to gradient)
  DT_ISOTROPY_GRADIENT = 2  // diffuse more in the gradient direction
} dt_isotropy_t;


// Discretization parameters for the Partial Derivative Equation solver
#define H_STEP 1    // spatial step
#define KAPPA 0.25f // 0.25 if h = 1, 1 if h = 2


inline void find_gradient(const float4 pixels[9], float4 xy[2])
{
  // Compute the gradient with centered finite differences in a 3×3 stencil
  // warning : x is vertical, y is horizontal
  xy[0] = (pixels[7] - pixels[1]) / 2.f;
  xy[1] = (pixels[5] - pixels[3]) / 2.f;
}

inline void find_laplacian(const float4 pixels[9], float4 xy[2])
{
  // Compute the laplacian with centered finite differences in a 3×3 stencil
  // warning : x is vertical, y is horizontal
  xy[0] = (pixels[7] + pixels[1]) - 2.f * pixels[4];
  xy[1] = (pixels[5] + pixels[3]) - 2.f * pixels[4];
}

inline float4 sqf(const float4 in)
{
  return in * in;
}


inline void compute_kern(const float4 c2,
                           const float4 cos_theta_sin_theta,
                           const float4 cos_theta2, const float4 sin_theta2,
                           const dt_isotropy_t isotropy_type,
                           float4 kern[5])
{
  // Build the matrix of rotation with anisotropy

  const float4 b1 = c2 * cos_theta2 + sin_theta2;
  const float4 b2 = cos_theta2 + c2 * sin_theta2;
  const float4 b3 = (1.f - c2) * cos_theta_sin_theta;
  const bool is_gradient = isotropy_type == DT_ISOTROPY_GRADIENT;

  const float4 a11 = is_gradient ? b1 : b2;
  const float4 a22 = is_gradient ? b2 : b1;
  const float4 a12 = is_gradient ? b3 : -b3;

  // [ [  a12 / 2 + (a11 + a22) / 8,  a22 / 2,         -a12 / 2 + (a11 + a22) / 8 ],
  //   [  a11 / 2,                   -1.5 (a11 + a22),  a11 / 2                   ],
  //   [ -a12 / 2 + (a11 + a22) / 8,  a22 / 2,          a12 / 2 + (a11 + a22) / 8 ] ]
  // but modified to reduce to the isotropic Laplacian when a12 goes to zero,
  // see https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Second-order-isotropic-finite-differences
  // for references (Oono & Puri).
  // N.B. we also have flipped the signs of the a12 terms
  // compared to the paper. There's probably a mismatch
  // of coordinate convention between the paper and the
  // original derivation of this convolution mask
  // (Witkin 1991, https://doi.org/10.1145/127719.122750).
  const float4 a11a22 = a11 + a22;
  kern[0] = a12 / 2.f + a11a22 / 8.f;
  kern[1] = a22 / 2.f;
  kern[2] = -a12 / 2.f + a11a22 / 8.f;
  kern[3] = a11 / 2.f;
  kern[4] = -1.5f * a11a22;
}


inline float4 convolve(const float4 kern[5], const float4 neighbours[9])
{
  return kern[4] * neighbours[4] +
    kern[0] * (neighbours[0] + neighbours[8]) +
    kern[1] * (neighbours[1] + neighbours[7]) +
    kern[2] * (neighbours[2] + neighbours[6]) +
    kern[3] * (neighbours[3] + neighbours[5]);
}


kernel void
diffuse_pde(read_only image2d_t HF, read_only image2d_t LF,
            read_only image2d_t mask, const int has_mask,
            write_only image2d_t output,
            const int width, const int height,
            const float4 anisotropy, const int4 isotropy_type,
            const float regularization, const float variance_threshold,
            const float current_radius_square, const int mult,
            const float4 ABCD, const float strength)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const char opacity = (has_mask) ? read_imageui(mask, sampleri, (int2)(x, y)).x : 1;

  const float4 regularization_factor = regularization * current_radius_square / 9.f;

  float4 out;

  if(opacity)
  {
    // non-local neighbours coordinates
    const int j_neighbours[3] = {
      clamp((x - mult * H_STEP), 0, width - 1),
      x,
      clamp((x + mult * H_STEP), 0, width - 1) };
    const int i_neighbours[3] = {
      clamp((y - mult * H_STEP), 0, height - 1),
      y,
      clamp((y + mult * H_STEP), 0, height - 1) };

    // fetch non-local pixels and store them locally and contiguously
    float4 neighbour_pixel_HF[9];
    float4 neighbour_pixel_LF[9];

    for(int ii = 0; ii < 3; ii++)
      for(int jj = 0; jj < 3; jj++)
      {
        neighbour_pixel_HF[3 * ii + jj] = read_imagef(HF, samplerA, (int2)(j_neighbours[ii], i_neighbours[jj]));
        neighbour_pixel_LF[3 * ii + jj] = read_imagef(LF, samplerA, (int2)(j_neighbours[ii], i_neighbours[jj]));
      }

    // build the local anisotropic convolution filters for gradients and laplacians

    float4 gradient[2];
    find_gradient(neighbour_pixel_LF, gradient);
    const float4 magnitude_grad = native_sqrt(sqf(gradient[0]) + sqf(gradient[1]));
    // Compute cos(arg(grad)) = dx / hypot - force arg(grad) = 0 if hypot == 0
    gradient[0] = (magnitude_grad != 0.f) ? gradient[0] / magnitude_grad
                                          : 1.f; // cos(0)
    // Compute sin (arg(grad))= dy / hypot - force arg(grad) = 0 if hypot == 0
    gradient[1] = (magnitude_grad != 0.f) ? gradient[1] / magnitude_grad
                                          : 0.f; // sin(0)
    // Warning : now gradient[2] = { cos(arg(grad)) , sin(arg(grad)) }
    const float4 cos_theta_grad_sq = sqf(gradient[0]);
    const float4 sin_theta_grad_sq = sqf(gradient[1]);
    const float4 cos_theta_sin_theta_grad = gradient[0] * gradient[1];

    float4 laplacian[2];
    find_gradient(neighbour_pixel_HF, laplacian);
    const float4 magnitude_lapl = native_sqrt(sqf(laplacian[0]) + sqf(laplacian[1]));
    // Compute cos(arg(lapl)) = dx / hypot - force arg(lapl) = 0 if hypot == 0
    laplacian[0] = (magnitude_lapl != 0.f) ? laplacian[0] / magnitude_lapl
                                           : 1.f; // cos(0)
    // Compute sin (arg(lapl))= dy / hypot - force arg(lapl) = 0 if hypot == 0
    laplacian[1] = (magnitude_lapl != 0.f) ? laplacian[1] / magnitude_lapl
                                           : 0.f; // sin(0)
    // Warning : now laplacian[2] = { cos(arg(lapl)) , sin(arg(lapl)) }
    const float4 cos_theta_lapl_sq = sqf(laplacian[0]);
    const float4 sin_theta_lapl_sq = sqf(laplacian[1]);
    const float4 cos_theta_sin_theta_lapl = laplacian[0] * laplacian[1];

    // c² in https://www.researchgate.net/publication/220663968
    // warning : in c2[s], s is the order of the derivative
    const float4 c2[4] = { native_exp(-magnitude_grad * anisotropy.x),
                           native_exp(-magnitude_lapl * anisotropy.y),
                           native_exp(-magnitude_grad * anisotropy.z),
                           native_exp(-magnitude_lapl * anisotropy.w) };

    float4 kern_first[5], kern_second[5], kern_third[5], kern_fourth[5];
    compute_kern(c2[0], cos_theta_sin_theta_grad, cos_theta_grad_sq, sin_theta_grad_sq, isotropy_type.x, kern_first);
    compute_kern(c2[1], cos_theta_sin_theta_lapl, cos_theta_lapl_sq, sin_theta_lapl_sq, isotropy_type.y, kern_second);
    compute_kern(c2[2], cos_theta_sin_theta_grad, cos_theta_grad_sq, sin_theta_grad_sq, isotropy_type.z, kern_third);
    compute_kern(c2[3], cos_theta_sin_theta_lapl, cos_theta_lapl_sq, sin_theta_lapl_sq, isotropy_type.w, kern_fourth);

    // convolve filters and compute the variance and the regularization term
    float4 derivatives[4] = {
      convolve(kern_first, neighbour_pixel_LF),
      convolve(kern_second, neighbour_pixel_LF),
      convolve(kern_third, neighbour_pixel_HF),
      convolve(kern_fourth, neighbour_pixel_HF),
    };

    float4 variance = (float4)0.f;
    #pragma unroll
    for(int k = 0; k < 9; k++)
    {
      variance += sqf(neighbour_pixel_HF[k]);
    }

    // Regularize the variance taking into account the blurring scale.
    // This allows to keep the scene-referred variance roughly constant
    // regardless of the wavelet scale where we compute it.
    // Prevents large scale halos when deblurring.
    variance = variance_threshold + variance * regularization_factor;

    // compute the update
    float4 acc = (float4)0.f;
    for(int k = 0; k < 4; k++) acc += derivatives[k] * ((float *)&ABCD)[k];
    acc = (neighbour_pixel_HF[4] * strength + acc / variance);

    // update the solution
    out = fmax(acc + neighbour_pixel_LF[4], 0.f);
  }
  else
  {
    float4 hf = read_imagef(HF, samplerA, (int2)(x, y));
    float4 lf = read_imagef(LF, samplerA, (int2)(x, y));
    out = hf + lf;
  }

  write_imagef(output, (int2)(x, y), out);
}

kernel void
build_mask(read_only image2d_t in, write_only image2d_t mask,
           const float threshold, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;
  float4 pix_in = read_imagef(in, samplerA, (int2)(x, y));
  float m = (pix_in.x > threshold || pix_in.y > threshold || pix_in.z > threshold);
  write_imageui(mask, (int2)(x, y), m);
}

kernel void
inpaint_mask(write_only image2d_t inpainted, read_only image2d_t original,
              read_only image2d_t mask, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;
  float4 pix_in = read_imagef(original, samplerA, (int2)(x, y));
  char m = read_imageui(mask, samplerA, (int2)(x, y)).x;
  float4 pix_out = pix_in;

  if(m)
  {
    unsigned int state[4] = { splitmix32(x + 1), splitmix32((x + 1) * (y + 3)), splitmix32(1337), splitmix32(666) };
    xoshiro128plus(state);
    xoshiro128plus(state);
    xoshiro128plus(state);
    xoshiro128plus(state);
    pix_out = fabs(gaussian_noise_simd(pix_in, pix_in, state));
  }

  write_imagef(inpainted, (int2)(x, y), pix_out);
}
