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
//   - textures: the SDK's TextureCache (guest_textures.h), resolved textures and placeholders (textures.h).
// Render targets are the host targets of the surfaces the D3D device has bound (targets.h).

#pragma once

#include <rex/graphics/pipeline/shader/spirv_translator.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "Buffer.h"
#include "internal.h"
#include "RefCntAutoPtr.hpp"
#include "TextureView.h"
#include "pipelines.h"
#include "scale.h"

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
class GuestTextures;
class PipelineCache;
class PipelineStorage;
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
  // `storage`: where the shaders and pipelines are kept between runs, a path without extension (D42);
  // empty for none.
  bool Initialize(Device& device, rex::graphics::RegisterFile& registers, rex::memory::Memory& memory,
                  ResolvedTextures& resolved, const RenderScale& scale, const std::filesystem::path& storage);

  // Draws a finished draw call. `targets`: the host targets of the surfaces the device has bound (RT0-3,
  // then depth), null where none. `tiled`: inside BeginTiling/EndTiling, where the per-tile window
  // registers are not in the tracker; the draw covers the whole surface once.
  void Draw(const StateTracker& tracker, const uint8_t* base, const std::array<HostTarget*, 5>& targets, bool tiled);

  // After the frame was presented.
  void EndFrame();
  // Something else bound render targets (a clear, a resolve, Present): the next draw binds its own again.
  void TargetsChanged() { bound_valid_ = false; }

  struct Stats {
    uint32_t draws = 0, drawn = 0, no_targets = 0, failed = 0;
    uint32_t translating = 0;  // skipped: a shader was still being translated (D34)
    uint32_t compiling = 0;    // skipped: their pipeline was still compiling (D28)
    // Texture bindings: resolved, guest, placeholders, and placeholders for the other dimension's binding.
    uint32_t textures_resolved = 0, textures_guest = 0, textures_placeholder = 0, textures_other_dimension = 0;
    uint32_t copies = 0;  // buffer copies (guest memory pages, converted indices)
    uint64_t uploaded_bytes = 0;
    uint64_t unchanged_bytes = 0;  // requested again, not copied: unchanged (D35)
    // Shader analyses and pipelines (render thread), translations with their host shaders (worker threads).
    internal::CreationTime analyses, translations, pipelines;
  };
  Stats TakeStats();
  // Totals for the log.
  size_t shader_count() const;
  size_t pipeline_count() const;
  // Guest texture loads since the start.
  uint64_t texture_loads() const;
  uint64_t texture_loaded_bytes() const;
  uint64_t texture_loads_unchanged() const;
  uint64_t textures_unsupported() const;
  // Guest textures on the host now: count and bytes.
  uint64_t texture_count() const;
  uint64_t texture_host_bytes() const;

 private:
  bool DrawInternal(const StateTracker& tracker, const uint8_t* base, const std::array<HostTarget*, 5>& targets);
  // The stored shaders and pipelines (D42): read, then the pipelines created as their shaders get ready.
  void OpenStorage(const std::filesystem::path& storage);
  void PrepareStored();
  void SavePipelineCache();
  void FillSystemConstants(const DrawContext& draw);

  Device* device_ = nullptr;
  rex::graphics::RegisterFile* registers_ = nullptr;
  RenderScale scale_;  // D37
  std::unique_ptr<GuestMemory> memory_;
  std::unique_ptr<GuestTextures> guest_textures_;  // after memory_: destroyed first
  std::unique_ptr<Primitives> primitives_;
  std::unique_ptr<ShaderCache> shaders_;
  std::unique_ptr<PipelineCache> pipelines_;
  std::unique_ptr<Textures> textures_;
  // Kept between runs (D42).
  std::unique_ptr<PipelineStorage> storage_;
  struct Stored {
    rex::graphics::SpirvShader* vertex_shader;
    uint64_t vertex_modification;
    rex::graphics::SpirvShader* pixel_shader;
    uint64_t pixel_modification;
    PipelineState state;
  };
  std::vector<Stored> stored_;  // stored pipelines not created yet
  uint32_t stored_total_ = 0;
  std::chrono::steady_clock::time_point stored_start_;
  Diligent::RefCntAutoPtr<Diligent::IPipelineStateCache> pipeline_cache_;
  std::filesystem::path pipeline_cache_path_;
  uint32_t pipelines_unsaved_ = 0;   // created since the pipeline cache was last saved
  uint32_t frames_since_created_ = 0;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> system_constants_buffer_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> bool_loop_constants_buffer_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> fetch_constants_buffer_;
  // Kept between draws, as the SDK does: some fields are written only when they apply.
  rex::graphics::SpirvShaderTranslator::SystemConstants system_constants_{};
  // The render targets the last draw bound (RT0-3, depth), so that a draw with the same ones does not
  // bind them again: DiligentCore puts a barrier after every transition out of a writable state, even to
  // the same state, and the barrier ends the render pass (D30).
  std::array<Diligent::ITextureView*, 5> bound_{};
  uint32_t bound_count_ = 0;
  bool bound_valid_ = false;
  Stats stats_;
};

}  // namespace xenos_native
