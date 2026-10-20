// NativeXRenderer - what a draw's texture fetch constants bind (internal).
//
// For each texture binding of the translated shaders, in this order:
//   - a texture a resolve wrote (targets.h), found by its address; a depth resolve read as k_8_8_8_8 binds
//     its depth and stencil words as bytes (D64);
//   - the guest texture loaded from memory (guest_textures.h);
//   - a placeholder (opaque white), when neither exists (an unsupported format, a dimension mismatch).
// The translator samples 1D and 2D textures as 2D arrays, stacked and 3D textures as 3D, cubes as cubes,
// and applies the texture's swizzle itself (image_view_format_swizzle is off): views are RGBA identity,
// and the swizzles come from the texture cache (system constants, draw.cpp).
// What the look-ups find for a fetch constant's contents is kept while the resolved textures and the guest
// texture bindings stay as they were, within a frame (D83): a draw binds about nine textures, and most draws
// bind the same ones as the draw before.

#pragma once

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Sampler.h"
#include "Texture.h"
#include "TextureView.h"

namespace native_x_renderer {

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
                   uint32_t& swizzle, uint8_t& signs);
  // Whether View binds a resolve result for this fetch constant: such textures are at the render scale,
  // which the translated shaders revert in texel offsets and weights (system constant
  // textures_resolution_scaled).
  bool IsResolved(uint32_t fetch_constant, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);
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
  // Once per frame, after GuestTextures::BeginFrame: the kept look-ups are dropped, so that the textures they
  // found are marked used again in the new frame (the SDK's LRU; the resolved textures', D52) (D83).
  void BeginFrame() { ++generation_; }

 private:
  // The look-ups themselves.
  Diligent::ITextureView* FindView(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, uint32_t fetch_constant,
                                   rex::graphics::xenos::FetchOpDimension dimension, bool is_signed, uint8_t& kind,
                                   bool& guest_read);
  void FindSwizzle(uint32_t fetch_constant, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                   uint32_t& swizzle, uint8_t& signs) const;
  Diligent::ISampler* FindSampler(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                                  const rex::graphics::SpirvShader::SamplerBinding& binding, float lod_bias);
  // The resolved textures' queued invalidations applied, and a new generation of kept look-ups when the
  // resolved textures or the guest texture bindings changed.
  void Sync();
  // A look-up kept for a fetch constant's contents, valid for one generation (0: none).
  template <typename T>
  struct Kept {
    std::array<uint32_t, 6> fetch{};
    uint64_t generation = 0;
    T value{};
    bool Matches(const rex::graphics::xenos::xe_gpu_texture_fetch_t& f, uint64_t g) const {
      return generation == g && !std::memcmp(fetch.data(), &f, sizeof(fetch));
    }
    void Keep(const rex::graphics::xenos::xe_gpu_texture_fetch_t& f, uint64_t g, const T& v) {
      std::memcpy(fetch.data(), &f, sizeof(fetch));
      generation = g;
      value = v;
    }
  };
  struct ViewFound {
    Diligent::ITextureView* view;
    uint8_t kind;     // the Stats counter it counts in: resolved, guest, placeholders, other dimension
    bool guest_read;  // ResolvedTextures::NoteGuestRead for the fetch's address
  };
  struct SwizzleFound {
    uint32_t swizzle;
    uint8_t signs;
  };
  // Per fetch constant: views by binding dimension (2D, 3D, cube) and sign, swizzles, IsResolved.
  std::array<Kept<ViewFound>, 32 * 3 * 2> views_{};
  std::array<Kept<SwizzleFound>, 32> swizzles_{};
  std::array<Kept<bool>, 32> resolved_kept_{};
  uint64_t generation_ = 1, resolved_generation_ = 0, guest_generation_ = 0;
  // Samplers by everything FindSampler reads: the fetch constant, the binding, the bias, the anisotropic
  // override; direct-mapped by the binding.
  struct SamplerKept {
    std::array<uint32_t, 9> key{};
    Diligent::ISampler* sampler = nullptr;
  };
  std::array<SamplerKept, 128> samplers_kept_{};
  void Count(uint8_t kind);

  Diligent::IRenderDevice* device_ = nullptr;
  ResolvedTextures* resolved_ = nullptr;
  GuestTextures* guest_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::ITexture> placeholder_2d_, placeholder_3d_, placeholder_cube_;
  // Zero words for a depth resolve read as k_8_8_8_8 before its words are written (D64).
  Diligent::RefCntAutoPtr<Diligent::ITexture> placeholder_packed_;
  Diligent::ITextureView* placeholder_views_[3] = {};  // 2D array, 3D, cube
  std::unordered_map<uint64_t, Diligent::RefCntAutoPtr<Diligent::ISampler>> samplers_;
  Stats stats_;
};

}  // namespace native_x_renderer
