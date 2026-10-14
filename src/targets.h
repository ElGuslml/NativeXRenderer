// XenosNative - host render targets and the textures that hold resolve results (internal).
//
// There is no EDRAM. A guest surface (render target or depth) becomes a host texture of its own size,
// keyed by what the surface object says: EDRAM base, pitch, MSAA, format and size. A resolve copies a
// host render target into a host texture registered for the destination guest texture (address,
// format, size), which later draws sample and Present shows; guest memory is not written.

#pragma once

#include <cstdint>
#include <map>
#include <tuple>

#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"
#include "TextureView.h"

namespace xenos_native {

// A guest surface as its D3D surface object describes it (XGSetSurfaceHeader).
struct GuestSurface {
  uint32_t object = 0;
  uint32_t surface_info = 0;  // RB_SURFACE_INFO: pitch, MSAA
  uint32_t info = 0;          // RB_COLOR_INFO or RB_DEPTH_INFO: EDRAM base, format
  uint32_t width = 0, height = 0;
  bool depth = false;

  uint32_t pitch() const { return surface_info & 0x3FFF; }
  uint32_t msaa() const { return (surface_info >> 16) & 3; }  // 0: 1x, 1: 2x, 2: 4x
  uint32_t samples() const { return 1u << msaa(); }
  uint32_t edram_base() const { return info & 0xFFF; }
  uint32_t format() const { return (info >> 16) & 0xF; }
  explicit operator bool() const { return object != 0; }
};

// Reads a surface object: +24 RB_SURFACE_INFO, +28 RB_COLOR_INFO / RB_DEPTH_INFO, +36 the size
// (width - 1 in bits 18-31, height - 1 in bits 3-17).
GuestSurface ReadSurface(const uint8_t* base, uint32_t object, bool depth);

struct HostTarget {
  Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
  Diligent::ITextureView* view = nullptr;  // render target or depth-stencil view
  Diligent::ITextureView* srv = nullptr;
  uint32_t width = 0, height = 0, samples = 1;
  Diligent::TEXTURE_FORMAT format = Diligent::TEX_FORMAT_UNKNOWN;
  bool depth = false;
};

class RenderTargets {
 public:
  void Initialize(Diligent::IRenderDevice* device) { device_ = device; }
  // The host target of a guest surface, created on first use. nullptr for an unknown format.
  HostTarget* Get(const GuestSurface& surface);
  size_t count() const { return targets_.size(); }

 private:
  using Key = std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
  Diligent::IRenderDevice* device_ = nullptr;
  std::map<Key, HostTarget> targets_;
};

// A host texture holding what resolves wrote to a guest texture.
struct ResolvedTexture {
  Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
  Diligent::ITextureView* rtv = nullptr;
  Diligent::ITextureView* srv = nullptr;
  uint32_t width = 0, height = 0;
  uint32_t guest_format = 0;
  Diligent::TEXTURE_FORMAT format = Diligent::TEX_FORMAT_UNKNOWN;
  uint64_t last_resolve = 0;  // resolve counter, to find the newest at an address

  // A one-slice 2D array view, as the translated shaders sample 2D textures. Created on first use.
  Diligent::ITextureView* ArrayView();

 private:
  Diligent::RefCntAutoPtr<Diligent::ITextureView> array_view_;
};

class ResolvedTextures {
 public:
  void Initialize(Diligent::IRenderDevice* device) { device_ = device; }
  // `address` is the GPU (physical) base address, as fetch constants hold it. Created on first use;
  // nullptr for an unknown format.
  ResolvedTexture* GetOrCreate(uint32_t address, uint32_t guest_format, uint32_t width, uint32_t height);
  ResolvedTexture* Find(uint32_t address, uint32_t guest_format, uint32_t width, uint32_t height);
  // The texture most recently resolved to `address`, whatever its format and size.
  ResolvedTexture* FindNewest(uint32_t address);
  void Touch(ResolvedTexture* texture) { texture->last_resolve = ++counter_; }
  size_t count() const { return textures_.size(); }

 private:
  using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
  Diligent::IRenderDevice* device_ = nullptr;
  std::map<Key, ResolvedTexture> textures_;
  uint64_t counter_ = 0;
};

// Host formats for guest formats (as the SDK's Vulkan backend chooses them).
Diligent::TEXTURE_FORMAT HostColorFormat(uint32_t color_render_target_format);
Diligent::TEXTURE_FORMAT HostDepthFormat(uint32_t depth_render_target_format);
Diligent::TEXTURE_FORMAT HostTextureFormat(uint32_t texture_format);

}  // namespace xenos_native
