// NativeXRenderer - see targets.h.

#include "targets.h"

#include "guest_memory.h"
#include "internal.h"

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "GraphicsAccessories.hpp"

#include <xxhash.h>

#include <set>
#include <string>

namespace native_x_renderer {

using namespace Diligent;
namespace xenos = rex::graphics::xenos;

GuestSurface ReadSurface(const uint8_t* base, uint32_t object, bool depth, const ObjectLayout& layout) {
  GuestSurface s;
  if (!object) return s;
  s.object = object;
  s.depth = depth;
  s.surface_info = internal::Load32(base, object + layout.surface_info);
  s.info = internal::Load32(base, object + layout.surface_color_depth_info);
  uint32_t size = internal::Load32(base, object + layout.surface_size);
  s.width = layout.surface_width_minus_one.Get(size) + 1;
  s.height = layout.surface_height_minus_one.Get(size) + 1;
  return s;
}

TEXTURE_FORMAT HostColorFormat(uint32_t format) {
  switch (xenos::ColorRenderTargetFormat(format)) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return TEX_FORMAT_RGBA8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return TEX_FORMAT_RGBA16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return TEX_FORMAT_RG16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return TEX_FORMAT_RGBA16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return TEX_FORMAT_RG16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return TEX_FORMAT_R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return TEX_FORMAT_RG32_FLOAT;
    default:
      return TEX_FORMAT_UNKNOWN;
  }
}

TEXTURE_FORMAT HostDepthFormat(uint32_t format) {
  return xenos::DepthRenderTargetFormat(format) == xenos::DepthRenderTargetFormat::kD24FS8
             ? TEX_FORMAT_D32_FLOAT_S8X24_UINT
             : TEX_FORMAT_D24_UNORM_S8_UINT;
}

TEXTURE_FORMAT HostTextureFormat(uint32_t format) {
  switch (xenos::TextureFormat(format)) {
    case xenos::TextureFormat::k_8:
      return TEX_FORMAT_R8_UNORM;
    case xenos::TextureFormat::k_8_8:
      return TEX_FORMAT_RG8_UNORM;
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_1_5_5_5:
    case xenos::TextureFormat::k_4_4_4_4:
      return TEX_FORMAT_RGBA8_UNORM;
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
      return TEX_FORMAT_RGB10A2_UNORM;
    case xenos::TextureFormat::k_10_11_11:
    case xenos::TextureFormat::k_11_11_10:
      return TEX_FORMAT_R11G11B10_FLOAT;
    case xenos::TextureFormat::k_16:
      return TEX_FORMAT_R16_UNORM;
    case xenos::TextureFormat::k_16_16:
      return TEX_FORMAT_RG16_UNORM;
    case xenos::TextureFormat::k_16_16_16_16:
      return TEX_FORMAT_RGBA16_UNORM;
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_16_EXPAND:
      return TEX_FORMAT_R16_FLOAT;
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_EXPAND:
      return TEX_FORMAT_RG16_FLOAT;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16_EXPAND:
      return TEX_FORMAT_RGBA16_FLOAT;
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
      return TEX_FORMAT_R32_FLOAT;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return TEX_FORMAT_RG32_FLOAT;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return TEX_FORMAT_RGBA32_FLOAT;
    default:
      return TEX_FORMAT_UNKNOWN;
  }
}

HostTarget* RenderTargets::Get(const GuestSurface& s, bool reduced) {
  if (!s) return nullptr;
  // Keyed by the EDRAM storage format, as the SDK does: the color formats "_AS_" only change what resolves
  // convert the data to, and the XDK rewrites a surface's format between its draws and its resolve
  // (xdk-d3d-reference.md).
  uint32_t guest_format =
      s.depth ? s.format() : uint32_t(xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(s.format())));
  Key key{s.depth, s.edram_base(), s.pitch(), s.msaa(), guest_format, s.width, s.height, reduced};
  auto it = targets_.find(key);
  if (it != targets_.end()) {
    it->second.last_used = frame_;
    return it->second.texture ? &it->second : nullptr;
  }
  HostTarget& target = targets_[key];
  target.last_used = frame_;

  TEXTURE_FORMAT format = s.depth ? HostDepthFormat(guest_format) : HostColorFormat(guest_format);
  if (format == TEX_FORMAT_UNKNOWN) {
    REXLOG_WARN("[native_x_renderer] render target format {} not supported", guest_format);
    return nullptr;
  }
  TextureDesc desc;
  std::string name = std::string(s.depth ? "NativeXRenderer depth " : "NativeXRenderer color ") + std::to_string(s.width) +
                     "x" + std::to_string(s.height) + " base " + std::to_string(s.edram_base()) +
                     (reduced ? " reduced" : "");
  const RenderScale& scale = reduced ? reduced_scale_ : scale_;
  desc.Name = name.c_str();
  desc.Type = RESOURCE_DIM_TEX_2D;
  desc.Width = scale.HostWidth(s.width);
  desc.Height = scale.HostHeight(s.height);
  desc.Format = format;
  // Without the game's MSAA (D31, D60): one sample per pixel, whatever the surface has. On the console MSAA
  // is what splits a scene into EDRAM tiles; NativeXRenderer antialiases with its own techniques.
  desc.SampleCount = 1;
  desc.BindFlags = BIND_SHADER_RESOURCE | (s.depth ? BIND_DEPTH_STENCIL : BIND_RENDER_TARGET);
  device_->CreateTexture(desc, nullptr, &target.texture);
  if (!target.texture) {
    REXLOG_ERROR("[native_x_renderer] could not create the render target {}", name);
    return nullptr;
  }
  target.view = target.texture->GetDefaultView(s.depth ? TEXTURE_VIEW_DEPTH_STENCIL : TEXTURE_VIEW_RENDER_TARGET);
  target.srv = target.texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  if (s.depth) {
    TextureViewDesc stencil;
    stencil.ViewType = TEXTURE_VIEW_SHADER_RESOURCE;
    stencil.Format = format == TEX_FORMAT_D32_FLOAT_S8X24_UINT ? TEX_FORMAT_X32_TYPELESS_G8X24_UINT
                                                               : TEX_FORMAT_X24_TYPELESS_G8_UINT;
    target.texture->CreateView(stencil, &target.stencil_srv);
  }
  target.width = desc.Width;
  target.height = desc.Height;
  target.guest_width = s.width;
  target.guest_height = s.height;
  target.samples = desc.SampleCount;
  target.format = format;
  target.depth = s.depth;
  target.reduced = reduced;
  target.scale = scale;
  REXLOG_INFO("[native_x_renderer] new render target: {} (pitch {}, {}x MSAA in the guest, guest format {}; {}x{} on the "
              "host)",
              name, s.pitch(), s.samples(), guest_format, desc.Width, desc.Height);
  return &target;
}

ITextureView* ResolvedTexture::SliceView(uint32_t slice) {
  if (!texture || slice >= slices) return nullptr;
  if (slice_views_.empty()) slice_views_.resize(slices);
  RefCntAutoPtr<ITextureView>& view = slice_views_[slice];
  if (!view) {
    TextureViewDesc desc;
    desc.ViewType = TEXTURE_VIEW_RENDER_TARGET;
    desc.TextureDim = RESOURCE_DIM_TEX_2D_ARRAY;
    desc.FirstArraySlice = slice;
    desc.NumArraySlices = 1;
    texture->CreateView(desc, &view);
  }
  return view;
}

ITextureView* ResolvedTexture::PackedSliceView(IRenderDevice* device, uint32_t slice) {
  if (!texture || cube || slice >= slices) return nullptr;
  if (!packed_) {
    TextureDesc desc;
    std::string name = std::string("NativeXRenderer resolve packed ") + std::to_string(guest_width) + "x" +
                       std::to_string(guest_height) + "x" + std::to_string(slices);
    desc.Name = name.c_str();
    desc.Type = RESOURCE_DIM_TEX_2D_ARRAY;
    desc.Width = width;
    desc.Height = height;
    desc.ArraySize = slices;
    desc.Format = TEX_FORMAT_RGBA8_UNORM;
    desc.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    device->CreateTexture(desc, nullptr, &packed_);
    if (!packed_) return nullptr;
    packed_array_view_ = packed_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    packed_slice_views_.resize(slices);
  }
  RefCntAutoPtr<ITextureView>& view = packed_slice_views_[slice];
  if (!view) {
    TextureViewDesc desc;
    desc.ViewType = TEXTURE_VIEW_RENDER_TARGET;
    desc.TextureDim = RESOURCE_DIM_TEX_2D_ARRAY;
    desc.FirstArraySlice = slice;
    desc.NumArraySlices = 1;
    packed_->CreateView(desc, &view);
  }
  return view;
}

void ResolvedTextures::Watch(GuestMemory* memory) {
  if (memory_ && watch_) memory_->memory().UnregisterPhysicalMemoryInvalidationCallback(watch_);
  watch_ = nullptr;
  memory_ = memory;
  if (memory_) watch_ = memory_->memory().RegisterPhysicalMemoryInvalidationCallback(OnMemoryInvalidated, this);
}

std::pair<uint32_t, uint32_t> ResolvedTextures::OnMemoryInvalidated(void* context, uint32_t start, uint32_t length,
                                                                    bool exact_range) {
  // A guest thread, in the global critical region, for a page some listener watches: a CPU write to it (not
  // exact: the page written), or an allocation, a release, a protection change, a file read (exact). The pages a
  // resolve covers are watched (ResolveWritten).
  auto* self = static_cast<ResolvedTextures*>(context);
  if (length) {
    uint32_t first = start, last = start + (length - 1);
    bool cpu_write = !exact_range;
    std::lock_guard lock(self->pending_mutex_);
    auto& pending = self->pending_;
    if (!pending.empty() && pending.back().cpu_write == cpu_write && first <= pending.back().last + 1 &&
        last + 1 >= pending.back().first) {
      pending.back() = {std::min(first, pending.back().first), std::max(last, pending.back().last), cpu_write};
    } else {
      pending.push_back({first, last, cpu_write});
    }
    self->has_pending_.store(true, std::memory_order_release);
  }
  // Whatever the others unwatch.
  return {0, UINT32_MAX};
}

uint64_t ResolvedTextures::HashGuest(const ResolvedTexture& texture) const {
  const uint8_t* data = memory_ ? memory_->memory().TranslatePhysical<const uint8_t*>(texture.address) : nullptr;
  return data && texture.guest_bytes ? XXH3_64bits(data, texture.guest_bytes) : 0;
}

void ResolvedTextures::ApplyInvalidations() {
  if (!has_pending_.load(std::memory_order_acquire)) return;
  {
    std::lock_guard lock(pending_mutex_);
    applying_.swap(pending_);
    has_pending_.store(false, std::memory_order_relaxed);
  }
  for (auto& [key, texture] : textures_) {
    if (!texture.texture || texture.invalidated || !texture.guest_bytes) continue;
    uint32_t first = texture.address, last = texture.address + (texture.guest_bytes - 1);
    bool cpu_write = false, other = false;
    uint32_t range_first = 0, range_last = 0;
    for (const Range& range : applying_) {
      if (range.first > last || range.last < first) continue;
      (range.cpu_write ? cpu_write : other) = true;
      range_first = range.first;
      range_last = range.last;
      if (cpu_write) break;
    }
    if (!cpu_write && !other) continue;
    if (!cpu_write && HashGuest(texture) == texture.guest_hash) {
      // Allocated, released or reprotected, with its data unchanged: as on the console, the memory still holds
      // the resolve. Watched again, so that the CPU's next write is reported (D74).
      ++kept_;
      if (logged_kept_ < 32) {
        ++logged_kept_;
        REXLOG_INFO("[native_x_renderer] resolve target {:#x} {}x{}x{} guest format {} kept: guest memory "
                    "{:#x}-{:#x} allocated, released or reprotected with its data unchanged, {} frames after its "
                    "last resolve{}",
                    texture.address, texture.guest_width, texture.guest_height, texture.slices, texture.guest_format,
                    range_first, range_last, frame_ - texture.last_resolve_frame,
                    logged_kept_ == 32 ? " (not logged again)" : "");
      }
      memory_->ResolveWritten(texture.address, texture.guest_bytes);
      texture.watched = true;
      continue;
    }
    texture.invalidated = true;
    texture.watched = false;
    ++generation_;
    ++invalidated_;
    NoteGone(texture, false);
    if (logged_invalidations_ < 32) {
      ++logged_invalidations_;
      REXLOG_INFO("[native_x_renderer] resolve target {:#x} {}x{}x{} guest format {} invalidated: guest memory "
                  "{:#x}-{:#x} {}, {} frames after its last resolve{}",
                  texture.address, texture.guest_width, texture.guest_height, texture.slices, texture.guest_format,
                  range_first, range_last,
                  cpu_write ? "written by the CPU" : "allocated, released or read into with new data",
                  frame_ - texture.last_resolve_frame, logged_invalidations_ == 32 ? " (not logged again)" : "");
    }
  }
  applying_.clear();
}

void ResolvedTextures::Touch(ResolvedTexture* texture, uint32_t guest_bytes) {
  ApplyInvalidations();  // what happened before this resolve
  texture->last_resolve = ++counter_;
  ++generation_;
  texture->last_used = frame_;
  texture->last_resolve_frame = frame_;
  texture->invalidated = false;
  gone_.erase(texture->address);
  // Marking the pages again each resolve walks them in three heaps; nothing unwatches them without the watch
  // reporting it, which clears `watched`.
  if (texture->watched && texture->guest_bytes == guest_bytes) return;
  texture->guest_bytes = guest_bytes;
  texture->watched = memory_ && guest_bytes;
  if (texture->watched) {
    memory_->ResolveWritten(texture->address, guest_bytes);
    texture->guest_hash = HashGuest(*texture);
  }
}

bool ResolvableFormat(uint32_t texture_format) {
  // DXT, DXN, CTX1 and the like: blocks of 4x4 texels. The formats that read a 32-bit word as several narrower
  // texels (k_32_AS_8, ...) are blocks of one row and may read a resolve.
  const rex::graphics::FormatInfo* info = rex::graphics::FormatInfo::Get(texture_format);
  return info && info->block_height == 1;
}

ResolvedTexture* ResolvedTextures::GetOrCreate(uint32_t address, uint32_t guest_format, uint32_t width,
                                               uint32_t height, uint32_t slices, bool cube) {
  ApplyInvalidations();
  if (cube) slices = 6;
  Key key{address, guest_format, width, height, slices, cube};
  auto it = textures_.find(key);
  if (it != textures_.end()) return it->second.texture ? &it->second : nullptr;
  ResolvedTexture& texture = textures_[key];
  ++generation_;
  texture.last_used = frame_;
  TEXTURE_FORMAT format = HostTextureFormat(guest_format);
  if (format == TEX_FORMAT_UNKNOWN) {
    REXLOG_WARN("[native_x_renderer] resolve to texture format {} not supported", guest_format);
    return nullptr;
  }
  TextureDesc desc;
  std::string name = std::string(cube ? "NativeXRenderer resolve cube " : "NativeXRenderer resolve ") +
                     std::to_string(width) + "x" + std::to_string(height) + "x" + std::to_string(slices) + " at " +
                     std::to_string(address);
  desc.Name = name.c_str();
  // A cube is a 2D array of six faces that can also be viewed as a cube (its default view).
  desc.Type = cube ? RESOURCE_DIM_TEX_CUBE : RESOURCE_DIM_TEX_2D_ARRAY;
  desc.Width = scale_.HostWidth(width);
  desc.Height = scale_.HostHeight(height);
  desc.ArraySize = slices;
  desc.Format = format;
  desc.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
  device_->CreateTexture(desc, nullptr, &texture.texture);
  if (!texture.texture) return nullptr;
  texture.array_view_ = texture.texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  if (!cube) {
    TextureViewDesc slice_0;
    slice_0.ViewType = TEXTURE_VIEW_SHADER_RESOURCE;
    slice_0.TextureDim = RESOURCE_DIM_TEX_2D;
    slice_0.FirstArraySlice = 0;
    slice_0.NumArraySlices = 1;
    texture.texture->CreateView(slice_0, &texture.slice_0_);
    texture.srv = texture.slice_0_;
  }
  texture.width = desc.Width;
  texture.height = desc.Height;
  texture.guest_width = width;
  texture.guest_height = height;
  texture.slices = slices;
  texture.guest_format = guest_format;
  texture.format = format;
  texture.cube = cube;
  texture.address = address;
  REXLOG_INFO("[native_x_renderer] new resolve target: {:#x} {}x{}x{}{} guest format {} ({}x{} on the host)", address,
              width, height, slices, cube ? " cube" : "", guest_format, desc.Width, desc.Height);
  return &texture;
}

ResolvedTexture* ResolvedTextures::Find(uint32_t address, uint32_t guest_format, uint32_t width, uint32_t height,
                                        uint32_t slices, bool cube) {
  ApplyInvalidations();
  auto it = textures_.find(Key{address, guest_format, width, height, cube ? 6 : slices, cube});
  return it != textures_.end() && it->second.texture ? &it->second : nullptr;
}

ResolvedTexture* ResolvedTextures::FindNewest(uint32_t address) {
  ApplyInvalidations();
  ResolvedTexture* newest = nullptr;
  for (auto it = textures_.lower_bound(Key{address, 0, 0, 0, 0, false});
       it != textures_.end() && std::get<0>(it->first) == address; ++it) {
    if (it->second.texture && (!newest || it->second.last_resolve > newest->last_resolve)) newest = &it->second;
  }
  return newest && !newest->invalidated ? newest : nullptr;
}

ResolvedTexture* ResolvedTextures::FindNewest(uint32_t address, bool cube) {
  ApplyInvalidations();
  ResolvedTexture* newest = nullptr;
  for (auto it = textures_.lower_bound(Key{address, 0, 0, 0, 0, false});
       it != textures_.end() && std::get<0>(it->first) == address; ++it) {
    if (it->second.texture && it->second.cube == cube &&
        (!newest || it->second.last_resolve > newest->last_resolve)) {
      newest = &it->second;
    }
  }
  return newest && !newest->invalidated ? newest : nullptr;
}

uint64_t TextureBytes(const TextureDesc& desc) {
  uint64_t bytes = 0;
  for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) bytes += GetMipLevelProperties(desc, mip).MipSize;
  return bytes * desc.GetArraySize() * std::max(desc.SampleCount, 1u);
}

namespace {

// Every 600 frames, the entries not used for `unused_frames` are freed (D52). DiligentCore keeps their
// memory until the GPU is done with them. Entries without a texture (an unsupported format) stay, so that
// their warning is not logged again.
template <typename Map>
Freed FreeUnused(Map& map, uint64_t& frame, uint64_t unused_frames) {
  Freed freed;
  if (++frame % 600 != 0) return freed;
  for (auto it = map.begin(); it != map.end();) {
    if (it->second.texture && it->second.last_used + unused_frames < frame) {
      ++freed.count;
      freed.bytes += TextureBytes(it->second.texture->GetDesc());
      it = map.erase(it);
    } else {
      ++it;
    }
  }
  return freed;
}

template <typename Map>
uint64_t Bytes(const Map& map) {
  uint64_t bytes = 0;
  for (const auto& [key, entry] : map) {
    if (entry.texture) bytes += TextureBytes(entry.texture->GetDesc());
  }
  return bytes;
}

}  // namespace

uint64_t RenderTargets::bytes() const { return Bytes(targets_); }

uint64_t ResolvedTextures::bytes() const {
  uint64_t bytes = Bytes(textures_);
  for (const auto& [key, entry] : textures_) {
    if (entry.packed_) bytes += TextureBytes(entry.packed_->GetDesc());
  }
  return bytes;
}

Freed RenderTargets::EndFrame(uint64_t unused_frames) { return FreeUnused(targets_, frame_, unused_frames); }

Freed ResolvedTextures::EndFrame(uint64_t unused_frames) {
  if ((frame_ + 1) % 600 == 0) {
    // What FreeUnused below frees, for the diagnosis of later reads there.
    for (const auto& [key, texture] : textures_) {
      if (texture.texture && texture.last_used + unused_frames < frame_ + 1 && texture.last_resolve_frame) {
        NoteGone(texture, true);
      }
    }
  }
  Freed freed = FreeUnused(textures_, frame_, unused_frames);
  if (freed.count) ++generation_;
  return freed;
}

void ResolvedTextures::NoteGone(const ResolvedTexture& texture, bool freed) {
  Gone& gone = gone_[texture.address];
  gone = {texture.guest_width, texture.guest_height, texture.guest_format, texture.last_resolve_frame, frame_, freed,
          false};
}

void ResolvedTextures::NoteGuestRead(uint32_t address) {
  auto it = gone_.find(address);
  if (it == gone_.end()) return;
  ++gone_reads_;
  Gone& gone = it->second;
  if (gone.logged || logged_gone_reads_ >= 32) return;
  gone.logged = true;
  ++logged_gone_reads_;
  REXLOG_INFO("[native_x_renderer] a draw reads guest memory at {:#x}, where a resolve wrote a {}x{} texture of guest "
              "format {}: {} {} frames ago, {} frames after its last resolve; it reads what guest memory holds there "
              "instead{}",
              address, gone.width, gone.height, gone.format, gone.freed ? "freed unused" : "invalidated",
              frame_ - gone.gone_frame, gone.gone_frame - gone.last_resolve_frame,
              logged_gone_reads_ == 32 ? " (not logged again)" : "");
}

}  // namespace native_x_renderer
