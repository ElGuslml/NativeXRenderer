// NativeXRenderer - guest draws on DiligentCore (internal).
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
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "internal.h"
#include "RefCntAutoPtr.hpp"
#include "TextureView.h"
#include "motion.h"
#include "pipelines.h"
#include "scale.h"

namespace rex::graphics {
class RegisterFile;
}  // namespace rex::graphics
namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace native_x_renderer {

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
  // The next draws are a 2D layer kept at the game's aspect ratio (native_x_renderer::SetGameAspectBox, D43).
  void SetAspectBox(bool on) { aspect_box_ = on; }
  // The next draws are the main 3D view's (D47): those into targets of this guest size are shifted by the
  // jitter, in host pixels.
  // The depth range of the last jittered draw's viewport (z of the near plane, of the far plane): reversed
  // when near is above far, as in games that clear depth to 0.
  float jittered_z_min() const { return jittered_z_[0]; }
  float jittered_z_max() const { return jittered_z_[1]; }
  void SetJitter(bool on, float x, float y, uint32_t guest_width, uint32_t guest_height) {
    jitter_on_ = on;
    jitter_[0] = x;
    jitter_[1] = y;
    jitter_size_[0] = guest_width;
    jitter_size_[1] = guest_height;
  }
  // The motion of objects that move on their own (D48, motion.h): the jittered draws that write depth are
  // matched with the last frame's, and those that moved are kept for DrawObjectMotion.
  void SetObjectMotion(bool on) { object_motion_on_ = on; }
  // Draws the motion of the kept draws into `motion` (kMotionVectorFormat), where their depth equals
  // `depth`'s (the depth-stencil view of the main view's depth target), over the camera's motion. Returns
  // how many were drawn. Binds its own targets (the next draw binds its own again).
  uint32_t DrawObjectMotion(Diligent::ITextureView* motion, Diligent::ITextureView* depth);

  struct Stats {
    uint32_t draws = 0, drawn = 0, no_targets = 0, failed = 0;
    uint32_t translating = 0;  // skipped: a shader was still being translated (D34)
    uint32_t compiling = 0;    // skipped: their pipeline was still compiling (D28)
    // Texture bindings: resolved, guest, placeholders, and placeholders for the other dimension's binding.
    uint32_t textures_resolved = 0, textures_guest = 0, textures_placeholder = 0, textures_other_dimension = 0;
    uint32_t copies = 0;  // buffer copies (guest memory pages, converted indices)
    // Object motion (D48): main view draws matched with the last frame's, of those the ones that moved,
    // and their motion draws drawn.
    uint32_t motion_matched = 0, motion_moved = 0, motion_drawn = 0;
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
  // Keeps a draw that moved for DrawObjectMotion (D48).
  struct MotionDraw;
  void KeepMotionDraw(rex::graphics::SpirvShader& vertex_shader, uint64_t vertex_modification,
                      const PipelineState& state, const uint8_t* previous_constants,
                      const Diligent::Viewport& viewport, const Diligent::Rect& scissor,
                      Diligent::IBuffer* index_buffer, uint64_t index_offset, Diligent::VALUE_TYPE index_type,
                      uint32_t vertex_count);

  Device* device_ = nullptr;
  rex::graphics::RegisterFile* registers_ = nullptr;
  RenderScale scale_;  // D37
  bool aspect_box_ = false;  // D43
  bool jitter_on_ = false;   // D47
  float jitter_[2] = {};
  uint32_t jitter_size_[2] = {};
  float jittered_z_[2] = {0.0f, 1.0f};
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
  uint32_t frames_since_saved_ = UINT32_MAX / 2;  // the first save does not wait (D52)
  // The pipeline cache is saved on a thread of its own: tens of MB that the driver serializes and that are
  // written to disk took 30-80 ms on the render thread (D45).
  std::thread save_thread_;
  std::atomic<bool> saving_{false};
  std::atomic<bool> save_failed_{false};
  Diligent::RefCntAutoPtr<Diligent::IBuffer> system_constants_buffer_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> bool_loop_constants_buffer_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> fetch_constants_buffer_;
  // Object motion (D48).
  bool object_motion_on_ = false;
  ObjectMotion object_motion_;
  Diligent::RefCntAutoPtr<Diligent::IShader> motion_pixel_shader_;  // created on first use
  bool motion_pixel_shader_failed_ = false;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> motion_constants_buffer_;
  std::vector<uint8_t> vertex_constants_;  // this draw's vertex shader float constants, packed
  std::vector<uint8_t> vertex_registers_;  // the registers they come from
  std::vector<MotionDraw> motion_draws_;
  std::vector<uint8_t> motion_data_;  // their constants
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

}  // namespace native_x_renderer
