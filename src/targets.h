// NativeXRenderer - host render targets and the textures that hold resolve results (internal).
//
// There is no EDRAM. A guest surface (render target or depth) becomes a host texture of its own size at
// the render scale (scale.h), keyed by what the surface object says: EDRAM base, pitch, MSAA, format and
// size. A resolve copies a host render target into a host texture registered for the destination guest
// texture (address, format, size), also at the render scale, which later draws sample and Present shows;
// guest memory is not written.
//
// With an upscaler (D49), the main 3D view's targets of the game's output size have a second host texture
// at a lower scale, the reduced scale: the view draws into those, and the upscaler brings the result to the
// render scale for what follows. Resolved textures stay at the render scale.
//
// Both kinds are freed after a while unused (D52): each new area brings surfaces and resolve destinations
// of its own (the journal's pages are at a new address in each level), and nothing else would free them.

#pragma once

#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"
#include "TextureView.h"
#include "scale.h"

namespace native_x_renderer {

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
  uint32_t width = 0, height = 0, samples = 1;  // on the host
  uint32_t guest_width = 0, guest_height = 0;
  Diligent::TEXTURE_FORMAT format = Diligent::TEX_FORMAT_UNKNOWN;
  bool depth = false;
  bool reduced = false;  // at the reduced scale (D49)
  RenderScale scale;     // host pixels per guest pixel of this target
  uint64_t last_used = 0;  // the frame it was last drawn to, cleared or resolved from (D52)
};

// The memory of a texture: its mips, slices and samples, compressed blocks included.
uint64_t TextureBytes(const Diligent::TextureDesc& desc);

// What a pass freeing unused textures freed (D52).
struct Freed {
  uint32_t count = 0;
  uint64_t bytes = 0;
};

class RenderTargets {
 public:
  void Initialize(Diligent::IRenderDevice* device, const RenderScale& scale) {
    device_ = device;
    scale_ = scale;
    reduced_scale_ = scale;
  }
  // The scale of the reduced targets (D49).
  void SetReducedScale(const RenderScale& scale) { reduced_scale_ = scale; }
  // The host target of a guest surface, created on first use: at the render scale, or with `reduced` at
  // the reduced scale. nullptr for an unknown format.
  HostTarget* Get(const GuestSurface& surface, bool reduced = false);
  size_t count() const { return targets_.size(); }
  uint64_t bytes() const;  // of the host textures, for the log
  // After a frame: every 600 frames, the targets not used for `unused_frames` are freed. Pointers to them
  // become invalid.
  Freed EndFrame(uint64_t unused_frames);

 private:
  using Key = std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool>;
  Diligent::IRenderDevice* device_ = nullptr;
  RenderScale scale_, reduced_scale_;
  std::map<Key, HostTarget> targets_;
  uint64_t frame_ = 0;
};

// A host texture holding what resolves wrote to a guest texture: a 2D array, one slice per slice of a
// stacked guest texture, or a cube, one slice per face (resolves write one slice or face each, D51).
struct ResolvedTexture {
  Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
  Diligent::ITextureView* srv = nullptr;  // slice 0 as a 2D texture (presenting); none for a cube
  uint32_t width = 0, height = 0, slices = 1;  // on the host
  uint32_t guest_width = 0, guest_height = 0;
  uint32_t guest_format = 0;
  Diligent::TEXTURE_FORMAT format = Diligent::TEX_FORMAT_UNKNOWN;
  bool cube = false;
  uint64_t last_resolve = 0;  // resolve counter, to find the newest at an address
  uint64_t last_used = 0;     // the frame it was last resolved to, sampled or presented (D52)

  // Every slice as a 2D array, as the translated shaders sample 2D and stacked textures (not a cube).
  Diligent::ITextureView* ArrayView() const { return cube ? nullptr : array_view_; }
  // The six faces as a cube, for a cube texture.
  Diligent::ITextureView* CubeView() const { return cube ? array_view_ : nullptr; }
  // A render target view of one slice. Created on first use.
  Diligent::ITextureView* SliceView(uint32_t slice);

 private:
  friend class ResolvedTextures;
  Diligent::ITextureView* array_view_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::ITextureView> slice_0_;
  std::vector<Diligent::RefCntAutoPtr<Diligent::ITextureView>> slice_views_;
};

class ResolvedTextures {
 public:
  void Initialize(Diligent::IRenderDevice* device, const RenderScale& scale) {
    device_ = device;
    scale_ = scale;
  }
  // `address` is the GPU (physical) base address, as fetch constants hold it; the size is the guest
  // texture's, `slices` 6 for a cube. Created on first use, at the render scale; nullptr for an unknown
  // format.
  ResolvedTexture* GetOrCreate(uint32_t address, uint32_t guest_format, uint32_t width, uint32_t height,
                               uint32_t slices, bool cube = false);
  ResolvedTexture* Find(uint32_t address, uint32_t guest_format, uint32_t width, uint32_t height,
                        uint32_t slices, bool cube = false);
  // The texture most recently resolved to `address`, whatever its format and size; with `cube`, only
  // among the cube textures, or only among the others.
  ResolvedTexture* FindNewest(uint32_t address);
  ResolvedTexture* FindNewest(uint32_t address, bool cube);
  void Touch(ResolvedTexture* texture) {
    texture->last_resolve = ++counter_;
    texture->last_used = frame_;
  }
  // Sampled or presented this frame (D52).
  void MarkUsed(ResolvedTexture* texture) { texture->last_used = frame_; }
  size_t count() const { return textures_.size(); }
  uint64_t bytes() const;  // of the host textures, for the log
  // After a frame: every 600 frames, the textures not used for `unused_frames` are freed. Pointers to them
  // become invalid.
  Freed EndFrame(uint64_t unused_frames);

 private:
  using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool>;
  Diligent::IRenderDevice* device_ = nullptr;
  RenderScale scale_;
  std::map<Key, ResolvedTexture> textures_;
  uint64_t counter_ = 0;
  uint64_t frame_ = 0;
};

// Host formats for guest formats (as the SDK's Vulkan backend chooses them).
Diligent::TEXTURE_FORMAT HostColorFormat(uint32_t color_render_target_format);
Diligent::TEXTURE_FORMAT HostDepthFormat(uint32_t depth_render_target_format);
Diligent::TEXTURE_FORMAT HostTextureFormat(uint32_t texture_format);

}  // namespace native_x_renderer
