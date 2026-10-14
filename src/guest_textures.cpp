// XenosNative - see guest_textures.h.

#include "guest_textures.h"

#include "guest_memory.h"
#include "spirv_patch.h"
#include "targets.h"

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "GraphicsAccessories.hpp"
#include "Shader.h"

namespace xenos_native {

// The SDK's precompiled SPIR-V texture load shaders (generated with Xenia's `xb buildshaders`).
namespace load_shaders {
#include "vulkan_spirv/texture_load_128bpb_cs.h"
#include "vulkan_spirv/texture_load_16bpb_cs.h"
#include "vulkan_spirv/texture_load_32bpb_cs.h"
#include "vulkan_spirv/texture_load_64bpb_cs.h"
#include "vulkan_spirv/texture_load_8bpb_cs.h"
#include "vulkan_spirv/texture_load_bgrg8_rgb8_cs.h"
#include "vulkan_spirv/texture_load_ctx1_cs.h"
#include "vulkan_spirv/texture_load_depth_float_cs.h"
#include "vulkan_spirv/texture_load_depth_unorm_cs.h"
#include "vulkan_spirv/texture_load_dxn_rg8_cs.h"
#include "vulkan_spirv/texture_load_dxt1_rgba8_cs.h"
#include "vulkan_spirv/texture_load_dxt3_rgba8_cs.h"
#include "vulkan_spirv/texture_load_dxt3a_cs.h"
#include "vulkan_spirv/texture_load_dxt5_rgba8_cs.h"
#include "vulkan_spirv/texture_load_dxt5a_r8_cs.h"
#include "vulkan_spirv/texture_load_gbgr8_rgb8_cs.h"
#include "vulkan_spirv/texture_load_r10g11b11_rgba16_cs.h"
#include "vulkan_spirv/texture_load_r11g11b10_rgba16_cs.h"
#include "vulkan_spirv/texture_load_r4g4b4a4_rgba8_cs.h"
#include "vulkan_spirv/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "vulkan_spirv/texture_load_r5g6b5_rgba8_cs.h"
}  // namespace load_shaders

using namespace Diligent;
namespace rg = rex::graphics;
namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;

namespace {

using rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
using rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB;
using rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;
using rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;

// Swaps red and blue in a host format swizzle: for formats whose DiligentCore Vulkan format stores them in
// the opposite order to the DXGI format the load shader writes (RGB10A2_UNORM is A2R10G10B10 there).
constexpr uint32_t SwapRedBlue(uint32_t swizzle) {
  uint32_t result = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t component = (swizzle >> (3 * i)) & 0b111;
    if (component == 0) {
      component = 2;
    } else if (component == 2) {
      component = 0;
    }
    result |= component << (3 * i);
  }
  return result;
}

// Names for the load shaders' resources (their SPIR-V is stripped): set 0 is the destination, set 1 the
// source, both binding 0 (the SDK's Vulkan backend, kLoadDescriptorSetIndex*), plus the push constants.
const std::vector<spirv::ResourceName> kLoadShaderNames = {
    {0, 0, false, "XeTextureLoadDest"},
    {1, 0, false, "XeTextureLoadSource"},
    {0, 0, true, "XeTextureLoadConstants"},
};

}  // namespace

// Indexed by xenos::TextureFormat. The SDK's D3D12 table (DiligentCore's formats are DXGI's), unsigned
// versions only, with these changes:
//   - formats whose DiligentCore Vulkan mapping does not match the DXGI layout: 2_10_10_10 keeps the format
//     and swaps red and blue in the swizzle; 5_6_5 and 4_4_4_4 are expanded to RGBA8 (as the SDK's Vulkan
//     backend does when the 16-bit formats are unsupported); 1_5_5_5 and DXT3A_AS_1_1_1_1 are not
//     supported yet;
//   - Y'CbCr formats use their RGB8 decompression.
// DXT1 stays compressed as BC1 (D33): with XenosNative's DiligentCore patch 0002, BC1_UNORM is Vulkan's BC1
// with 1-bit alpha, as in Direct3D.
// Block-compressed formats fall back to decompression when their size is not a multiple of the block, or
// when the device has no BC formats (GetHostFormat).
const GuestTextures::HostFormat GuestTextures::kHostFormats[64] = {
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_1_REVERSE
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_1
    {TEX_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},    // k_8
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_1_5_5_5
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexR5G6B5ToRGBA8, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_5_6_5
    // k_6_5_5: green bits in blue, blue in green (DXGI B5G6R5 is Vulkan R5G6B5; DiligentCore's B5G6R5 is
    // Vulkan B5G6R5): red and blue swapped on top.
    {TEX_FORMAT_B5G6R5_UNORM, kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, false,
     SwapRedBlue(XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G))},
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_8_8_8_8
    {TEX_FORMAT_RGB10A2_UNORM, kLoadShaderIndex32bpb, false, SwapRedBlue(XE_GPU_TEXTURE_SWIZZLE_RGBA)},  // k_2_10_10_10
    {TEX_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},    // k_8_A
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_8_B
    {TEX_FORMAT_RG8_UNORM, kLoadShaderIndex16bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_8_8
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexGBGR8ToRGB8, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_Cr_Y1_Cb_Y0_REP
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexBGRG8ToRGB8, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_Y1_Cr_Y0_Cb_REP
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_16_16_EDRAM
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_8_8_8_8_A
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexRGBA4ToRGBA8, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_4_4_4_4
    {TEX_FORMAT_RGBA16_UNORM, kLoadShaderIndexR11G11B10ToRGBA16, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_10_11_11
    {TEX_FORMAT_RGBA16_UNORM, kLoadShaderIndexR10G11B11ToRGBA16, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_11_11_10
    {TEX_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT1
    {TEX_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT2_3
    {TEX_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT4_5
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_16_16_16_16_EDRAM
    {TEX_FORMAT_R32_FLOAT, kLoadShaderIndexDepthUnorm, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_24_8
    {TEX_FORMAT_R32_FLOAT, kLoadShaderIndexDepthFloat, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_24_8_FLOAT
    {TEX_FORMAT_R16_UNORM, kLoadShaderIndex16bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_16
    {TEX_FORMAT_RG16_UNORM, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_16_16
    {TEX_FORMAT_RGBA16_UNORM, kLoadShaderIndex64bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_16_16_16_16
    {TEX_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_16_EXPAND
    {TEX_FORMAT_RG16_FLOAT, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_16_16_EXPAND
    {TEX_FORMAT_RGBA16_FLOAT, kLoadShaderIndex64bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_16_16_16_16_EXPAND
    {TEX_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_16_FLOAT
    {TEX_FORMAT_RG16_FLOAT, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_16_16_FLOAT
    {TEX_FORMAT_RGBA16_FLOAT, kLoadShaderIndex64bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_16_16_16_16_FLOAT
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_32
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_32_32
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_32_32_32_32
    {TEX_FORMAT_R32_FLOAT, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_32_FLOAT
    {TEX_FORMAT_RG32_FLOAT, kLoadShaderIndex64bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_32_32_FLOAT
    {TEX_FORMAT_RGBA32_FLOAT, kLoadShaderIndex128bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_32_32_32_32_FLOAT
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_32_AS_8
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_32_AS_8_8
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_16_MPEG
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_16_16_MPEG
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_8_INTERLACED
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_32_AS_8_INTERLACED
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_32_AS_8_8_INTERLACED
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_16_INTERLACED
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_16_MPEG_INTERLACED
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_16_16_MPEG_INTERLACED
    {TEX_FORMAT_BC5_UNORM, kLoadShaderIndex128bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_DXN
    {TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndex32bpb, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_8_8_8_8_AS_16_16_16_16
    {TEX_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT1_AS_16_16_16_16
    {TEX_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT2_3_AS_16_16_16_16
    {TEX_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, true, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT4_5_AS_16_16_16_16
    {TEX_FORMAT_RGB10A2_UNORM, kLoadShaderIndex32bpb, false, SwapRedBlue(XE_GPU_TEXTURE_SWIZZLE_RGBA)},  // k_2_10_10_10_AS_16_16_16_16
    {TEX_FORMAT_RGBA16_UNORM, kLoadShaderIndexR11G11B10ToRGBA16, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_10_11_11_AS_16_16_16_16
    {TEX_FORMAT_RGBA16_UNORM, kLoadShaderIndexR10G11B11ToRGBA16, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_11_11_10_AS_16_16_16_16
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBB},  // k_32_32_32_FLOAT
    {TEX_FORMAT_R8_UNORM, kLoadShaderIndexDXT3A, false, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_DXT3A
    {TEX_FORMAT_BC4_UNORM, kLoadShaderIndex64bpb, true, XE_GPU_TEXTURE_SWIZZLE_RRRR},  // k_DXT5A
    {TEX_FORMAT_RG8_UNORM, kLoadShaderIndexCTX1, false, XE_GPU_TEXTURE_SWIZZLE_RGGG},  // k_CTX1
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_DXT3A_AS_1_1_1_1
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_8_8_8_8_GAMMA_EDRAM
    {TEX_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, XE_GPU_TEXTURE_SWIZZLE_RGBA},  // k_2_10_10_10_FLOAT_EDRAM
};

const GuestTextures::HostFormat& GuestTextures::GetHostFormat(TextureKey key) const {
  const HostFormat& host_format = kHostFormats[uint32_t(key.format)];
  if (!host_format.block_compressed) return host_format;
  // Decompressed when the size is not a multiple of the block (as the SDK's D3D12 backend does), or without
  // BC formats on the device (as its Vulkan backend does).
  const rg::FormatInfo* info = rg::FormatInfo::Get(key.format);
  if (block_compression_ && !(key.GetWidth() & (info->block_width - 1)) &&
      !(key.GetHeight() & (info->block_height - 1))) {
    return host_format;
  }
  static const HostFormat kDXT1{TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexDXT1ToRGBA8, false,
                               XE_GPU_TEXTURE_SWIZZLE_RGBA};
  static const HostFormat kDXT2_3{TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexDXT3ToRGBA8, false,
                                  XE_GPU_TEXTURE_SWIZZLE_RGBA};
  static const HostFormat kDXT4_5{TEX_FORMAT_RGBA8_UNORM, kLoadShaderIndexDXT5ToRGBA8, false,
                                  XE_GPU_TEXTURE_SWIZZLE_RGBA};
  static const HostFormat kDXN{TEX_FORMAT_RG8_UNORM, kLoadShaderIndexDXNToRG8, false, XE_GPU_TEXTURE_SWIZZLE_RGGG};
  static const HostFormat kDXT5A{TEX_FORMAT_R8_UNORM, kLoadShaderIndexDXT5AToR8, false,
                                 XE_GPU_TEXTURE_SWIZZLE_RRRR};
  switch (key.format) {
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      return kDXT1;
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return kDXT2_3;
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return kDXT4_5;
    case xenos::TextureFormat::k_DXN:
      return kDXN;
    case xenos::TextureFormat::k_DXT5A:
      return kDXT5A;
    default:
      return host_format;
  }
}

// One guest texture on the host: a 2D array (1D, 2D, stacked), 3D or cube texture with its mips.
class GuestTextures::HostTexture final : public rg::TextureCache::Texture {
 public:
  HostTexture(GuestTextures& cache, const TextureKey& key, RefCntAutoPtr<ITexture> texture, uint64_t size)
      : Texture(cache, key), texture_(std::move(texture)) {
    view_ = texture_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    SetHostMemoryUsage(size);
    ++cache.stats_.textures;
    cache.stats_.host_bytes += size;
  }
  ~HostTexture() override {
    auto& stats = static_cast<GuestTextures&>(texture_cache()).stats_;
    --stats.textures;
    stats.host_bytes -= GetHostMemoryUsage();
  }
  ITexture* texture() const { return texture_; }
  ITextureView* view() const { return view_; }

  // The guest data of the last load.
  std::optional<uint64_t> base_hash, mips_hash;

 private:
  RefCntAutoPtr<ITexture> texture_;
  ITextureView* view_ = nullptr;
};

GuestTextures::GuestTextures(const rg::RegisterFile& registers, GuestMemory& memory, ResolvedTextures& resolved,
                             IRenderDevice* device, IDeviceContext* context)
    : TextureCache(registers, memory, 1, 1),
      memory_(memory),
      resolved_(resolved),
      device_(device),
      context_(context) {}

GuestTextures::~GuestTextures() { DestroyAllTextures(true); }

bool GuestTextures::Initialize() {
  block_compression_ = device_->GetDeviceInfo().Features.TextureCompressionBC == DEVICE_FEATURE_STATE_ENABLED;
  if (!block_compression_) REXLOG_WARN("[xenos_native] no BC texture formats: compressed textures are decompressed");
  struct Code {
    LoadShaderIndex index;
    const uint32_t* words;
    size_t bytes;
    const char* name;
  };
#define XN_LOAD_SHADER(index, array) {index, load_shaders::array, sizeof(load_shaders::array), #array}
  const Code codes[] = {
      XN_LOAD_SHADER(kLoadShaderIndex8bpb, texture_load_8bpb_cs),
      XN_LOAD_SHADER(kLoadShaderIndex16bpb, texture_load_16bpb_cs),
      XN_LOAD_SHADER(kLoadShaderIndex32bpb, texture_load_32bpb_cs),
      XN_LOAD_SHADER(kLoadShaderIndex64bpb, texture_load_64bpb_cs),
      XN_LOAD_SHADER(kLoadShaderIndex128bpb, texture_load_128bpb_cs),
      XN_LOAD_SHADER(kLoadShaderIndexR5G6B5ToRGBA8, texture_load_r5g6b5_rgba8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs),
      XN_LOAD_SHADER(kLoadShaderIndexRGBA4ToRGBA8, texture_load_r4g4b4a4_rgba8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexGBGR8ToRGB8, texture_load_gbgr8_rgb8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexBGRG8ToRGB8, texture_load_bgrg8_rgb8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexR10G11B11ToRGBA16, texture_load_r10g11b11_rgba16_cs),
      XN_LOAD_SHADER(kLoadShaderIndexR11G11B10ToRGBA16, texture_load_r11g11b10_rgba16_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXT1ToRGBA8, texture_load_dxt1_rgba8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXT3ToRGBA8, texture_load_dxt3_rgba8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXT5ToRGBA8, texture_load_dxt5_rgba8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXNToRG8, texture_load_dxn_rg8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXT3A, texture_load_dxt3a_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDXT5AToR8, texture_load_dxt5a_r8_cs),
      XN_LOAD_SHADER(kLoadShaderIndexCTX1, texture_load_ctx1_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDepthUnorm, texture_load_depth_unorm_cs),
      XN_LOAD_SHADER(kLoadShaderIndexDepthFloat, texture_load_depth_float_cs),
  };
#undef XN_LOAD_SHADER
  for (const Code& code : codes) {
    std::vector<uint32_t> spirv = spirv::PrepareForDiligent(code.words, code.bytes / 4, kLoadShaderNames);
    ShaderCreateInfo shader_info;
    shader_info.Desc.Name = code.name;
    shader_info.Desc.ShaderType = SHADER_TYPE_COMPUTE;
    shader_info.EntryPoint = "main";
    shader_info.ByteCode = spirv.data();
    shader_info.ByteCodeSize = spirv.size() * 4;
    RefCntAutoPtr<IShader> shader;
    device_->CreateShader(shader_info, &shader);
    if (!shader) {
      REXLOG_ERROR("[xenos_native] texture load shader {}: DiligentCore did not accept it", code.name);
      continue;
    }
    ComputePipelineStateCreateInfo pso_info;
    std::string pso_name = std::string("XenosNative ") + code.name;
    pso_info.PSODesc.Name = pso_name.c_str();
    pso_info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    pso_info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    pso_info.pCS = shader;
    LoadPipeline& pipeline = load_pipelines_[code.index];
    device_->CreateComputePipelineState(pso_info, &pipeline.pso);
    if (!pipeline.pso) {
      REXLOG_ERROR("[xenos_native] texture load pipeline {}: DiligentCore did not create it", code.name);
      continue;
    }
    pipeline.pso->CreateShaderResourceBinding(&pipeline.srb, true);
    pipeline.dest = pipeline.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "XeTextureLoadDest");
    pipeline.source = pipeline.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "XeTextureLoadSource");
    pipeline.constants = pipeline.srb->GetVariableByName(SHADER_TYPE_COMPUTE, "XeTextureLoadConstants");
    if (!pipeline.dest || !pipeline.source || !pipeline.constants) {
      REXLOG_ERROR("[xenos_native] texture load pipeline {}: resources not found by name", code.name);
      pipeline = {};
    }
  }
  return true;
}

void GuestTextures::RequestTextures(uint32_t used_texture_mask) {
  TextureFetchConstantsWritten(0, 31);
  TextureCache::RequestTextures(used_texture_mask);
}

ITextureView* GuestTextures::GetView(uint32_t fetch_constant, xenos::FetchOpDimension dimension, bool is_signed,
                                     bool& other_dimension) {
  other_dimension = false;
  (void)is_signed;  // signed formats are not loaded separately yet: the unsigned data is sampled
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant);
  if (!binding || !binding->texture) return nullptr;
  xenos::DataDimension data = binding->key.dimension;
  bool compatible;
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      compatible = data == xenos::DataDimension::k3D;
      break;
    case xenos::FetchOpDimension::kCube:
      compatible = data == xenos::DataDimension::kCube;
      break;
    default:  // 1D and 2D bindings sample 2D arrays
      compatible = data == xenos::DataDimension::k1D || data == xenos::DataDimension::k2DOrStacked;
      break;
  }
  if (!compatible) {
    other_dimension = true;
    return nullptr;
  }
  auto* texture = static_cast<HostTexture*>(binding->texture);
  texture->MarkAsUsed();
  return texture->view();
}

void GuestTextures::BeginFrame(uint64_t frame, uint64_t completed_frame) {
  CompletedSubmissionUpdated(completed_frame);
  BeginSubmission(frame);
  TextureCache::BeginFrame();
}

uint32_t GuestTextures::GetHostFormatSwizzle(TextureKey key) const { return GetHostFormat(key).swizzle; }

uint32_t GuestTextures::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  return dimension == xenos::DataDimension::k3D ? 2048 : 16384;
}

uint32_t GuestTextures::GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const {
  return dimension == xenos::DataDimension::kCube ? 6 : 2048;
}

std::unique_ptr<rg::TextureCache::Texture> GuestTextures::CreateTexture(TextureKey key) {
  const HostFormat& host_format = GetHostFormat(key);
  if (host_format.format == TEX_FORMAT_UNKNOWN || host_format.load_shader == kLoadShaderIndexUnknown ||
      !load_pipelines_[host_format.load_shader].pso) {
    ++stats_.unsupported;
    return nullptr;
  }
  TextureDesc desc;
  std::string name = "XenosNative texture " + std::to_string(key.base_page << 12) + " format " +
                     std::to_string(uint32_t(key.format));
  desc.Name = name.c_str();
  uint32_t depth_or_array_size = key.GetDepthOrArraySize();
  switch (key.dimension) {
    case xenos::DataDimension::k3D:
      desc.Type = RESOURCE_DIM_TEX_3D;
      desc.Depth = depth_or_array_size;
      break;
    case xenos::DataDimension::kCube:
      desc.Type = RESOURCE_DIM_TEX_CUBE;
      desc.ArraySize = 6;
      break;
    default:
      desc.Type = RESOURCE_DIM_TEX_2D_ARRAY;
      desc.ArraySize = depth_or_array_size;
      break;
  }
  desc.Width = key.GetWidth();
  desc.Height = key.GetHeight();
  desc.MipLevels = key.mip_max_level + 1;
  desc.Format = host_format.format;
  desc.Usage = USAGE_DEFAULT;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  RefCntAutoPtr<ITexture> texture;
  device_->CreateTexture(desc, nullptr, &texture);
  if (!texture) {
    REXLOG_ERROR("[xenos_native] could not create {} ({}x{}x{}, {} mips)", name, desc.Width, desc.Height,
                 depth_or_array_size, desc.MipLevels);
    return nullptr;
  }
  // The host size, mips included: the SDK's cache destroys unused textures above its memory limits.
  uint64_t size = 0;
  for (uint32_t level = 0; level < desc.MipLevels; ++level) size += GetMipLevelProperties(desc, level).MipSize;
  size *= desc.GetArraySize();
  return std::make_unique<HostTexture>(*this, key, std::move(texture), size);
}

bool GuestTextures::TryLoadTextureDataFromResolveCache(Texture& texture, bool load_base, bool load_mips) {
  (void)load_mips;
  // Resolve destinations hold no data in guest memory (resolves write host textures only): skip the load.
  // GetView's callers bind the resolved texture instead.
  return load_base && resolved_.FindNewest(texture.key().base_page << 12) != nullptr;
}

bool GuestTextures::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base, bool load_mips) {
  // SharedMemory invalidates a whole 256 KB block around a CPU write (fewer access violations to catch), so
  // CPU data that a game keeps next to a texture and writes every frame makes the texture outdated with its
  // data intact. What did not change is not loaded again.
  auto& host_texture = static_cast<HostTexture&>(texture);
  TextureKey key = host_texture.key();
  auto hash = [&](uint32_t page, uint32_t size) {
    return XXH3_64bits(memory_.memory().TranslatePhysical<const uint8_t*>(page << 12), size);
  };
  uint64_t base_hash = 0, mips_hash = 0;
  if (load_base) {
    base_hash = hash(key.base_page, host_texture.GetGuestBaseSize());
    load_base = host_texture.base_hash != base_hash;
  }
  if (load_mips) {
    mips_hash = hash(key.mip_page, host_texture.GetGuestMipsSize());
    load_mips = host_texture.mips_hash != mips_hash;
  }
  if (!load_base && !load_mips) {
    ++stats_.unchanged;
    return true;
  }
  if (!LoadFromMemory(host_texture, load_base, load_mips)) return false;
  if (load_base) host_texture.base_hash = base_hash;
  if (load_mips) host_texture.mips_hash = mips_hash;
  return true;
}

// VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl, at resolution scale 1, on DiligentCore: the load
// shader reads the guest memory mirror and writes the host layout into a scratch buffer, which is then
// copied into the texture level by level.
bool GuestTextures::LoadFromMemory(HostTexture& host_texture, bool load_base, bool load_mips) {
  TextureKey key = host_texture.key();
  const HostFormat& host_format = GetHostFormat(key);
  LoadShaderIndex load_shader = host_format.load_shader;
  if (load_shader == kLoadShaderIndexUnknown) return false;
  LoadPipeline& pipeline = load_pipelines_[load_shader];
  if (!pipeline.pso) return false;
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(load_shader);

  const texture_util::TextureGuestLayout& guest_layout = host_texture.guest_layout();
  bool is_3d = key.dimension == xenos::DataDimension::k3D;
  uint32_t width = key.GetWidth(), height = key.GetHeight();
  uint32_t depth_or_array_size = key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  const rg::FormatInfo* guest_format_info = rg::FormatInfo::Get(key.format);
  uint32_t block_width = guest_format_info->block_width, block_height = guest_format_info->block_height;
  uint32_t bytes_per_block = guest_format_info->bytes_per_block();
  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? key.mip_max_level : 0;
  if (level_first > level_last) return true;
  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);
  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  // The host layout of the scratch buffer.
  uint32_t host_block_width = host_format.block_compressed ? block_width : 1;
  uint32_t host_block_height = host_format.block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread = UINT32_C(1) << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_format.block_compressed) host_x_blocks_per_thread *= block_width;
  struct HostLayout {
    uint64_t offset_bytes, slice_size_bytes;
    uint32_t x_pitch_blocks, y_pitch_blocks;
  };
  HostLayout host_layout_base{};
  HostLayout host_layout_mips[xenos::kTextureMaxMips]{};
  uint64_t host_buffer_size = 0;
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = level_packed == 0 ? 0 : loop_level;
    HostLayout& level_host_layout = is_base ? host_layout_base : host_layout_mips[level];
    level_host_layout.offset_bytes = host_buffer_size;
    uint32_t level_x_texels, level_y_texels, level_z_texels;
    if (level == level_packed) {
      const texture_util::TextureGuestLayout::Level& packed = is_base ? guest_layout.base : guest_layout.mips[level];
      level_x_texels = packed.x_extent_blocks * block_width;
      level_y_texels = packed.y_extent_blocks * block_height;
      level_z_texels = packed.z_extent;
    } else {
      level_x_texels = std::max(width >> level, UINT32_C(1));
      level_y_texels = std::max(height >> level, UINT32_C(1));
      level_z_texels = std::max(depth >> level, UINT32_C(1));
    }
    level_host_layout.x_pitch_blocks =
        rex::round_up((level_x_texels + (host_block_width - 1)) / host_block_width, host_x_blocks_per_thread);
    level_host_layout.y_pitch_blocks = (level_y_texels + (host_block_height - 1)) / host_block_height;
    level_host_layout.slice_size_bytes = uint64_t(load_shader_info.bytes_per_host_block) *
                                         level_host_layout.x_pitch_blocks * level_host_layout.y_pitch_blocks *
                                         level_z_texels;
    host_buffer_size += level_host_layout.slice_size_bytes * array_size;
  }
  if (!host_buffer_size) return true;

  BufferDesc scratch_desc;
  scratch_desc.Name = "XenosNative texture load";
  scratch_desc.Size = rex::align(host_buffer_size, uint64_t(16));
  scratch_desc.Usage = USAGE_DEFAULT;
  scratch_desc.BindFlags = BIND_UNORDERED_ACCESS;
  scratch_desc.Mode = BUFFER_MODE_RAW;
  RefCntAutoPtr<IBuffer> scratch;
  device_->CreateBuffer(scratch_desc, nullptr, &scratch);
  if (!scratch) return false;

  // The guest data: views of the guest memory mirror at the base and mip pages.
  uint64_t source_alignment = UINT64_C(1) << load_shader_info.source_bpe_log2;
  auto make_source_view = [&](uint32_t start, uint32_t size) -> RefCntAutoPtr<IBufferView> {
    uint64_t range = rex::align(uint64_t(size), std::max(source_alignment, uint64_t(16)));
    if (!size || range > uint64_t(rg::SharedMemory::kBufferSize) - start) return {};
    BufferViewDesc view_desc;
    view_desc.ViewType = BUFFER_VIEW_SHADER_RESOURCE;
    view_desc.ByteOffset = start;
    view_desc.ByteWidth = range;
    RefCntAutoPtr<IBufferView> view;
    memory_.buffer()->CreateView(view_desc, &view);
    return view;
  };
  RefCntAutoPtr<IBufferView> source_base, source_mips;
  if (level_first == 0) {
    source_base = make_source_view(key.base_page << 12, host_texture.GetGuestBaseSize());
    if (!source_base) return false;
  }
  if (level_last != 0) {
    source_mips = make_source_view(key.mip_page << 12, host_texture.GetGuestMipsSize());
    if (!source_mips) return false;
  }
  memory_.PrepareForRead(context_);

  context_->SetPipelineState(pipeline.pso);
  pipeline.dest->Set(scratch->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
  LoadConstants constants{};
  constants.is_tiled_3d_endian_scale = uint32_t(key.tiled) | (uint32_t(is_3d) << 1) |
                                       (uint32_t(key.endianness) << 2) | (UINT32_C(1) << 4) | (UINT32_C(1) << 7);
  uint32_t guest_x_blocks_per_group_log2 = load_shader_info.GetGuestXBlocksPerGroupLog2();
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = level_packed == 0 ? 0 : loop_level;
    pipeline.source->Set(is_base ? source_base : source_mips);
    constants.guest_offset = is_base ? 0 : guest_layout.mip_offsets_bytes[level];
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    uint32_t level_guest_pitch = level_guest_layout.row_pitch_bytes;
    if (key.tiled) level_guest_pitch /= bytes_per_block;
    constants.guest_pitch_aligned = level_guest_pitch;
    constants.guest_z_stride_block_rows_aligned = level_guest_layout.z_slice_stride_block_rows;
    uint32_t level_width, level_height, level_depth;
    if (level == level_packed) {
      level_width = level_guest_layout.x_extent_blocks * block_width;
      level_height = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width = std::max(width >> level, UINT32_C(1));
      level_height = std::max(height >> level, UINT32_C(1));
      level_depth = std::max(depth >> level, UINT32_C(1));
    }
    constants.size_blocks[0] = (level_width + (block_width - 1)) / block_width;
    constants.size_blocks[1] = (level_height + (block_height - 1)) / block_height;
    constants.size_blocks[2] = level_depth;
    constants.height_texels = level_height;
    uint32_t group_count_x = (constants.size_blocks[0] + ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
                             guest_x_blocks_per_group_log2;
    uint32_t group_count_y = (constants.size_blocks[1] + ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
                             kLoadGuestYBlocksPerGroupLog2;
    const HostLayout& level_host_layout = is_base ? host_layout_base : host_layout_mips[level];
    constants.host_offset = uint32_t(level_host_layout.offset_bytes);
    constants.host_pitch = load_shader_info.bytes_per_host_block * level_host_layout.x_pitch_blocks;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      pipeline.constants->SetInlineConstants(&constants, 0, sizeof(constants) / 4);
      context_->CommitShaderResources(pipeline.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      DispatchComputeAttribs dispatch{group_count_x, group_count_y, constants.size_blocks[2]};
      context_->DispatchCompute(dispatch);
      constants.guest_offset += level_guest_layout.array_slice_stride_bytes;
      constants.host_offset += uint32_t(level_host_layout.slice_size_bytes);
    }
  }

  // Into the texture, level by level and slice by slice (packed mips are inside the packed level's data).
  for (uint32_t level = level_first; level <= level_last; ++level) {
    const HostLayout& level_host_layout = level != 0 ? host_layout_mips[std::min(level, level_packed)] : host_layout_base;
    uint64_t offset = level_host_layout.offset_bytes;
    if (level >= level_packed) {
      uint32_t offset_x, offset_y, offset_z;
      texture_util::GetPackedMipOffset(width, height, depth, key.format, level, offset_x, offset_y, offset_z);
      if (!host_format.block_compressed) {
        offset_x *= block_width;
        offset_y *= block_height;
      }
      offset += uint64_t(load_shader_info.bytes_per_host_block) *
                (offset_x + uint64_t(level_host_layout.x_pitch_blocks) *
                                (offset_y + uint64_t(level_host_layout.y_pitch_blocks) * offset_z));
    }
    uint32_t row_pitch = load_shader_info.bytes_per_host_block * level_host_layout.x_pitch_blocks;
    // DiligentCore copies with a tightly packed image height (bufferImageHeight 0), while a packed level's
    // rows are its whole extent: 3D levels are copied one depth slice at a time.
    uint64_t depth_stride = uint64_t(row_pitch) * level_host_layout.y_pitch_blocks;
    Box box;
    box.MaxX = std::max(width >> level, UINT32_C(1));
    box.MaxY = std::max(height >> level, UINT32_C(1));
    uint32_t level_depth = std::max(depth >> level, UINT32_C(1));
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      for (uint32_t z = 0; z < level_depth; ++z) {
        box.MinZ = z;
        box.MaxZ = z + 1;
        TextureSubResData data;
        data.pSrcBuffer = scratch;
        data.SrcOffset = offset + level_host_layout.slice_size_bytes * slice + depth_stride * z;
        data.Stride = row_pitch;
        data.DepthStride = depth_stride;
        context_->UpdateTexture(host_texture.texture(), level, slice, box, data,
                                RESOURCE_STATE_TRANSITION_MODE_TRANSITION, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      }
    }
  }
  ++stats_.loads;
  stats_.loaded_bytes += host_buffer_size;
  return true;
}

}  // namespace xenos_native
