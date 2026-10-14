// XenosNative - guest draws on DiligentCore (internal).
//
// A draw call (DrawIndexedVertices, DrawVertices...) returns; the state tracker holds the GPU state the
// draw runs with (D12). DrawPath draws it on the host, as the SDK's Vulkan command processor does in
// IssueDraw, with the SDK's own pieces where they are backend-independent:
//   - shaders: the last microcode loaded, translated to SPIR-V by the SDK (shaders.h);
//   - primitives and indices: the SDK's PrimitiveProcessor (primitives.h);
//   - vertex and index data: the guest memory mirror (guest_memory.h), uploaded as pages change;
//   - viewport, scissor, depth and color normalization, system constants: the SDK's draw utilities and a
//     port of UpdateSystemConstantValues;
//   - fixed-function state: a DiligentCore pipeline (pipelines.h);
//   - textures: textures.h.
// Render targets are the host targets of the surfaces the D3D device has bound (targets.h).

#pragma once

#include <rex/graphics/pipeline/shader/spirv_translator.h>

#include <array>
#include <cstdint>
#include <memory>

#include "Buffer.h"
#include "RefCntAutoPtr.hpp"

namespace rex::graphics {
class RegisterFile;
}  // namespace rex::graphics
namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace xenos_native {

class Device;
struct DrawContext;
class GuestMemory;
class PipelineCache;
class Primitives;
class ResolvedTextures;
class ShaderCache;
class StateTracker;
class Textures;
struct HostTarget;

class DrawPath {
 public:
  DrawPath();
  ~DrawPath();

  // `registers` is the state tracker's register file (it lives as long as the tracker).
  bool Initialize(Device& device, rex::graphics::RegisterFile& registers, rex::memory::Memory& memory,
                  ResolvedTextures& resolved);

  // Draws a finished draw call. `targets`: the host targets of the surfaces the device has bound (RT0-3,
  // then depth), null where none. `tiled`: inside BeginTiling/EndTiling, where the per-tile window
  // registers are not in the tracker; the draw covers the whole surface once.
  void Draw(const StateTracker& tracker, const uint8_t* base, const std::array<HostTarget*, 5>& targets, bool tiled);

  void EndFrame();

  struct Stats {
    uint32_t draws = 0, drawn = 0, no_targets = 0, failed = 0;
    uint32_t textures_resolved = 0, textures_placeholder = 0;  // texture bindings
    uint32_t copies = 0;  // buffer copies (guest memory pages, converted indices)
    uint64_t uploaded_bytes = 0;
  };
  Stats TakeStats();
  // Totals for the log.
  size_t shader_count() const;
  size_t pipeline_count() const;

 private:
  bool DrawInternal(const StateTracker& tracker, const uint8_t* base, const std::array<HostTarget*, 5>& targets);
  void FillSystemConstants(const DrawContext& draw);

  Device* device_ = nullptr;
  rex::graphics::RegisterFile* registers_ = nullptr;
  std::unique_ptr<GuestMemory> memory_;
  std::unique_ptr<Primitives> primitives_;
  std::unique_ptr<ShaderCache> shaders_;
  std::unique_ptr<PipelineCache> pipelines_;
  std::unique_ptr<Textures> textures_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> system_constants_buffer_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> bool_loop_constants_buffer_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> fetch_constants_buffer_;
  // Kept between draws, as the SDK does: some fields are written only when they apply.
  rex::graphics::SpirvShaderTranslator::SystemConstants system_constants_{};
  bool memory_in_read_state_ = false;
  Stats stats_;
};

}  // namespace xenos_native
