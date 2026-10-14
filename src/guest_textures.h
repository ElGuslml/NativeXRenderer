// XenosNative - guest textures: the SDK's TextureCache with DiligentCore textures (internal, D23).
//
// The SDK's TextureCache does what is backend-independent: texture keys from the fetch constants, the guest
// layout (tiling, packed mips, pitch), which memory to request, invalidation when the CPU writes the pages
// (through the guest memory mirror's watches), the LRU. This subclass gives it the host side, ported from
// the SDK's Vulkan backend:
//   - host formats: the D3D12 backend's table (DXGI-like formats are DiligentCore's), adjusted where
//     DiligentCore's Vulkan mapping differs from DXGI (see guest_textures.cpp);
//   - loading: the SDK's precompiled SPIR-V load shaders (untiling, endian swap, format conversion), run
//     as DiligentCore compute pipelines that read the guest memory mirror and write a scratch buffer, then
//     copied into the texture level by level.
// Textures that resolves wrote are not loaded: their host copies (targets.h) are bound instead. A texture
// made outdated whose guest data did not change is not loaded again (its data is hashed, D23).

#pragma once

#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/xenos.h>

#include <array>
#include <cstdint>
#include <memory>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

namespace xenos_native {

class GuestMemory;
class ResolvedTextures;

class GuestTextures final : public rex::graphics::TextureCache {
 public:
  GuestTextures(const rex::graphics::RegisterFile& registers, GuestMemory& memory, ResolvedTextures& resolved,
                Diligent::IRenderDevice* device, Diligent::IDeviceContext* context);
  ~GuestTextures() override;

  bool Initialize();

  // Before a draw binds its textures. The tracker does not report fetch constant writes one by one, so
  // every binding is checked again (cheap when the fetch constants did not change).
  void RequestTextures(uint32_t used_texture_mask) override;

  // The view a texture binding of the translated shaders samples (2D array, 3D or cube), or nullptr when
  // the fetch constant has no loadable texture, or (`other_dimension` set) has a texture of another
  // dimension: the translator binds a stacked and a 3D texture for the same fetch and picks one at run
  // time, so one of the two bindings never matches.
  Diligent::ITextureView* GetView(uint32_t fetch_constant, rex::graphics::xenos::FetchOpDimension dimension,
                                  bool is_signed, bool& other_dimension);

  // Once per frame: `frame` is the frame starting, `completed_frame` the last one the GPU finished (the
  // SDK's LRU destroys textures only after their last use completed).
  void BeginFrame(uint64_t frame, uint64_t completed_frame);

  // Since the start, for the log.
  struct Stats {
    uint64_t loads = 0, loaded_bytes = 0;
    uint64_t unchanged = 0;    // loads skipped: the guest data had not changed
    uint64_t unsupported = 0;  // textures not created: no host format or load shader
  };
  const Stats& stats() const { return stats_; }

 protected:
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(rex::graphics::xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(rex::graphics::xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool TryLoadTextureDataFromResolveCache(Texture& texture, bool load_base, bool load_mips) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base, bool load_mips) override;

 private:
  struct HostFormat {
    Diligent::TEXTURE_FORMAT format;
    LoadShaderIndex load_shader;
    bool block_compressed;  // the host keeps the guest's blocks
    uint32_t swizzle;       // guest components to host components
  };
  static const HostFormat kHostFormats[64];
  static const HostFormat& GetHostFormat(TextureKey key);

  class HostTexture;
  bool LoadFromMemory(HostTexture& texture, bool load_base, bool load_mips);

  struct LoadPipeline {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    Diligent::IShaderResourceVariable* dest = nullptr;
    Diligent::IShaderResourceVariable* source = nullptr;
    Diligent::IShaderResourceVariable* constants = nullptr;
  };

  GuestMemory& memory_;
  ResolvedTextures& resolved_;
  Diligent::IRenderDevice* device_;
  Diligent::IDeviceContext* context_;
  std::array<LoadPipeline, kLoadShaderCount> load_pipelines_;
  Stats stats_;
};

}  // namespace xenos_native
