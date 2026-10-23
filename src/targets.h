// NativeXRenderer - host render targets and the textures that hold resolve results (internal).
//
// There is no EDRAM. A guest surface (render target or depth) becomes a host texture of its own size at
// the render scale (scale.h), keyed by what the surface object says: EDRAM base, pitch, MSAA, format and
// size; in a tiled pass the renderer gives it the size of all the tiles (D60). Host targets have one
// sample: the game's MSAA is not brought over (D31, D60). A resolve copies a host render target into a host
// texture registered for the destination guest texture (address, format, size), also at the render scale,
// which later draws sample and Present shows; guest memory is not written.
//
// With an upscaler (D49), the main 3D view's targets of the game's output size have a second host texture
// at a lower scale, the reduced scale: the view draws into those, and the upscaler brings the result to the
// render scale for what follows. Resolved textures stay at the render scale.
//
// Shadow maps (D92) can have a scale of their own, the shadow scale: a depth surface drawn or cleared with no
// color surface bound, which is not of the game's output size, is marked a shadow map, and its host target, and the
// textures resolved from it, are at the shadow scale.
//
// Both kinds are freed after a while unused (D52): each new area brings surfaces and resolve destinations
// of its own (the journal's pages are at a new address in each level), and nothing else would free them.
//
// A resolved texture stands for guest memory that the resolve did not write. When the CPU writes that memory,
// the resolved texture is invalidated, and fetches read the guest's data there again (D70). Allocating or
// releasing the memory does not change its data, on the console or in the SDK: a resolved texture outlives
// that (D74). The memory's invalidation callbacks report both, the CPU's writes with a range around the
// written page and the others with their exact range; for those, the guest's data is compared with what it
// was at the resolve.

#pragma once

#include <native_x_renderer/native_x_renderer.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"
#include "TextureView.h"
#include "scale.h"

namespace native_x_renderer {

class GuestMemory;

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

// Reads a surface object where the profile's object layout says (POTCAWE, XDK 5426: +24 RB_SURFACE_INFO, +28
// RB_COLOR_INFO / RB_DEPTH_INFO, +36 the size; GRAW, 2417: +48, +52, +24).
GuestSurface ReadSurface(const uint8_t* base, uint32_t object, bool depth, const ObjectLayout& layout);

struct HostTarget {
  Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
  Diligent::ITextureView* view = nullptr;  // render target or depth-stencil view
  Diligent::ITextureView* srv = nullptr;
  // Depth targets: the stencil, as an integer texture (D64).
  Diligent::RefCntAutoPtr<Diligent::ITextureView> stencil_srv;
  uint32_t width = 0, height = 0, samples = 1;  // on the host
  uint32_t guest_width = 0, guest_height = 0;
  Diligent::TEXTURE_FORMAT format = Diligent::TEX_FORMAT_UNKNOWN;
  bool depth = false;
  bool reduced = false;  // at the reduced scale (D49)
  bool shadow = false;   // a shadow map, at the shadow scale (D92)
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
  // The scale a host target is at: the render scale, the reduced one (D49) or the shadow maps' (D92).
  enum class Kind : uint8_t { kRender, kReduced, kShadow };

  void Initialize(Diligent::IRenderDevice* device, const RenderScale& scale) {
    device_ = device;
    scale_ = scale;
    reduced_scale_ = scale;
    shadow_scale_ = scale;
  }
  // The scale of the reduced targets (D49).
  void SetReducedScale(const RenderScale& scale) { reduced_scale_ = scale; }
  // The scale of the shadow maps (D92).
  void SetShadowScale(const RenderScale& scale) { shadow_scale_ = scale; }
  // The host target of a guest surface, created on first use, at the scale of its kind. nullptr for an unknown
  // format.
  HostTarget* Get(const GuestSurface& surface, Kind kind = Kind::kRender);
  // A depth surface found to be a shadow map (D92), and whether one is: the uses that do not show it (a resolve
  // from it) then find its target too.
  void MarkShadow(const GuestSurface& surface) { shadow_surfaces_.insert(SurfaceKey(surface)); }
  bool IsShadow(const GuestSurface& surface) const {
    return !shadow_surfaces_.empty() && shadow_surfaces_.count(SurfaceKey(surface)) != 0;
  }
  size_t count() const { return targets_.size(); }
  uint64_t bytes() const;  // of the host textures, for the log
  // After a frame: every 600 frames, the targets not used for `unused_frames` are freed. Pointers to them
  // become invalid.
  Freed EndFrame(uint64_t unused_frames);

 private:
  // A surface as targets are keyed: depth, EDRAM base, pitch, MSAA, storage format, size.
  using SurfaceTuple = std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
  using Key = std::tuple<SurfaceTuple, Kind>;
  static SurfaceTuple SurfaceKey(const GuestSurface& surface);
  Diligent::IRenderDevice* device_ = nullptr;
  RenderScale scale_, reduced_scale_, shadow_scale_;
  std::map<Key, HostTarget> targets_;
  std::set<SurfaceTuple> shadow_surfaces_;
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
  RenderScale scale;    // host pixels per guest pixel: the render scale, or the shadow scale (D92)
  bool shadow = false;  // resolved from shadow maps, at the shadow scale (D92)
  Diligent::TEXTURE_FORMAT format = Diligent::TEX_FORMAT_UNKNOWN;
  bool cube = false;
  uint32_t address = 0;       // the guest texture's GPU address
  uint32_t guest_bytes = 0;   // what it covers in guest memory from there, as of its last resolve
  // The guest memory it covers was allocated again or written by the CPU since its last resolve (D70).
  bool invalidated = false;
  // That memory is marked written by the GPU and watched in the mirror, as of its last resolve (D70). It stays so
  // until a watch reports it, which invalidates the texture.
  bool watched = false;
  uint64_t last_resolve = 0;  // resolve counter, to find the newest at an address
  uint64_t last_used = 0;     // the frame it was last resolved to, sampled or presented (D52)
  uint64_t last_resolve_frame = 0;  // the frame of its last resolve, for the log
  // The guest's data in the memory it covers, as of the resolve that started watching it (D74).
  uint64_t guest_hash = 0;

  // Every slice as a 2D array, as the translated shaders sample 2D and stacked textures (not a cube).
  Diligent::ITextureView* ArrayView() const { return cube ? nullptr : array_view_; }
  // The six faces as a cube, for a cube texture.
  Diligent::ITextureView* CubeView() const { return cube ? array_view_ : nullptr; }
  // A render target view of one slice. Created on first use.
  Diligent::ITextureView* SliceView(uint32_t slice);

  // A depth resolve into a k_24_8 or k_24_8_FLOAT texture writes depth and stencil as 32-bit words, which a
  // fetch may read as k_8_8_8_8 to get the stencil (GRAW's objective outline). The host texture holds the
  // depth; once a fetch reads it as k_8_8_8_8 (`read_packed`), the depth resolves into it also write the
  // words, as RGBA8, here (D64).
  bool read_packed = false;
  bool packed_written = false;  // by the newest resolve into this texture
  Diligent::ITextureView* PackedArrayView() const { return packed_array_view_; }
  // A render target view of one slice of the packed texture, which is created on first use.
  Diligent::ITextureView* PackedSliceView(Diligent::IRenderDevice* device, uint32_t slice);

 private:
  friend class ResolvedTextures;
  Diligent::ITextureView* array_view_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::ITextureView> slice_0_;
  std::vector<Diligent::RefCntAutoPtr<Diligent::ITextureView>> slice_views_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> packed_;
  Diligent::ITextureView* packed_array_view_ = nullptr;
  std::vector<Diligent::RefCntAutoPtr<Diligent::ITextureView>> packed_slice_views_;
};

class ResolvedTextures {
 public:
  void Initialize(Diligent::IRenderDevice* device, const RenderScale& scale) {
    device_ = device;
    scale_ = scale;
    shadow_scale_ = scale;
  }
  // The scale of the textures resolved from shadow maps (D92).
  void SetShadowScale(const RenderScale& scale) { shadow_scale_ = scale; }
  // Watches the guest memory the resolved textures cover, through the invalidation callbacks of the memory
  // `memory` mirrors (D70, D74); nullptr stops. The callbacks report on guest threads; the ranges are applied on
  // the renderer's thread, by the next call below.
  void Watch(GuestMemory* memory);
  // `address` is the GPU (physical) base address, as fetch constants hold it; the size is the guest
  // texture's, `slices` 6 for a cube. Created on first use, at the render scale, or with `shadow` at the shadow
  // scale (resolved from shadow maps, D92); nullptr for an unknown format.
  ResolvedTexture* GetOrCreate(uint32_t address, uint32_t guest_format, uint32_t width, uint32_t height,
                               uint32_t slices, bool cube = false, bool shadow = false);
  ResolvedTexture* Find(uint32_t address, uint32_t guest_format, uint32_t width, uint32_t height,
                        uint32_t slices, bool cube = false, bool shadow = false);
  // The texture most recently resolved to `address`, whatever its format and size; with `cube`, only
  // among the cube textures, or only among the others. nullptr if that one was invalidated (D70): the guest
  // memory holds newer data.
  ResolvedTexture* FindNewest(uint32_t address);
  ResolvedTexture* FindNewest(uint32_t address, bool cube);
  // After a resolve into `texture`, which covers `guest_bytes` of guest memory: the newest at its address,
  // valid again, and that memory watched (D70).
  void Touch(ResolvedTexture* texture, uint32_t guest_bytes);
  // Sampled or presented this frame (D52).
  void MarkUsed(ResolvedTexture* texture) { texture->last_used = frame_; }
  // Invalidates the textures the guest memory ranges queued by the watch overlap: always for a CPU write, else if the
  // guest's data there changed since the resolve (D74). The calls above apply them first; cheap when none is queued.
  void ApplyInvalidations();
  // Changes whenever what FindNewest returns may have changed (D83): a texture created, resolved to, invalidated or
  // freed.
  uint64_t generation() const { return generation_; }
  // A fetch found no valid resolved texture at `address`: if a resolve wrote there and its texture was invalidated
  // (D70) or freed (D52) since, the draw reads the guest's memory, which no resolve wrote. Counted, and the first
  // such addresses logged: the diagnosis of a texture the game resolved once and still samples.
  void NoteGuestRead(uint32_t address);
  uint32_t TakeGoneReads() {
    uint32_t reads = gone_reads_;
    gone_reads_ = 0;
    return reads;
  }
  size_t count() const { return textures_.size(); }
  // Textures invalidated since the last call, for the log.
  uint32_t TakeInvalidated() {
    uint32_t invalidated = invalidated_;
    invalidated_ = 0;
    return invalidated;
  }
  // Textures kept since the last call: their memory was allocated, released or reprotected, its data unchanged
  // (D74).
  uint32_t TakeKept() {
    uint32_t kept = kept_;
    kept_ = 0;
    return kept;
  }
  uint64_t bytes() const;  // of the host textures, for the log
  // After a frame: every 600 frames, the textures not used for `unused_frames` are freed. Pointers to them
  // become invalid.
  Freed EndFrame(uint64_t unused_frames);

 private:
  using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool, bool>;
  // The memory's invalidation callback: queues the range. `exact_range`: not a CPU write (an allocation, a
  // release, a protection change, a file read).
  static std::pair<uint32_t, uint32_t> OnMemoryInvalidated(void* context, uint32_t start, uint32_t length,
                                                           bool exact_range);
  uint64_t HashGuest(const ResolvedTexture& texture) const;

  Diligent::IRenderDevice* device_ = nullptr;
  RenderScale scale_, shadow_scale_;
  std::map<Key, ResolvedTexture> textures_;
  uint64_t counter_ = 0;
  uint64_t generation_ = 1;
  uint64_t frame_ = 0;
  GuestMemory* memory_ = nullptr;
  void* watch_ = nullptr;
  std::mutex pending_mutex_;
  struct Range {
    uint32_t first, last;
    bool cpu_write;
  };
  std::vector<Range> pending_;  // guarded by pending_mutex_
  std::vector<Range> applying_;
  std::atomic<bool> has_pending_{false};
  uint32_t invalidated_ = 0, logged_invalidations_ = 0, kept_ = 0, logged_kept_ = 0;
  // The addresses resolves wrote whose textures are gone (invalidated or freed), until a resolve writes there again.
  struct Gone {
    uint32_t width, height, format;
    uint64_t last_resolve_frame, gone_frame;
    bool freed;
    bool logged;
  };
  std::map<uint32_t, Gone> gone_;
  uint32_t gone_reads_ = 0, logged_gone_reads_ = 0;
  void NoteGone(const ResolvedTexture& texture, bool freed);
};

// Whether a fetch of this guest format can read what a resolve wrote: not a block-compressed one, as a resolve never
// writes blocks of 4x4 texels; such a fetch reads the guest's data even at a resolve destination's address (D70).
bool ResolvableFormat(uint32_t texture_format);

// Host formats for guest formats (as the SDK's Vulkan backend chooses them).
Diligent::TEXTURE_FORMAT HostColorFormat(uint32_t color_render_target_format);
Diligent::TEXTURE_FORMAT HostDepthFormat(uint32_t depth_render_target_format);
Diligent::TEXTURE_FORMAT HostTextureFormat(uint32_t texture_format);

}  // namespace native_x_renderer
