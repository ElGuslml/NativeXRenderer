// NativeXRenderer - ambient occlusion of its own on the main 3D view (internal, D178).
//
// XeGTAO (Intel, MIT: thirdparty notice in THIRD_PARTY_NOTICES.md; GTAO, Jimenez et al. 2016), ported to GLSL compute
// shaders, at the main 3D view's end (its upscale point, D119; the view's end with a game's view hooks), before DLSS,
// FSR 3.1 or the spatial filter of D174 and before the game's post-processing reads the scene:
//   1. the view's depth made view-space depth, with four smaller levels (one pass);
//   2. the occlusion, its normals made from that depth, with 1-9 slices of 2-3 steps by the quality (one pass);
//   3. an edge-aware denoise (one pass with a temporal upscaler, which accumulates the noise away; two without);
//   4. the scene's color multiplied by the result (a draw with a multiplying blend).
// The multiplier is the occlusion to the power strength / 2.2: the scene's color is gamma-encoded, as the game wrote
// it, and the occlusion is a fraction of linear light. It fades out on bright pixels (lamps, screens, fire: light the
// game drew there is not ambient light) and is 1 on the background (depth at the far plane).
//
// The camera comes from a world-view-projection matrix (the first of the view's draws that has one, D124): its planes
// from how its z row follows its w row, its fields of view from the lengths of its x and y rows against w's. A world's
// uniform scale, rotation and position leave those unchanged, so any draw of the view gives the camera's.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

namespace native_x_renderer {

class AmbientOcclusion {
 public:
  enum class Quality : uint8_t { kOff, kLow, kMedium, kHigh, kUltra };
  // "off", "low", "medium", "high" or "ultra"; anything else is off (logged).
  static Quality ParseQuality(std::string_view name);
  static const char* QualityName(Quality quality);

  // The view's camera: view-space depth = linearize_mul / (linearize_add - depth), the depth in 0-1 of the view's
  // viewport depth range; the tangents of half the fields of view.
  struct Camera {
    float linearize_mul = 0.0f, linearize_add = 0.0f;
    float tan_half_fov_x = 0.0f, tan_half_fov_y = 0.0f;
    float near_plane = 0.0f, far_plane = 0.0f;
    bool reversed = false;  // depth 1 at the near plane
  };
  // From a perspective world-view-projection in column form (clip = m x position, m[row * 4 + column]). False when
  // the matrix is not one.
  static bool CameraFromMatrix(const std::array<double, 16>& m, Camera& camera);

  struct Settings {
    Quality quality = Quality::kOff;
    float radius = 0.0f;    // the occlusion's reach, in the game's world units
    float strength = 1.0f;  // 1: the occlusion as computed; 2: doubled (an exponent on the multiplier)
    float bright = 0.85f;   // the luminance from which the occlusion fades out, to none at 1 (1 or more: never)
    bool debug = false;     // the multiplier itself in place of the image, to tune the radius
    bool half = false;      // computed at half the view's width and height, enlarged by depth (D184)
  };
  bool Initialize(Diligent::IRenderDevice* device, const Settings& settings);
  bool active() const { return active_; }
  const Settings& settings() const { return settings_; }

  // The working textures of one size. Made on the renderer's thread; an operation that uses them holds them, so that
  // a new size does not free them while it is queued.
  struct Targets {
    uint32_t width = 0, height = 0;            // the view's
    uint32_t work_width = 0, work_height = 0;  // the textures' below: the view's, or half of it (Settings::half)
    Diligent::RefCntAutoPtr<Diligent::ITexture> depth;  // view-space depth, R32F, 5 levels
    std::array<Diligent::RefCntAutoPtr<Diligent::ITextureView>, 5> depth_levels;  // unordered access, one level each
    Diligent::RefCntAutoPtr<Diligent::ITexture> term;    // RGBA8: x the occlusion term, y the packed edges
    Diligent::RefCntAutoPtr<Diligent::ITexture> blur;    // RGBA8: the first denoise pass's (two passes)
    Diligent::RefCntAutoPtr<Diligent::ITexture> result;  // RGBA8: x the multiplier
  };
  std::shared_ptr<const Targets> TargetsFor(uint32_t width, uint32_t height);

  // Inside an operation of the record queue. The view's depth (`depth`, a shader resource view of the targets' size)
  // gives the occlusion, multiplied into the view's color: `color` its render target view, `color_srv` its shader
  // resource view. `z_min` / `z_max`: the view's viewport depth range. `noise_index`: 0, or the frame's index with a
  // temporal upscaler (its noise then changes every frame and is accumulated away).
  void Apply(Diligent::IDeviceContext* context, const Targets& targets, Diligent::ITextureView* color,
             Diligent::ITextureView* color_srv, Diligent::ITextureView* depth, const Camera& camera, float z_min,
             float z_max, uint32_t noise_index, bool temporal);

  uint64_t applied() const { return applied_.load(std::memory_order_relaxed); }

 private:
  struct Pass {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  bool CreateComputePass(Pass& pass, const char* name, const std::string& source,
                         std::initializer_list<const char*> textures, std::initializer_list<const char*> images);
  Pass* CompositePass(Diligent::TEXTURE_FORMAT format);
  void Dispatch(Diligent::IDeviceContext* context, Pass& pass, uint32_t groups_x, uint32_t groups_y);

  Diligent::IRenderDevice* device_ = nullptr;
  Settings settings_;
  bool active_ = false;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Pass prefilter_, main_, denoise_, denoise_final_;
  std::map<Diligent::TEXTURE_FORMAT, Pass> composite_;  // by the color's format; only the record stage uses it
  std::shared_ptr<const Targets> targets_;
  std::atomic<uint64_t> applied_{0};
};

}  // namespace native_x_renderer
