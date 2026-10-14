// XenosNative - what a draw's texture fetch constants bind (internal).
//
// Phase 2, first part: a texture a resolve wrote (targets.h) is bound by its address; any other texture
// gets a placeholder (opaque white, so geometry and lighting are visible). Loading guest textures from
// memory, through the SDK's texture loaders, is plan step 7.
//
// The translator samples 1D and 2D textures as 2D arrays, stacked and 3D textures as 3D, cubes as cubes,
// and applies the texture's swizzle itself (image_view_format_swizzle is off): views are RGBA identity.

#pragma once

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>

#include <cstdint>
#include <unordered_map>

#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Sampler.h"
#include "Texture.h"
#include "TextureView.h"

namespace xenos_native {

class ResolvedTextures;

class Textures {
 public:
  bool Initialize(Diligent::IRenderDevice* device, ResolvedTextures& resolved);

  // The view a texture binding samples, never null.
  Diligent::ITextureView* View(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                               rex::graphics::xenos::FetchOpDimension dimension);
  // The sampler for a sampler binding of the fetch constant, never null.
  Diligent::ISampler* Sampler(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                              const rex::graphics::SpirvShader::SamplerBinding& binding);
  // The swizzle the shader applies (system constant texture_swizzles): the guest's, on RGBA views.
  static uint32_t HostSwizzle(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);

  struct Stats {
    uint32_t resolved = 0, placeholders = 0;
  };
  Stats TakeStats() {
    Stats stats = stats_;
    stats_ = {};
    return stats;
  }
  size_t sampler_count() const { return samplers_.size(); }

 private:
  Diligent::IRenderDevice* device_ = nullptr;
  ResolvedTextures* resolved_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::ITexture> placeholder_2d_, placeholder_3d_, placeholder_cube_;
  Diligent::ITextureView* placeholder_views_[3] = {};  // 2D array, 3D, cube
  std::unordered_map<uint64_t, Diligent::RefCntAutoPtr<Diligent::ISampler>> samplers_;
  Stats stats_;
};

}  // namespace xenos_native
