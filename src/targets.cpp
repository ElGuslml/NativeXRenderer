// XenosNative - see targets.h.

#include "targets.h"

#include "internal.h"

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <set>
#include <string>

namespace xenos_native {

using namespace Diligent;
namespace xenos = rex::graphics::xenos;

GuestSurface ReadSurface(const uint8_t* base, uint32_t object, bool depth) {
  GuestSurface s;
  if (!object) return s;
  s.object = object;
  s.depth = depth;
  s.surface_info = internal::Load32(base, object + 24);
  s.info = internal::Load32(base, object + 28);
  uint32_t size = internal::Load32(base, object + 36);
  s.width = ((size >> 18) & 0x3FFF) + 1;
  s.height = ((size >> 3) & 0x7FFF) + 1;
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
  if (it != targets_.end()) return it->second.texture ? &it->second : nullptr;
  HostTarget& target = targets_[key];

  TEXTURE_FORMAT format = s.depth ? HostDepthFormat(guest_format) : HostColorFormat(guest_format);
  if (format == TEX_FORMAT_UNKNOWN) {
    REXLOG_WARN("[xenos_native] render target format {} not supported", guest_format);
    return nullptr;
  }
  TextureDesc desc;
  std::string name = std::string(s.depth ? "XenosNative depth " : "XenosNative color ") + std::to_string(s.width) +
                     "x" + std::to_string(s.height) + " base " + std::to_string(s.edram_base()) +
                     (reduced ? " reduced" : "");
  const RenderScale& scale = reduced ? reduced_scale_ : scale_;
  desc.Name = name.c_str();
  desc.Type = RESOURCE_DIM_TEX_2D;
  desc.Width = scale.HostWidth(s.width);
  desc.Height = scale.HostHeight(s.height);
  desc.Format = format;
  desc.SampleCount = s.samples();
  desc.BindFlags = BIND_SHADER_RESOURCE | (s.depth ? BIND_DEPTH_STENCIL : BIND_RENDER_TARGET);
  device_->CreateTexture(desc, nullptr, &target.texture);
  if (!target.texture) {
    REXLOG_ERROR("[xenos_native] could not create the render target {}", name);
    return nullptr;
  }
  target.view = target.texture->GetDefaultView(s.depth ? TEXTURE_VIEW_DEPTH_STENCIL : TEXTURE_VIEW_RENDER_TARGET);
  target.srv = target.texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  target.width = desc.Width;
  target.height = desc.Height;
  target.guest_width = s.width;
  target.guest_height = s.height;
  target.samples = s.samples();
  target.format = format;
  target.depth = s.depth;
  target.reduced = reduced;
  target.scale = scale;
  REXLOG_INFO("[xenos_native] new render target: {} (pitch {}, {}x MSAA, guest format {}; {}x{} on the host)", name,
              s.pitch(), s.samples(), guest_format, desc.Width, desc.Height);
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

ResolvedTexture* ResolvedTextures::GetOrCreate(uint32_t address, uint32_t guest_format, uint32_t width,
                                               uint32_t height, uint32_t slices, bool cube) {
  if (cube) slices = 6;
  Key key{address, guest_format, width, height, slices, cube};
  auto it = textures_.find(key);
  if (it != textures_.end()) return it->second.texture ? &it->second : nullptr;
  ResolvedTexture& texture = textures_[key];
  TEXTURE_FORMAT format = HostTextureFormat(guest_format);
  if (format == TEX_FORMAT_UNKNOWN) {
    REXLOG_WARN("[xenos_native] resolve to texture format {} not supported", guest_format);
    return nullptr;
  }
  TextureDesc desc;
  std::string name = std::string(cube ? "XenosNative resolve cube " : "XenosNative resolve ") +
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
  REXLOG_INFO("[xenos_native] new resolve target: {:#x} {}x{}x{}{} guest format {} ({}x{} on the host)", address,
              width, height, slices, cube ? " cube" : "", guest_format, desc.Width, desc.Height);
  return &texture;
}

ResolvedTexture* ResolvedTextures::Find(uint32_t address, uint32_t guest_format, uint32_t width, uint32_t height,
                                        uint32_t slices, bool cube) {
  auto it = textures_.find(Key{address, guest_format, width, height, cube ? 6 : slices, cube});
  return it != textures_.end() && it->second.texture ? &it->second : nullptr;
}

ResolvedTexture* ResolvedTextures::FindNewest(uint32_t address) {
  ResolvedTexture* newest = nullptr;
  for (auto it = textures_.lower_bound(Key{address, 0, 0, 0, 0, false});
       it != textures_.end() && std::get<0>(it->first) == address; ++it) {
    if (it->second.texture && (!newest || it->second.last_resolve > newest->last_resolve)) newest = &it->second;
  }
  return newest;
}

ResolvedTexture* ResolvedTextures::FindNewest(uint32_t address, bool cube) {
  ResolvedTexture* newest = nullptr;
  for (auto it = textures_.lower_bound(Key{address, 0, 0, 0, 0, false});
       it != textures_.end() && std::get<0>(it->first) == address; ++it) {
    if (it->second.texture && it->second.cube == cube &&
        (!newest || it->second.last_resolve > newest->last_resolve)) {
      newest = &it->second;
    }
  }
  return newest;
}

}  // namespace xenos_native
