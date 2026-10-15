// XenosNative - what a draw's texture fetch constants bind (internal).
//
// For each texture binding of the translated shaders, in this order:
//   - a texture a resolve wrote (targets.h), found by its address;
//   - the guest texture loaded from memory (guest_textures.h);
//   - a placeholder (opaque white), when neither exists (an unsupported format, a dimension mismatch).
// The translator samples 1D and 2D textures as 2D arrays, stacked and 3D textures as 3D, cubes as cubes,
// and applies the texture's swizzle itself (image_view_format_swizzle is off): views are RGBA identity,
// and the swizzles come from the texture cache (system constants, draw.cpp).

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

class GuestTextures;
class ResolvedTextures;

class Textures {
 public:
  bool Initialize(Diligent::IRenderDevice* device, ResolvedTextures& resolved, GuestTextures& guest);

  // The view a texture binding samples, never null. The guest textures must have been requested for the
  // draw (GuestTextures::RequestTextures).
  Diligent::ITextureView* View(uint32_t fetch_constant, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                               rex::graphics::xenos::FetchOpDimension dimension, bool is_signed);
  // The swizzle and the signs the shader applies (system constants texture_swizzles and
  // texture_swizzled_signs), for the view View binds: the texture cache's, or for a resolved texture the
  // guest swizzle on its host format, whose missing components repeat the last one as on the guest (one
  // component: RRRR, two: RGGG).
  void HostSwizzle(uint32_t fetch_constant, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                   uint32_t& swizzle, uint8_t& signs) const;
  // Whether View binds a resolve result for this fetch constant: such textures are at the render scale,
  // which the translated shaders revert in texel offsets and weights (system constant
  // textures_resolution_scaled).
  bool IsResolved(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch) const;
  // The sampler for a sampler binding of the fetch constant, never null. `lod_bias`: added to the mip level
  // (the reduced scale's, D49).
  Diligent::ISampler* Sampler(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                              const rex::graphics::SpirvShader::SamplerBinding& binding, float lod_bias = 0.0f);
  struct Stats {
    uint32_t resolved = 0, guest = 0, placeholders = 0;
    uint32_t other_dimension = 0;  // placeholders for the binding of the other dimension (GuestTextures::GetView)
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
  GuestTextures* guest_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::ITexture> placeholder_2d_, placeholder_3d_, placeholder_cube_;
  Diligent::ITextureView* placeholder_views_[3] = {};  // 2D array, 3D, cube
  std::unordered_map<uint64_t, Diligent::RefCntAutoPtr<Diligent::ISampler>> samplers_;
  Stats stats_;
};

}  // namespace xenos_native
