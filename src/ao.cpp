// NativeXRenderer - see ao.h.
//
// The compute shaders below are a GLSL port of XeGTAO 1.30's XeGTAO.hlsli and vaGTAO.hlsl
// (https://github.com/GameTechDev/XeGTAO; Copyright (C) 2016-2021, Intel Corporation; SPDX-License-Identifier: MIT;
// THIRD_PARTY_NOTICES.md), with its default constants (radius multiplier 1.457, falloff range 0.615, sample
// distribution power 2, no thin occluder compensation, final value power 2.2, depth level offset 3.3), 32-bit
// view-space depths, the normals made from them in the occlusion pass (XE_GTAO_GENERATE_NORMALS_INPLACE) and no bent
// normals. The occlusion term and the depth edges share one RGBA8 storage image (x, y): R8 is not a storage format
// every Vulkan device has.
//
// At half resolution (Settings::half, D184) the working size is half the view's width and height: each working
// pixel's depth is the closest of its 2x2 (a real surface, so the normals made from the depths stay smooth and thin
// objects are kept), the occlusion and its denoise run on a quarter of the pixels, the luminance fade reads the 2x2's
// mean color, and the composite enlarges the multiplier onto the view with weights by depth (the four nearest working
// pixels, bilinear, those at another depth left out; at a silhouette where none matches, the closest in depth of the
// 4x4 around), the background (the far plane) tested at the view's own depth.

#include "ao.h"

#include "glsl.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace native_x_renderer {

using namespace Diligent;

namespace {

constexpr int kDepthLevels = 5;

// std140, as GtaoConstants below.
struct Constants {
  float viewport[4];      // width, height, 1 / width, 1 / height
  float depth_unpack[4];  // linearize mul, linearize add, viewport z min, 1 / (z max - z min)
  float ndc_to_view[4];   // mul x, mul y, add x, add y: a screen position (0-1, top left 0, 0) to view x, y at depth 1
  float params0[4];       // effect radius, unused, final value power, denoise blur beta
  float params1[4];       // depth level offset, the multiplier's exponent, bright start, the background's depth
  int32_t params2[4];     // noise index, the view's width and height (the working size is `viewport`'s), unused
  float params3[4];       // D188: x 1 when the view's surfaces were copied (CopySurfaces), y 1 for the first copy
};

constexpr const char* kCommon = R"(
#version 450
layout(set = 0, binding = 0, std140) uniform GtaoConstants {
  vec4 viewport;
  vec4 depth_unpack;
  vec4 ndc_to_view;
  vec4 params0;
  vec4 params1;
  ivec4 params2;
  vec4 params3;
};
#define XE_GTAO_PI 3.1415926535897932384626433832795
#define XE_GTAO_PI_HALF 1.5707963267948966192313216916398
#define XE_GTAO_RADIUS_MULTIPLIER 1.457
#define XE_GTAO_FALLOFF_RANGE 0.615
#define XE_GTAO_SAMPLE_DISTRIBUTION_POWER 2.0
#define XE_GTAO_OCCLUSION_TERM_SCALE 1.5
)";

// Pass 1: XeGTAO_PrefilterDepths16x16. Each thread takes 2x2 working pixels; a group of 8x8, 16x16 pixels and their
// four smaller levels. HALF_RES: a working pixel is the closest of the view's 2x2.
constexpr const char* kPrefilter = R"(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D src_depth;
layout(set = 0, binding = 2, r32f) uniform writeonly image2D out_depth0;
layout(set = 0, binding = 3, r32f) uniform writeonly image2D out_depth1;
layout(set = 0, binding = 4, r32f) uniform writeonly image2D out_depth2;
layout(set = 0, binding = 5, r32f) uniform writeonly image2D out_depth3;
layout(set = 0, binding = 6, r32f) uniform writeonly image2D out_depth4;
shared float g_scratch_depths[8][8];

float ViewDepth(ivec2 p) {
  ivec2 size = params2.yz;
  float d = texelFetch(src_depth, min(p, size - 1), 0).x;
  d = (d - depth_unpack.z) * depth_unpack.w;
  return clamp(depth_unpack.x / (depth_unpack.y - d), 0.0, 3.402823466e+38);
}

float WorkDepth(ivec2 p) {
#if HALF_RES
  ivec2 q = p * 2;
  return min(min(ViewDepth(q), ViewDepth(q + ivec2(1, 0))), min(ViewDepth(q + ivec2(0, 1)), ViewDepth(q + ivec2(1, 1))));
#else
  return ViewDepth(p);
#endif
}

float DepthMipFilter(float depth0, float depth1, float depth2, float depth3) {
  float max_depth = max(max(depth0, depth1), max(depth2, depth3));
  const float depth_range_scale_factor = 0.75;
  float effect_radius = depth_range_scale_factor * params0.x * XE_GTAO_RADIUS_MULTIPLIER;
  float falloff_range = XE_GTAO_FALLOFF_RANGE * effect_radius;
  float falloff_from = effect_radius * (1.0 - XE_GTAO_FALLOFF_RANGE);
  float falloff_mul = -1.0 / falloff_range;
  float falloff_add = falloff_from / falloff_range + 1.0;
  float weight0 = clamp((max_depth - depth0) * falloff_mul + falloff_add, 0.0, 1.0);
  float weight1 = clamp((max_depth - depth1) * falloff_mul + falloff_add, 0.0, 1.0);
  float weight2 = clamp((max_depth - depth2) * falloff_mul + falloff_add, 0.0, 1.0);
  float weight3 = clamp((max_depth - depth3) * falloff_mul + falloff_add, 0.0, 1.0);
  float weight_sum = weight0 + weight1 + weight2 + weight3;
  return (weight0 * depth0 + weight1 * depth1 + weight2 * depth2 + weight3 * depth3) / weight_sum;
}

void Store(int level, ivec2 p, float value) {
  ivec2 size = max(ivec2(viewport.xy) >> level, ivec2(1));
  if (p.x >= size.x || p.y >= size.y) return;
  if (level == 0) imageStore(out_depth0, p, vec4(value));
  else if (level == 1) imageStore(out_depth1, p, vec4(value));
  else if (level == 2) imageStore(out_depth2, p, vec4(value));
  else if (level == 3) imageStore(out_depth3, p, vec4(value));
  else imageStore(out_depth4, p, vec4(value));
}

void main() {
  uvec2 base = gl_GlobalInvocationID.xy;
  uvec2 group = gl_LocalInvocationID.xy;
  ivec2 pix = ivec2(base) * 2;
  float depth0 = WorkDepth(pix + ivec2(0, 0));
  float depth1 = WorkDepth(pix + ivec2(1, 0));
  float depth2 = WorkDepth(pix + ivec2(0, 1));
  float depth3 = WorkDepth(pix + ivec2(1, 1));
  Store(0, pix + ivec2(0, 0), depth0);
  Store(0, pix + ivec2(1, 0), depth1);
  Store(0, pix + ivec2(0, 1), depth2);
  Store(0, pix + ivec2(1, 1), depth3);

  float dm1 = DepthMipFilter(depth0, depth1, depth2, depth3);
  Store(1, ivec2(base), dm1);
  g_scratch_depths[group.x][group.y] = dm1;
  memoryBarrierShared();
  barrier();

  if (all(equal(group % 2u, uvec2(0)))) {
    float dm2 = DepthMipFilter(g_scratch_depths[group.x][group.y], g_scratch_depths[group.x + 1u][group.y],
                               g_scratch_depths[group.x][group.y + 1u], g_scratch_depths[group.x + 1u][group.y + 1u]);
    Store(2, ivec2(base / 2u), dm2);
    g_scratch_depths[group.x][group.y] = dm2;
  }
  memoryBarrierShared();
  barrier();

  if (all(equal(group % 4u, uvec2(0)))) {
    float dm3 = DepthMipFilter(g_scratch_depths[group.x][group.y], g_scratch_depths[group.x + 2u][group.y],
                               g_scratch_depths[group.x][group.y + 2u], g_scratch_depths[group.x + 2u][group.y + 2u]);
    Store(3, ivec2(base / 4u), dm3);
    g_scratch_depths[group.x][group.y] = dm3;
  }
  memoryBarrierShared();
  barrier();

  if (all(equal(group % 8u, uvec2(0)))) {
    float dm4 = DepthMipFilter(g_scratch_depths[group.x][group.y], g_scratch_depths[group.x + 4u][group.y],
                               g_scratch_depths[group.x][group.y + 4u], g_scratch_depths[group.x + 4u][group.y + 4u]);
    Store(4, ivec2(base / 8u), dm4);
  }
}
)";

// Pass 2: XeGTAO_MainPass with the normals made in place (32-bit depths) and the Hilbert curve's R2 noise.
constexpr const char* kMain = R"(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D src_view_depth;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D out_term;

vec3 ViewspacePosition(vec2 screen_pos, float viewspace_depth) {
  return vec3((ndc_to_view.xy * screen_pos + ndc_to_view.zw) * viewspace_depth, viewspace_depth);
}

vec4 CalculateEdges(float center_z, float left_z, float right_z, float top_z, float bottom_z) {
  vec4 edges_lrtb = vec4(left_z, right_z, top_z, bottom_z) - center_z;
  float slope_lr = (edges_lrtb.y - edges_lrtb.x) * 0.5;
  float slope_tb = (edges_lrtb.w - edges_lrtb.z) * 0.5;
  vec4 edges_lrtb_slope_adjusted = edges_lrtb + vec4(slope_lr, -slope_lr, slope_tb, -slope_tb);
  edges_lrtb = min(abs(edges_lrtb), abs(edges_lrtb_slope_adjusted));
  return clamp(1.25 - edges_lrtb / (center_z * 0.011), 0.0, 1.0);
}

float PackEdges(vec4 edges_lrtb) {
  edges_lrtb = round(clamp(edges_lrtb, 0.0, 1.0) * 2.9);
  return dot(edges_lrtb, vec4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}

vec3 CalculateNormal(vec4 edges_lrtb, vec3 center, vec3 left, vec3 right, vec3 top, vec3 bottom) {
  vec4 accepted = clamp(vec4(edges_lrtb.x * edges_lrtb.z, edges_lrtb.z * edges_lrtb.y, edges_lrtb.y * edges_lrtb.w,
                             edges_lrtb.w * edges_lrtb.x) + 0.01, 0.0, 1.0);
  left = normalize(left - center);
  right = normalize(right - center);
  top = normalize(top - center);
  bottom = normalize(bottom - center);
  vec3 normal = accepted.x * cross(left, top) + accepted.y * cross(top, right) + accepted.z * cross(right, bottom) +
                accepted.w * cross(bottom, left);
  return normalize(normal);
}

float FastSqrt(float x) { return intBitsToFloat(0x1fbd1df5 + (floatBitsToInt(x) >> 1)); }

float FastACos(float in_x) {
  const float pi = 3.141593;
  const float half_pi = 1.570796;
  float x = abs(in_x);
  float res = -0.156583 * x + half_pi;
  res *= FastSqrt(1.0 - x);
  return in_x >= 0.0 ? res : pi - res;
}

uint HilbertIndex(uint pos_x, uint pos_y) {
  uint index = 0u;
  for (uint level = 32u; level > 0u; level /= 2u) {
    uint region_x = (pos_x & level) > 0u ? 1u : 0u;
    uint region_y = (pos_y & level) > 0u ? 1u : 0u;
    index += level * level * ((3u * region_x) ^ region_y);
    if (region_y == 0u) {
      if (region_x == 1u) {
        pos_x = 63u - pos_x;
        pos_y = 63u - pos_y;
      }
      uint temp = pos_x;
      pos_x = pos_y;
      pos_y = temp;
    }
  }
  return index;
}

vec2 SpatioTemporalNoise(uvec2 pix, uint temporal_index) {
  uint index = HilbertIndex(pix.x % 64u, pix.y % 64u);
  index += 288u * (temporal_index % 64u);
  return fract(0.5 + float(index) * vec2(0.75487766624669276005, 0.5698402909980532659114));
}

void main() {
  ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
  ivec2 size = ivec2(viewport.xy);
  if (pix.x >= size.x || pix.y >= size.y) return;
  vec2 pixel_size = viewport.zw;
  vec2 normalized_screen_pos = (vec2(pix) + 0.5) * pixel_size;

  vec4 values_ul = textureGather(src_view_depth, vec2(pix) * pixel_size, 0);
  vec4 values_br = textureGatherOffset(src_view_depth, vec2(pix) * pixel_size, ivec2(1, 1), 0);
  float viewspace_z = values_ul.y;
  float pix_lz = values_ul.x;
  float pix_tz = values_ul.z;
  float pix_rz = values_br.z;
  float pix_bz = values_br.x;

  vec4 edges_lrtb = CalculateEdges(viewspace_z, pix_lz, pix_rz, pix_tz, pix_bz);
  float packed_edges = PackEdges(edges_lrtb);

  vec3 center_pos = ViewspacePosition(normalized_screen_pos, viewspace_z);
  vec3 left_pos = ViewspacePosition(normalized_screen_pos + vec2(-1.0, 0.0) * pixel_size, pix_lz);
  vec3 right_pos = ViewspacePosition(normalized_screen_pos + vec2(1.0, 0.0) * pixel_size, pix_rz);
  vec3 top_pos = ViewspacePosition(normalized_screen_pos + vec2(0.0, -1.0) * pixel_size, pix_tz);
  vec3 bottom_pos = ViewspacePosition(normalized_screen_pos + vec2(0.0, 1.0) * pixel_size, pix_bz);
  vec3 viewspace_normal = CalculateNormal(edges_lrtb, center_pos, left_pos, right_pos, top_pos, bottom_pos);

  viewspace_z *= 0.99999;  // 32-bit depths
  vec3 pix_center_pos = ViewspacePosition(normalized_screen_pos, viewspace_z);
  vec3 view_vec = normalize(-pix_center_pos);

  float effect_radius = params0.x * XE_GTAO_RADIUS_MULTIPLIER;
  float falloff_range = XE_GTAO_FALLOFF_RANGE * effect_radius;
  float falloff_from = effect_radius * (1.0 - XE_GTAO_FALLOFF_RANGE);
  float falloff_mul = -1.0 / falloff_range;
  float falloff_add = falloff_from / falloff_range + 1.0;

  float visibility = 0.0;
  vec2 local_noise = SpatioTemporalNoise(uvec2(pix), uint(params2.x));
  float noise_slice = local_noise.x;
  float noise_sample = local_noise.y;
  const float pixel_too_close_threshold = 1.3;
  vec2 pixel_dir_rb_viewspace_size_at_center_z = viewspace_z * (ndc_to_view.xy * pixel_size);
  float screenspace_radius = effect_radius / pixel_dir_rb_viewspace_size_at_center_z.x;
  visibility += clamp((10.0 - screenspace_radius) / 100.0, 0.0, 1.0) * 0.5;
  float min_s = pixel_too_close_threshold / screenspace_radius;

  for (int slice = 0; slice < SLICE_COUNT; slice++) {
    float slice_k = (float(slice) + noise_slice) / float(SLICE_COUNT);
    float phi = slice_k * XE_GTAO_PI;
    float cos_phi = cos(phi);
    float sin_phi = sin(phi);
    vec2 omega = vec2(cos_phi, -sin_phi) * screenspace_radius;
    vec3 direction_vec = vec3(cos_phi, sin_phi, 0.0);
    vec3 ortho_direction_vec = direction_vec - dot(direction_vec, view_vec) * view_vec;
    vec3 axis_vec = normalize(cross(ortho_direction_vec, view_vec));
    vec3 projected_normal_vec = viewspace_normal - axis_vec * dot(viewspace_normal, axis_vec);
    float sign_norm = sign(dot(ortho_direction_vec, projected_normal_vec));
    float projected_normal_vec_length = length(projected_normal_vec);
    float cos_norm = clamp(dot(projected_normal_vec, view_vec) / projected_normal_vec_length, 0.0, 1.0);
    float n = sign_norm * FastACos(cos_norm);
    float low_horizon_cos0 = cos(n + XE_GTAO_PI_HALF);
    float low_horizon_cos1 = cos(n - XE_GTAO_PI_HALF);
    float horizon_cos0 = low_horizon_cos0;
    float horizon_cos1 = low_horizon_cos1;

    for (int step_index = 0; step_index < STEPS_PER_SLICE; step_index++) {
      float step_base_noise = float(slice + step_index * STEPS_PER_SLICE) * 0.6180339887498948482;
      float step_noise = fract(noise_sample + step_base_noise);
      float s = (float(step_index) + step_noise) / float(STEPS_PER_SLICE);
      s = pow(s, XE_GTAO_SAMPLE_DISTRIBUTION_POWER);
      s += min_s;
      vec2 sample_offset = s * omega;
      float sample_offset_length = length(sample_offset);
      float mip_level = clamp(log2(sample_offset_length) - params1.x, 0.0, 5.0);
      sample_offset = round(sample_offset) * pixel_size;

      vec2 sample_screen_pos0 = normalized_screen_pos + sample_offset;
      float sz0 = textureLod(src_view_depth, sample_screen_pos0, mip_level).x;
      vec3 sample_pos0 = ViewspacePosition(sample_screen_pos0, sz0);
      vec2 sample_screen_pos1 = normalized_screen_pos - sample_offset;
      float sz1 = textureLod(src_view_depth, sample_screen_pos1, mip_level).x;
      vec3 sample_pos1 = ViewspacePosition(sample_screen_pos1, sz1);

      vec3 sample_delta0 = sample_pos0 - pix_center_pos;
      vec3 sample_delta1 = sample_pos1 - pix_center_pos;
      float sample_dist0 = length(sample_delta0);
      float sample_dist1 = length(sample_delta1);
      vec3 sample_horizon_vec0 = sample_delta0 / sample_dist0;
      vec3 sample_horizon_vec1 = sample_delta1 / sample_dist1;
      float weight0 = clamp(sample_dist0 * falloff_mul + falloff_add, 0.0, 1.0);
      float weight1 = clamp(sample_dist1 * falloff_mul + falloff_add, 0.0, 1.0);
      float shc0 = dot(sample_horizon_vec0, view_vec);
      float shc1 = dot(sample_horizon_vec1, view_vec);
      shc0 = mix(low_horizon_cos0, shc0, weight0);
      shc1 = mix(low_horizon_cos1, shc1, weight1);
      horizon_cos0 = max(horizon_cos0, shc0);
      horizon_cos1 = max(horizon_cos1, shc1);
    }

    projected_normal_vec_length = mix(projected_normal_vec_length, 1.0, 0.05);
    float h0 = -FastACos(horizon_cos1);
    float h1 = FastACos(horizon_cos0);
    float iarc0 = (cos_norm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
    float iarc1 = (cos_norm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
    visibility += projected_normal_vec_length * (iarc0 + iarc1);
  }
  visibility /= float(SLICE_COUNT);
  visibility = pow(visibility, params0.z);
  visibility = max(0.03, visibility);
  visibility = clamp(visibility / XE_GTAO_OCCLUSION_TERM_SCALE, 0.0, 1.0);
  imageStore(out_term, pix, vec4(visibility, packed_edges, 0.0, 0.0));
}
)";

// D188: the view's color and raw depth before a run of translucent draws, at the working size: a working pixel the mean
// of its 2x2's colors and the sum of their depths (any of them drawn again changes the sum). Kept where the depth is
// still the last copy's, so each surface keeps its color from before any translucent draw over it.
constexpr const char* kCopySurfaces = R"(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D scene_color;
layout(set = 0, binding = 2) uniform sampler2D scene_depth;
layout(set = 0, binding = 3, rgba8) uniform image2D out_color;
layout(set = 0, binding = 4, r32f) uniform image2D out_depth;
void main() {
  ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
  ivec2 size = ivec2(viewport.xy);
  if (pix.x >= size.x || pix.y >= size.y) return;
  ivec2 full_size = params2.yz;
#if HALF_RES
  ivec2 q = pix * 2;
  ivec2 p0 = min(q, full_size - 1), p1 = min(q + ivec2(1, 0), full_size - 1);
  ivec2 p2 = min(q + ivec2(0, 1), full_size - 1), p3 = min(q + ivec2(1, 1), full_size - 1);
  vec3 color = (texelFetch(scene_color, p0, 0).rgb + texelFetch(scene_color, p1, 0).rgb +
                texelFetch(scene_color, p2, 0).rgb + texelFetch(scene_color, p3, 0).rgb) * 0.25;
  float depth = texelFetch(scene_depth, p0, 0).x + texelFetch(scene_depth, p1, 0).x +
                texelFetch(scene_depth, p2, 0).x + texelFetch(scene_depth, p3, 0).x;
#else
  vec3 color = texelFetch(scene_color, pix, 0).rgb;
  float depth = texelFetch(scene_depth, pix, 0).x;
#endif
  if (params3.y != 0.0 || abs(imageLoad(out_depth, pix).x - depth) > 1e-6) {
    imageStore(out_color, pix, vec4(color, 1.0));
    imageStore(out_depth, pix, vec4(depth, 0.0, 0.0, 0.0));
  }
}
)";

// Pass 3: XeGTAO_Denoise, two pixels a thread. The last pass also makes the multiplier the composite applies.
constexpr const char* kDenoise = R"(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 1) uniform sampler2D src_edges;  // the occlusion pass's: y the packed edges
layout(set = 0, binding = 2) uniform sampler2D src_ao;     // x the occlusion term
layout(set = 0, binding = 3, rgba8) uniform writeonly image2D out_ao;
#if FINAL_APPLY
layout(set = 0, binding = 4) uniform sampler2D scene_color;
layout(set = 0, binding = 5) uniform sampler2D view_depth;
layout(set = 0, binding = 6) uniform sampler2D surface_color;  // D188
layout(set = 0, binding = 7) uniform sampler2D surface_depth;  // D188
layout(set = 0, binding = 8) uniform sampler2D scene_depth;    // D188: the view's raw depth
#endif

vec4 UnpackEdges(float packed_value) {
  uint value = uint(packed_value * 255.5);
  vec4 edges_lrtb = vec4(float((value >> 6) & 3u), float((value >> 4) & 3u), float((value >> 2) & 3u),
                         float(value & 3u)) / 3.0;
  return clamp(edges_lrtb, 0.0, 1.0);
}

void AddSample(float ssao_value, float edge_value, inout float sum, inout float sum_weight) {
  sum += edge_value * ssao_value;
  sum_weight += edge_value;
}

void Output(ivec2 pix, float value) {
  ivec2 size = ivec2(viewport.xy);
  if (pix.x >= size.x || pix.y >= size.y) return;
#if FINAL_APPLY
  float visibility = clamp(value * XE_GTAO_OCCLUSION_TERM_SCALE, 0.0, 1.0);
  float multiplier = pow(visibility, params1.y);
#if HALF_RES
  ivec2 full_size = params2.yz;
  ivec2 q = pix * 2;
  vec3 color = (texelFetch(scene_color, min(q, full_size - 1), 0).rgb +
                texelFetch(scene_color, min(q + ivec2(1, 0), full_size - 1), 0).rgb +
                texelFetch(scene_color, min(q + ivec2(0, 1), full_size - 1), 0).rgb +
                texelFetch(scene_color, min(q + ivec2(1, 1), full_size - 1), 0).rgb) * 0.25;
#else
  vec3 color = texelFetch(scene_color, pix, 0).rgb;
#endif
  float luminance = dot(color, vec3(0.299, 0.587, 0.114));
  // D188: where no solid draw came after the last copy of this surface (the depth is still the copy's), the light the
  // translucent draws added since (light beams, haze) is spared: the occlusion takes only the surface's share.
  if (params3.x != 0.0) {
#if HALF_RES
    float depth_now = texelFetch(scene_depth, min(q, full_size - 1), 0).x +
                      texelFetch(scene_depth, min(q + ivec2(1, 0), full_size - 1), 0).x +
                      texelFetch(scene_depth, min(q + ivec2(0, 1), full_size - 1), 0).x +
                      texelFetch(scene_depth, min(q + ivec2(1, 1), full_size - 1), 0).x;
#else
    float depth_now = texelFetch(scene_depth, pix, 0).x;
#endif
    if (abs(depth_now - texelFetch(surface_depth, pix, 0).x) <= 1e-6) {
      float surface = dot(texelFetch(surface_color, pix, 0).rgb, vec3(0.299, 0.587, 0.114));
      float share = luminance > 1.0 / 255.0 ? clamp(surface / luminance, 0.0, 1.0) : 1.0;
      multiplier = 1.0 - share * (1.0 - multiplier);
    }
  }
  if (params1.z < 1.0) multiplier = mix(multiplier, 1.0, smoothstep(params1.z, 1.0, luminance));
  if (texelFetch(view_depth, pix, 0).x >= params1.w) multiplier = 1.0;
  imageStore(out_ao, pix, vec4(multiplier, 0.0, 0.0, 1.0));
#else
  imageStore(out_ao, pix, vec4(value, 0.0, 0.0, 1.0));
#endif
}

void main() {
  ivec2 pix_base = ivec2(gl_GlobalInvocationID.xy) * ivec2(2, 1);
  ivec2 size = ivec2(viewport.xy);
  if (pix_base.x >= size.x || pix_base.y >= size.y) return;
  float blur_amount = FINAL_APPLY != 0 ? params0.w : params0.w / 5.0;
  const float diag_weight = 0.85 * 0.5;

  vec2 gather_center = vec2(pix_base) * viewport.zw;
  vec4 edges_q0 = textureGatherOffset(src_edges, gather_center, ivec2(0, 0), 1);
  vec4 edges_q1 = textureGatherOffset(src_edges, gather_center, ivec2(2, 0), 1);
  vec4 edges_q2 = textureGatherOffset(src_edges, gather_center, ivec2(1, 2), 1);
  vec4 vis_q0 = textureGatherOffset(src_ao, gather_center, ivec2(0, 0), 0);
  vec4 vis_q1 = textureGatherOffset(src_ao, gather_center, ivec2(2, 0), 0);
  vec4 vis_q2 = textureGatherOffset(src_ao, gather_center, ivec2(0, 2), 0);
  vec4 vis_q3 = textureGatherOffset(src_ao, gather_center, ivec2(2, 2), 0);

  for (int side = 0; side < 2; side++) {
    ivec2 pix = ivec2(pix_base.x + side, pix_base.y);
    vec4 edges_l_lrtb = UnpackEdges(side == 0 ? edges_q0.x : edges_q0.y);
    vec4 edges_t_lrtb = UnpackEdges(side == 0 ? edges_q0.z : edges_q1.w);
    vec4 edges_r_lrtb = UnpackEdges(side == 0 ? edges_q1.x : edges_q1.y);
    vec4 edges_b_lrtb = UnpackEdges(side == 0 ? edges_q2.w : edges_q2.z);
    vec4 edges_c_lrtb = UnpackEdges(side == 0 ? edges_q0.y : edges_q1.x);
    edges_c_lrtb *= vec4(edges_l_lrtb.y, edges_r_lrtb.x, edges_t_lrtb.w, edges_b_lrtb.z);

    const float leak_threshold = 2.5;
    const float leak_strength = 0.5;
    float edginess = (clamp(4.0 - leak_threshold - dot(edges_c_lrtb, vec4(1.0)), 0.0, 1.0) / (4.0 - leak_threshold)) *
                     leak_strength;
    edges_c_lrtb = clamp(edges_c_lrtb + edginess, 0.0, 1.0);

    float weight_tl = diag_weight * (edges_c_lrtb.x * edges_l_lrtb.z + edges_c_lrtb.z * edges_t_lrtb.x);
    float weight_tr = diag_weight * (edges_c_lrtb.z * edges_t_lrtb.y + edges_c_lrtb.y * edges_r_lrtb.z);
    float weight_bl = diag_weight * (edges_c_lrtb.w * edges_b_lrtb.x + edges_c_lrtb.x * edges_l_lrtb.w);
    float weight_br = diag_weight * (edges_c_lrtb.y * edges_r_lrtb.w + edges_c_lrtb.w * edges_b_lrtb.y);

    float ssao_value = side == 0 ? vis_q0.y : vis_q1.x;
    float ssao_value_l = side == 0 ? vis_q0.x : vis_q0.y;
    float ssao_value_t = side == 0 ? vis_q0.z : vis_q1.w;
    float ssao_value_r = side == 0 ? vis_q1.x : vis_q1.y;
    float ssao_value_b = side == 0 ? vis_q2.z : vis_q3.w;
    float ssao_value_tl = side == 0 ? vis_q0.w : vis_q0.z;
    float ssao_value_br = side == 0 ? vis_q3.w : vis_q3.z;
    float ssao_value_tr = side == 0 ? vis_q1.w : vis_q1.z;
    float ssao_value_bl = side == 0 ? vis_q2.w : vis_q2.z;

    float sum_weight = blur_amount;
    float sum = ssao_value * sum_weight;
    AddSample(ssao_value_l, edges_c_lrtb.x, sum, sum_weight);
    AddSample(ssao_value_r, edges_c_lrtb.y, sum, sum_weight);
    AddSample(ssao_value_t, edges_c_lrtb.z, sum, sum_weight);
    AddSample(ssao_value_b, edges_c_lrtb.w, sum, sum_weight);
    AddSample(ssao_value_tl, weight_tl, sum, sum_weight);
    AddSample(ssao_value_tr, weight_tr, sum, sum_weight);
    AddSample(ssao_value_bl, weight_bl, sum, sum_weight);
    AddSample(ssao_value_br, weight_br, sum, sum_weight);
    Output(pix, sum / sum_weight);
  }
}
)";

// Pass 4: the scene's color times the multiplier (a multiplying blend), or the multiplier alone (debug).
constexpr const char* kCompositeVS = R"(
#version 450
layout(location = 0) out vec2 v_texcoord;
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_texcoord = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
constexpr const char* kCompositePS = R"(
#version 450
layout(set = 0, binding = 0) uniform sampler2D ao_result;
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 out_color;
void main() {
  float multiplier = textureLod(ao_result, v_texcoord, 0.0).x;
  out_color = vec4(multiplier, multiplier, multiplier, 1.0);
}
)";
// At half resolution: the multiplier enlarged onto the view by depth (the four nearest working pixels, bilinear, each
// weighted by how close its depth is to the view pixel's; where none is, the closest in depth of the 4x4 around).
constexpr const char* kCompositeHalfPS = R"(
layout(set = 0, binding = 1) uniform sampler2D ao_result;   // the multiplier, at the working size
layout(set = 0, binding = 2) uniform sampler2D work_depth;  // the working pixels' view-space depth (level 0)
layout(set = 0, binding = 3) uniform sampler2D scene_depth; // the view's depth
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 out_color;
void main() {
  ivec2 full_size = params2.yz;
  ivec2 p = ivec2(gl_FragCoord.xy);
  float d = texelFetch(scene_depth, min(p, full_size - 1), 0).x;
  d = (d - depth_unpack.z) * depth_unpack.w;
  float z = clamp(depth_unpack.x / (depth_unpack.y - d), 0.0, 3.402823466e+38);
  float multiplier = 1.0;
  if (z < params1.w) {
    ivec2 size = ivec2(viewport.xy);
    vec2 pos = (vec2(p) + 0.5) * 0.5 - 0.5;
    ivec2 base = ivec2(floor(pos));
    vec2 f = pos - vec2(base);
    float inv_z = 1.0 / max(z, 1e-6);
    float sum = 0.0, weight_sum = 0.0;
    for (int i = 0; i < 4; i++) {
      ivec2 o = ivec2(i & 1, i >> 1);
      ivec2 q = clamp(base + o, ivec2(0), size - 1);
      float dz = abs(texelFetch(work_depth, q, 0).x - z) * inv_z;
      float w = ((o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) + 0.01) * exp(-dz * 40.0);
      sum += w * texelFetch(ao_result, q, 0).x;
      weight_sum += w;
    }
    if (weight_sum > 0.02) {
      multiplier = sum / weight_sum;
    } else {
      float best = 3.402823466e+38;
      for (int y = -1; y <= 2; y++) {
        for (int x = -1; x <= 2; x++) {
          ivec2 q = clamp(base + ivec2(x, y), ivec2(0), size - 1);
          float dz = abs(texelFetch(work_depth, q, 0).x - z);
          if (dz < best) {
            best = dz;
            multiplier = texelFetch(ao_result, q, 0).x;
          }
        }
      }
    }
  }
  out_color = vec4(multiplier, multiplier, multiplier, 1.0);
}
)";

RefCntAutoPtr<IShader> CreateShader(IRenderDevice* device, SHADER_TYPE type, const std::string& source,
                                    const char* name) {
  RefCntAutoPtr<IShader> shader;
  GlslStage stage = type == SHADER_TYPE_COMPUTE  ? GlslStage::kCompute
                    : type == SHADER_TYPE_VERTEX ? GlslStage::kVertex
                                                 : GlslStage::kPixel;
  std::vector<uint32_t> spirv = CompileGlsl(stage, source, name);
  if (spirv.empty()) return shader;
  ShaderCreateInfo info;
  info.Desc.Name = name;
  info.Desc.ShaderType = type;
  info.Desc.UseCombinedTextureSamplers = true;
  info.ByteCode = spirv.data();
  info.ByteCodeSize = spirv.size() * sizeof(uint32_t);
  device->CreateShader(info, &shader);
  return shader;
}

SamplerDesc PointClamp() {
  SamplerDesc point;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  return point;
}

void SetVariable(IShaderResourceBinding* srb, SHADER_TYPE stage, const char* name, IDeviceObject* object) {
  if (auto* variable = srb->GetVariableByName(stage, name)) variable->Set(object);
}

}  // namespace

AmbientOcclusion::Quality AmbientOcclusion::ParseQuality(std::string_view name) {
  if (name == "off" || name.empty()) return Quality::kOff;
  if (name == "low") return Quality::kLow;
  if (name == "medium") return Quality::kMedium;
  if (name == "high") return Quality::kHigh;
  if (name == "ultra") return Quality::kUltra;
  REXLOG_WARN("[native_x_renderer] ambient occlusion \"{}\" unknown: off", name);
  return Quality::kOff;
}

const char* AmbientOcclusion::QualityName(Quality quality) {
  switch (quality) {
    case Quality::kLow:
      return "low";
    case Quality::kMedium:
      return "medium";
    case Quality::kHigh:
      return "high";
    case Quality::kUltra:
      return "ultra";
    default:
      return "off";
  }
}

bool AmbientOcclusion::CameraFromMatrix(const std::array<double, 16>& m, Camera& camera) {
  const double* x = &m[0];
  const double* y = &m[4];
  const double* z = &m[8];
  const double* w = &m[12];
  auto dot3 = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  double ww = dot3(w, w), xx = dot3(x, x), yy = dot3(y, y), zz = dot3(z, z);
  if (!(ww > 1e-12) || !(xx > 1e-12) || !(yy > 1e-12)) return false;
  // A perspective: clip z = a * clip w + b, the z row along the w row.
  double a = dot3(z, w) / ww;
  double b = z[3] - a * w[3];
  double r[3] = {z[0] - a * w[0], z[1] - a * w[1], z[2] - a * w[2]};
  if (!std::isfinite(a) || !std::isfinite(b) || std::abs(b) < 1e-12 || dot3(r, r) > 1e-6 * std::max(zz, 1e-12)) {
    return false;
  }
  // The view's depth (clip w) at a buffer depth d: b / (d - a). At d = 0 and d = 1, the planes.
  double at0 = std::abs(a) < 1e-12 ? std::numeric_limits<double>::infinity() : -b / a;
  double at1 = std::abs(1.0 - a) < 1e-12 ? std::numeric_limits<double>::infinity() : b / (1.0 - a);
  bool reversed = at0 > at1;
  double near_plane = reversed ? at1 : at0, far_plane = reversed ? at0 : at1;
  if (!(near_plane > 0.0) || !(far_plane > near_plane)) return false;
  // A camera a game draws a scene with: planes not crowded together near the eye, a field of view and an aspect ratio a
  // screen shows. GH Warriors of Rock's menus offered a matrix that passed the vote's test with planes under 0.05 apart
  // at 90 x 90 degrees, and its occlusion would have been made with it.
  double tan_x = std::sqrt(ww / xx), tan_y = std::sqrt(ww / yy);
  if (near_plane < 1e-4 || far_plane < 1.0 || far_plane < near_plane * 4.0 || tan_y < 0.04 || tan_y > 11.4 ||
      tan_x / tan_y < 0.25 || tan_x / tan_y > 8.0) {
    return false;
  }
  camera.linearize_mul = float(-b);
  camera.linearize_add = float(a);
  camera.tan_half_fov_x = float(tan_x);
  camera.tan_half_fov_y = float(tan_y);
  camera.near_plane = float(near_plane);
  camera.far_plane = std::isfinite(far_plane) ? float(far_plane) : std::numeric_limits<float>::max();
  camera.reversed = reversed;
  return true;
}

bool AmbientOcclusion::CreateComputePass(Pass& pass, const char* name, const std::string& source,
                                         std::initializer_list<const char*> textures,
                                         std::initializer_list<const char*> images) {
  RefCntAutoPtr<IShader> shader = CreateShader(device_, SHADER_TYPE_COMPUTE, source, name);
  if (!shader) return false;
  ComputePipelineStateCreateInfo info;
  info.PSODesc.Name = name;
  info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
  info.pCS = shader;
  std::vector<ShaderResourceVariableDesc> variables;
  std::vector<ImmutableSamplerDesc> samplers;
  for (const char* texture : textures) {
    variables.push_back({SHADER_TYPE_COMPUTE, texture, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
    samplers.push_back({SHADER_TYPE_COMPUTE, texture, PointClamp()});
  }
  for (const char* image : images) {
    variables.push_back({SHADER_TYPE_COMPUTE, image, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
  }
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables.data();
  info.PSODesc.ResourceLayout.NumVariables = uint32_t(variables.size());
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers.data();
  info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(samplers.size());
  device_->CreateComputePipelineState(info, &pass.pso);
  if (!pass.pso) {
    REXLOG_ERROR("[native_x_renderer] {}: no pipeline", name);
    return false;
  }
  if (auto* variable = pass.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "GtaoConstants")) {
    variable->Set(constants_);
  }
  pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  return pass.srb != nullptr;
}

bool AmbientOcclusion::Initialize(IRenderDevice* device, const Settings& settings) {
  device_ = device;
  settings_ = settings;
  active_ = false;
  if (settings_.quality == Quality::kOff) return true;
  if (!(settings_.radius > 0.0f)) {
    REXLOG_WARN("[native_x_renderer] ambient occlusion: no radius (the game's profile gives none, and "
                "native_x_renderer_ao_radius is 0): off");
    return false;
  }
  BufferDesc constants;
  constants.Name = "NativeXRenderer ambient occlusion constants";
  constants.Size = sizeof(Constants);
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device_->CreateBuffer(constants, nullptr, &constants_);
  // XeGTAO's quality levels: slices and steps per slice.
  int slices = 3, steps = 3;
  switch (settings_.quality) {
    case Quality::kLow:
      slices = 1, steps = 2;
      break;
    case Quality::kMedium:
      slices = 2, steps = 2;
      break;
    case Quality::kUltra:
      slices = 9, steps = 3;
      break;
    default:
      break;
  }
  std::string main = std::string(kCommon) + "#define SLICE_COUNT " + std::to_string(slices) +
                     "\n#define STEPS_PER_SLICE " + std::to_string(steps) + "\n" + kMain;
  const std::string common = std::string(kCommon) + (settings_.half ? "#define HALF_RES 1\n" : "#define HALF_RES 0\n");
  bool ok = constants_ &&
            CreateComputePass(prefilter_, "NativeXRenderer ambient occlusion depth", common + kPrefilter,
                              {"src_depth"}, {"out_depth0", "out_depth1", "out_depth2", "out_depth3", "out_depth4"}) &&
            CreateComputePass(main_, "NativeXRenderer ambient occlusion", main, {"src_view_depth"}, {"out_term"}) &&
            CreateComputePass(denoise_, "NativeXRenderer ambient occlusion denoise",
                              common + "#define FINAL_APPLY 0\n" + kDenoise, {"src_edges", "src_ao"}, {"out_ao"}) &&
            CreateComputePass(denoise_final_, "NativeXRenderer ambient occlusion denoise (last)",
                              common + "#define FINAL_APPLY 1\n" + kDenoise,
                              {"src_edges", "src_ao", "scene_color", "view_depth", "surface_color", "surface_depth",
                               "scene_depth"},
                              {"out_ao"}) &&
            CreateComputePass(surfaces_, "NativeXRenderer ambient occlusion surfaces", common + kCopySurfaces,
                              {"scene_color", "scene_depth"}, {"out_color", "out_depth"});
  if (!ok) {
    REXLOG_ERROR("[native_x_renderer] ambient occlusion could not be set up: off");
    return false;
  }
  active_ = true;
  REXLOG_INFO("[native_x_renderer] ambient occlusion (D178): XeGTAO {} ({} slices of {} steps){}, radius {:.3g} world "
              "units, strength {:.2f}, fading out from luminance {:.2f}; denoise {}{}{}",
              QualityName(settings_.quality), slices, steps,
              settings_.half ? " at half the view's width and height, enlarged by depth (D184)" : "", settings_.radius,
              settings_.strength, settings_.bright,
              fmt::format("{}, blur {:.2f}{}",
                          settings_.denoise_passes > 0 ? fmt::format("{} passes (D187)", settings_.denoise_passes)
                                                       : std::string("1 pass with a temporal upscaler, 2 without"),
                          settings_.denoise_blur, settings_.noise_moves ? ", the noise changed every frame" : ""),
              settings_.debug ? "; debug: the multiplier in place of the image" : "",
              settings_.spare_translucent ? "; the light of translucent draws spared (D188)" : "");
  return true;
}

std::shared_ptr<const AmbientOcclusion::Targets> AmbientOcclusion::TargetsFor(uint32_t width, uint32_t height) {
  if (targets_ && targets_->width == width && targets_->height == height) return targets_;
  auto targets = std::make_shared<Targets>();
  targets->width = width;
  targets->height = height;
  targets->work_width = settings_.half ? (width + 1) / 2 : width;
  targets->work_height = settings_.half ? (height + 1) / 2 : height;
  auto create = [&](RefCntAutoPtr<ITexture>& texture, const char* name, TEXTURE_FORMAT format, uint32_t levels) {
    TextureDesc desc;
    desc.Name = name;
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = targets->work_width;
    desc.Height = targets->work_height;
    desc.MipLevels = levels;
    desc.Format = format;
    desc.Usage = USAGE_DEFAULT;
    desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    device_->CreateTexture(desc, nullptr, &texture);
    return texture != nullptr;
  };
  bool ok = create(targets->depth, "NativeXRenderer ambient occlusion view depth", TEX_FORMAT_R32_FLOAT,
                   kDepthLevels) &&
            create(targets->term, "NativeXRenderer ambient occlusion term", TEX_FORMAT_RGBA8_UNORM, 1) &&
            create(targets->blur, "NativeXRenderer ambient occlusion blur", TEX_FORMAT_RGBA8_UNORM, 1) &&
            create(targets->result, "NativeXRenderer ambient occlusion result", TEX_FORMAT_RGBA8_UNORM, 1) &&
            create(targets->surface_color, "NativeXRenderer ambient occlusion surfaces' color", TEX_FORMAT_RGBA8_UNORM,
                   1) &&
            create(targets->surface_depth, "NativeXRenderer ambient occlusion surfaces' depth", TEX_FORMAT_R32_FLOAT,
                   1);
  if (ok && settings_.denoise_passes > 2) {
    ok = create(targets->blur2, "NativeXRenderer ambient occlusion blur 2", TEX_FORMAT_RGBA8_UNORM, 1);
  }
  for (int level = 0; ok && level < kDepthLevels; ++level) {
    TextureViewDesc view;
    view.ViewType = TEXTURE_VIEW_UNORDERED_ACCESS;
    view.MostDetailedMip = uint32_t(level);
    view.NumMipLevels = 1;
    targets->depth->CreateView(view, &targets->depth_levels[size_t(level)]);
    ok = targets->depth_levels[size_t(level)] != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("[native_x_renderer] ambient occlusion: no working textures for {}x{}", width, height);
    targets_.reset();
    return nullptr;
  }
  targets_ = targets;
  return targets_;
}

AmbientOcclusion::Pass* AmbientOcclusion::CompositePass(TEXTURE_FORMAT format) {
  auto it = composite_.find(format);
  if (it != composite_.end()) return it->second.pso ? &it->second : nullptr;
  Pass& pass = composite_[format];
  RefCntAutoPtr<IShader> vs =
      CreateShader(device_, SHADER_TYPE_VERTEX, kCompositeVS, "NativeXRenderer ambient occlusion VS");
  RefCntAutoPtr<IShader> ps =
      settings_.half ? CreateShader(device_, SHADER_TYPE_PIXEL, std::string(kCommon) + kCompositeHalfPS,
                                    "NativeXRenderer ambient occlusion PS (half resolution)")
                     : CreateShader(device_, SHADER_TYPE_PIXEL, kCompositePS, "NativeXRenderer ambient occlusion PS");
  if (!vs || !ps) return nullptr;
  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = "NativeXRenderer ambient occlusion composite";
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = format;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  if (!settings_.debug) {
    // color = color x multiplier; alpha kept.
    RenderTargetBlendDesc& blend = info.GraphicsPipeline.BlendDesc.RenderTargets[0];
    blend.BlendEnable = True;
    blend.SrcBlend = BLEND_FACTOR_ZERO;
    blend.DestBlend = BLEND_FACTOR_SRC_COLOR;
    blend.BlendOp = BLEND_OPERATION_ADD;
    blend.SrcBlendAlpha = BLEND_FACTOR_ZERO;
    blend.DestBlendAlpha = BLEND_FACTOR_ONE;
    blend.BlendOpAlpha = BLEND_OPERATION_ADD;
  }
  info.pVS = vs;
  info.pPS = ps;
  ShaderResourceVariableDesc variables[] = {{SHADER_TYPE_PIXEL, "ao_result", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
                                            {SHADER_TYPE_PIXEL, "work_depth", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
                                            {SHADER_TYPE_PIXEL, "scene_depth", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "ao_result", PointClamp()},
                                     {SHADER_TYPE_PIXEL, "work_depth", PointClamp()},
                                     {SHADER_TYPE_PIXEL, "scene_depth", PointClamp()}};
  const uint32_t count = settings_.half ? 3u : 1u;
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables;
  info.PSODesc.ResourceLayout.NumVariables = count;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = count;
  device_->CreateGraphicsPipelineState(info, &pass.pso);
  if (!pass.pso) {
    REXLOG_ERROR("[native_x_renderer] ambient occlusion composite: no pipeline for format {}", int(format));
    return nullptr;
  }
  if (auto* variable = pass.pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "GtaoConstants")) {
    variable->Set(constants_);
  }
  pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  return pass.srb ? &pass : nullptr;
}

void AmbientOcclusion::Dispatch(IDeviceContext* context, Pass& pass, uint32_t groups_x, uint32_t groups_y) {
  context->SetPipelineState(pass.pso);
  context->CommitShaderResources(pass.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DispatchComputeAttribs dispatch(groups_x, groups_y, 1);
  context->DispatchCompute(dispatch);
}

void AmbientOcclusion::Apply(IDeviceContext* context, const Targets& targets, ITextureView* color,
                             ITextureView* color_srv, ITextureView* depth, const Camera& camera, float z_min,
                             float z_max, uint32_t noise_index, bool temporal, bool surfaces) {
  if (!active_ || !color || !color_srv || !depth || !targets.depth) return;
  Pass* composite = CompositePass(color->GetDesc().Format);
  if (!composite) return;
  // The working size: the view's, or half of it (Settings::half).
  const uint32_t width = targets.work_width, height = targets.work_height;

  Constants constants{};
  constants.viewport[0] = float(width);
  constants.viewport[1] = float(height);
  constants.viewport[2] = 1.0f / float(width);
  constants.viewport[3] = 1.0f / float(height);
  constants.depth_unpack[0] = camera.linearize_mul;
  constants.depth_unpack[1] = camera.linearize_add;
  constants.depth_unpack[2] = z_min;
  constants.depth_unpack[3] = z_max != z_min ? 1.0f / (z_max - z_min) : 1.0f;
  constants.ndc_to_view[0] = camera.tan_half_fov_x * 2.0f;
  constants.ndc_to_view[1] = camera.tan_half_fov_y * -2.0f;
  constants.ndc_to_view[2] = -camera.tan_half_fov_x;
  constants.ndc_to_view[3] = camera.tan_half_fov_y;
  constants.params0[0] = settings_.radius;
  constants.params0[2] = 2.2f;  // XeGTAO's final value power
  constants.params0[3] = settings_.denoise_blur;  // its denoise blur beta (1.2 its default; D187)
  constants.params1[0] = 3.3f;  // its depth level offset
  constants.params1[1] = std::max(settings_.strength, 0.0f) / 2.2f;
  constants.params1[2] = settings_.bright;
  constants.params1[3] = camera.far_plane * 0.999f;
  constants.params2[0] = int32_t((temporal || settings_.noise_moves) ? noise_index % 64 : 0);
  constants.params2[1] = int32_t(targets.width);
  constants.params2[2] = int32_t(targets.height);
  constants.params3[0] = surfaces ? 1.0f : 0.0f;
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &constants, sizeof(constants));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  // The view's targets are bound: unbound, so that its depth and color can be read.
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);

  ITextureView* view_depth = targets.depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* term = targets.term->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  SetVariable(prefilter_.srb, SHADER_TYPE_COMPUTE, "src_depth", depth);
  for (int level = 0; level < kDepthLevels; ++level) {
    std::string name = "out_depth" + std::to_string(level);
    SetVariable(prefilter_.srb, SHADER_TYPE_COMPUTE, name.c_str(), targets.depth_levels[size_t(level)]);
  }
  Dispatch(context, prefilter_, (width + 15) / 16, (height + 15) / 16);

  SetVariable(main_.srb, SHADER_TYPE_COMPUTE, "src_view_depth", view_depth);
  SetVariable(main_.srb, SHADER_TYPE_COMPUTE, "out_term",
              targets.term->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
  Dispatch(context, main_, (width + 7) / 8, (height + 7) / 8);

  // The denoise's passes before the last: without a temporal upscaler to accumulate the noise away, one (XeGTAO's
  // "medium"); with one, none; Settings::denoise_passes chooses (D187), alternating between blur and blur2. Each pass
  // reads the edges from the term and the occlusion from the pass before.
  ITextureView* ao = term;
  const int passes = settings_.denoise_passes > 0 ? settings_.denoise_passes : (temporal ? 1 : 2);
  for (int pass = 0; pass + 1 < passes; ++pass) {
    ITexture* out = (pass % 2 == 1 && targets.blur2) ? targets.blur2.RawPtr() : targets.blur.RawPtr();
    if (out->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) == ao) break;  // no second texture: stop rather than alias
    SetVariable(denoise_.srb, SHADER_TYPE_COMPUTE, "src_edges", term);
    SetVariable(denoise_.srb, SHADER_TYPE_COMPUTE, "src_ao", ao);
    SetVariable(denoise_.srb, SHADER_TYPE_COMPUTE, "out_ao", out->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
    Dispatch(context, denoise_, (width + 15) / 16, (height + 7) / 8);
    ao = out->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  }
  SetVariable(denoise_final_.srb, SHADER_TYPE_COMPUTE, "src_edges", term);
  SetVariable(denoise_final_.srb, SHADER_TYPE_COMPUTE, "src_ao", ao);
  SetVariable(denoise_final_.srb, SHADER_TYPE_COMPUTE, "scene_color", color_srv);
  SetVariable(denoise_final_.srb, SHADER_TYPE_COMPUTE, "view_depth", view_depth);
  SetVariable(denoise_final_.srb, SHADER_TYPE_COMPUTE, "surface_color",
              targets.surface_color->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  SetVariable(denoise_final_.srb, SHADER_TYPE_COMPUTE, "surface_depth",
              targets.surface_depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  SetVariable(denoise_final_.srb, SHADER_TYPE_COMPUTE, "scene_depth", depth);
  SetVariable(denoise_final_.srb, SHADER_TYPE_COMPUTE, "out_ao",
              targets.result->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
  Dispatch(context, denoise_final_, (width + 15) / 16, (height + 7) / 8);

  context->SetRenderTargets(1, &color, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  const TextureDesc& desc = color->GetTexture()->GetDesc();
  Viewport viewport;
  viewport.Width = float(desc.Width);
  viewport.Height = float(desc.Height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  SetVariable(composite->srb, SHADER_TYPE_PIXEL, "ao_result",
              targets.result->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  if (settings_.half) {
    SetVariable(composite->srb, SHADER_TYPE_PIXEL, "work_depth", view_depth);
    SetVariable(composite->srb, SHADER_TYPE_PIXEL, "scene_depth", depth);
  }
  context->SetPipelineState(composite->pso);
  context->CommitShaderResources(composite->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  applied_.fetch_add(1, std::memory_order_relaxed);
}

void AmbientOcclusion::CopySurfaces(IDeviceContext* context, const Targets& targets, ITextureView* color_srv,
                                    ITextureView* depth, bool first) {
  if (!active_ || !color_srv || !depth || !targets.surface_color || !targets.surface_depth) return;
  Constants constants{};
  constants.viewport[0] = float(targets.work_width);
  constants.viewport[1] = float(targets.work_height);
  constants.params2[1] = int32_t(targets.width);
  constants.params2[2] = int32_t(targets.height);
  constants.params3[1] = first ? 1.0f : 0.0f;
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &constants, sizeof(constants));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  // The view's targets are bound: unbound, so that they can be read (the draw path binds them again).
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  SetVariable(surfaces_.srb, SHADER_TYPE_COMPUTE, "scene_color", color_srv);
  SetVariable(surfaces_.srb, SHADER_TYPE_COMPUTE, "scene_depth", depth);
  SetVariable(surfaces_.srb, SHADER_TYPE_COMPUTE, "out_color",
              targets.surface_color->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
  SetVariable(surfaces_.srb, SHADER_TYPE_COMPUTE, "out_depth",
              targets.surface_depth->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
  Dispatch(context, surfaces_, (targets.work_width + 7) / 8, (targets.work_height + 7) / 8);
}

}  // namespace native_x_renderer
