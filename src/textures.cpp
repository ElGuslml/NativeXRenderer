// XenosNative - see textures.h.

#include "textures.h"

#include "targets.h"

#include <rex/graphics/pipeline/texture/util.h>

#include <algorithm>
#include <vector>

namespace xenos_native {

using namespace Diligent;
namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;

namespace {

RefCntAutoPtr<ITexture> CreatePlaceholder(IRenderDevice* device, RESOURCE_DIMENSION type, const char* name) {
  TextureDesc desc;
  desc.Name = name;
  desc.Type = type;
  desc.Width = 1;
  desc.Height = 1;
  desc.ArraySize = type == RESOURCE_DIM_TEX_CUBE ? 6 : 1;  // Depth for the 3D texture
  desc.Format = TEX_FORMAT_RGBA8_UNORM;
  desc.Usage = USAGE_IMMUTABLE;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  const uint32_t white = 0xFFFFFFFFu;
  std::vector<TextureSubResData> data(desc.ArraySize, TextureSubResData{&white, 4, 4});
  TextureData initial{data.data(), uint32_t(data.size())};
  RefCntAutoPtr<ITexture> texture;
  device->CreateTexture(desc, &initial, &texture);
  return texture;
}

TEXTURE_ADDRESS_MODE AddressMode(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat:
      return TEXTURE_ADDRESS_WRAP;
    case xenos::ClampMode::kClampToEdge:
    case xenos::ClampMode::kClampToHalfway:
      return TEXTURE_ADDRESS_CLAMP;
    case xenos::ClampMode::kClampToBorder:
      return TEXTURE_ADDRESS_BORDER;
    default:
      // Mirrored modes. The mirror-once modes need samplerMirrorClampToEdge, which DiligentCore does not
      // enable: approximated with mirroring.
      return TEXTURE_ADDRESS_MIRROR;
  }
}

}  // namespace

bool Textures::Initialize(IRenderDevice* device, ResolvedTextures& resolved) {
  device_ = device;
  resolved_ = &resolved;
  placeholder_2d_ = CreatePlaceholder(device, RESOURCE_DIM_TEX_2D_ARRAY, "XenosNative placeholder 2D");
  placeholder_3d_ = CreatePlaceholder(device, RESOURCE_DIM_TEX_3D, "XenosNative placeholder 3D");
  placeholder_cube_ = CreatePlaceholder(device, RESOURCE_DIM_TEX_CUBE, "XenosNative placeholder cube");
  if (!placeholder_2d_ || !placeholder_3d_ || !placeholder_cube_) return false;
  placeholder_views_[0] = placeholder_2d_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  placeholder_views_[1] = placeholder_3d_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  placeholder_views_[2] = placeholder_cube_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  return true;
}

ITextureView* Textures::View(const xenos::xe_gpu_texture_fetch_t& fetch, xenos::FetchOpDimension dimension) {
  if (dimension == xenos::FetchOpDimension::k3DOrStacked) return placeholder_views_[1];
  if (dimension == xenos::FetchOpDimension::kCube) return placeholder_views_[2];
  if (fetch.type == xenos::FetchConstantType::kTexture) {
    if (ResolvedTexture* texture = resolved_->FindNewest(fetch.base_address << 12)) {
      if (ITextureView* view = texture->ArrayView()) {
        ++stats_.resolved;
        return view;
      }
    }
  }
  ++stats_.placeholders;
  return placeholder_views_[0];
}

ISampler* Textures::Sampler(const xenos::xe_gpu_texture_fetch_t& fetch,
                            const rex::graphics::SpirvShader::SamplerBinding& binding) {
  // As the SDK's texture caches derive it (GetSamplerParameters), without the host format checks.
  xenos::ClampMode clamp_x, clamp_y, clamp_z;
  texture_util::GetClampModesForDimension(fetch, clamp_x, clamp_y, clamp_z);
  auto pick = [](xenos::TextureFilter binding_filter, xenos::TextureFilter fetch_filter) {
    return binding_filter == xenos::TextureFilter::kUseFetchConst ? fetch_filter : binding_filter;
  };
  xenos::TextureFilter mag = pick(binding.mag_filter, fetch.mag_filter);
  xenos::TextureFilter min = pick(binding.min_filter, fetch.min_filter);
  xenos::TextureFilter mip = pick(binding.mip_filter, fetch.mip_filter);
  xenos::AnisoFilter aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst ? fetch.aniso_filter
                                                                                         : binding.aniso_filter;
  aniso = std::min(aniso, xenos::AnisoFilter::kMax_16_1);
  uint32_t mip_min_level = 0, mip_max_level = 0;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 &mip_min_level, &mip_max_level);
  bool base_map = mip == xenos::TextureFilter::kBaseMap;
  xenos::BorderColor border = fetch.border_color;

  uint64_t key = uint64_t(clamp_x) | uint64_t(clamp_y) << 3 | uint64_t(clamp_z) << 6 | uint64_t(mag) << 9 |
                 uint64_t(min) << 11 | uint64_t(mip) << 13 | uint64_t(aniso) << 15 | uint64_t(border) << 18 |
                 uint64_t(mip_min_level) << 20;
  auto it = samplers_.find(key);
  if (it != samplers_.end()) return it->second;

  SamplerDesc desc;
  desc.Name = "XenosNative sampler";
  if (aniso != xenos::AnisoFilter::kDisabled) {
    desc.MinFilter = desc.MagFilter = desc.MipFilter = FILTER_TYPE_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (uint32_t(aniso) - uint32_t(xenos::AnisoFilter::kMax_1_1));
  } else {
    desc.MagFilter = mag == xenos::TextureFilter::kLinear ? FILTER_TYPE_LINEAR : FILTER_TYPE_POINT;
    desc.MinFilter = min == xenos::TextureFilter::kLinear ? FILTER_TYPE_LINEAR : FILTER_TYPE_POINT;
    desc.MipFilter = mip == xenos::TextureFilter::kLinear ? FILTER_TYPE_LINEAR : FILTER_TYPE_POINT;
  }
  desc.AddressU = AddressMode(clamp_x);
  desc.AddressV = AddressMode(clamp_y);
  desc.AddressW = AddressMode(clamp_z);
  float white = border == xenos::BorderColor::k_ABGR_White ? 1.0f : 0.0f;
  desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = desc.BorderColor[3] = white;
  desc.MinLOD = float(mip_min_level);
  desc.MaxLOD = base_map ? desc.MinLOD + 0.25f : 1000.0f;
  RefCntAutoPtr<ISampler>& sampler = samplers_[key];
  device_->CreateSampler(desc, &sampler);
  return sampler;
}

uint32_t Textures::HostSwizzle(const xenos::xe_gpu_texture_fetch_t& fetch) {
  // TextureCache::GuestToHostSwizzle with an RGBA host format: constants (0, 1) stay constants.
  uint32_t host = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t component = (fetch.swizzle >> (3 * i)) & 0b111;
    if (component >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) component &= 0b101;
    host |= component << (3 * i);
  }
  return host;
}

}  // namespace xenos_native
