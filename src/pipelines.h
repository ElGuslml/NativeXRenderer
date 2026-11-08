// NativeXRenderer - DiligentCore pipelines for guest draws (internal).
//
// A pipeline is the pair of translated shaders plus the fixed-function state a guest draw sets: render
// target formats and sample count, topology, rasterizer, depth and stencil, blending. DiligentCore has no
// dynamic depth bias and one stencil read and write mask for both faces, so those are part of the state
// too. The state is plain bytes, hashed and compared as memory.
//
// Resources are bound by name. DiligentCore names uniform and storage buffers by their block type, not
// their variable:
//   - XeSharedMemory: the guest memory mirror (set 0);
//   - XeSystemConstants, XeBoolLoopConstants, XeFetchConstants: one buffer each, shared by both stages;
//   - XeFloatConstants: one per stage (the vertex shader's c0-c255, the pixel shader's c256-c511), so
//     the two are separate variables;
//   - xe_texture{N}_{2d|3d|cube}_{u|s} and xe_sampler{N}_...: per draw.
// Pipelines are compiled asynchronously by DiligentCore's thread pool (D28): a draw whose pipeline is still
// compiling is skipped, as the SDK's backends do with asynchronous pipelines, instead of stalling the frame
// (a new pipeline the driver has not cached takes up to 200 ms).
// The buffers are dynamic buffers, mapped per draw, so they are static variables, set once per pipeline:
// the guest memory and the system, bool/loop and fetch constants are shared by every pipeline; each
// pipeline has its own float constant buffers, the size of its shaders' constants (DiligentCore maps a
// dynamic buffer whole). Textures and samplers are dynamic variables, in the order of the shader's bindings,
// set by a draw when their object changes (D82).
//
// With native_x_renderer_binding_cache (D111) a pipeline's textures and samplers are mutable variables instead,
// and each set of them it draws with has a shader resource binding of its own, its variables set once: a draw
// with a set the pipeline drew with before commits that binding, and DiligentCore neither allocates nor writes a
// descriptor set for it. The object motion pipelines (D48) keep the dynamic variables.

#pragma once

#include <rex/graphics/pipeline/shader/spirv.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Buffer.h"
#include "BufferView.h"
#include "PipelineState.h"
#include "PipelineResourceSignature.h"
#include "PipelineStateCache.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Shader.h"
#include "ShaderResourceBinding.h"
#include "TextureView.h"
#include "internal.h"

namespace native_x_renderer {

// The state of a draw as DiligentCore enums (stored as bytes). Reset() before filling.
struct PipelineState {
  const Diligent::IShader* vertex_shader;
  const Diligent::IShader* pixel_shader;  // may be null: depth-only draws
  uint16_t rtv_formats[4];
  uint16_t dsv_format;
  uint8_t render_target_count;
  uint8_t samples;
  uint8_t topology;
  uint8_t cull_mode;
  uint8_t front_counter_clockwise;
  uint8_t wireframe;
  uint8_t depth_clip;
  uint8_t depth_enable;
  uint8_t depth_write;
  uint8_t depth_func;
  uint8_t stencil_enable;
  uint8_t stencil_read_mask;
  uint8_t stencil_write_mask;
  uint8_t stencil_front[4];  // fail, depth fail, pass, function
  uint8_t stencil_back[4];
  uint8_t padding[5];
  int32_t depth_bias;
  float slope_scaled_depth_bias;
  struct Target {
    uint8_t blend_enable, src, dst, op, src_alpha, dst_alpha, op_alpha, write_mask;
  } targets[4];
  uint8_t tail_padding[4];

  void Reset() { std::memset(this, 0, sizeof(*this)); }
  bool operator==(const PipelineState& other) const { return !std::memcmp(this, &other, sizeof(*this)); }
};

static_assert(sizeof(PipelineState) == 96, "PipelineState must have no implicit padding (it is hashed as bytes)");

struct PipelineStateHash {
  size_t operator()(const PipelineState& state) const;
};

// D131: the game pipelines' one resource signature, so that every one of them has the same pipeline layout (the
// parts of a pipeline linked from libraries must). Its textures and samplers are named by their place in the
// translation (spirv::NamePositionally: "xe_vs_t0", "xe_ps_s3"...), these many per stage (vertex, pixel); a shader
// with more keeps the translator's names and its pipelines their own layout, as before. The stored translations of ten
// games have at most 24 pixel shader textures and 12 samplers (Kameo), and no vertex shader texture.
inline constexpr uint32_t kSignatureTextures[2] = {8, 32};
inline constexpr uint32_t kSignatureSamplers[2] = {8, 16};
inline constexpr uint32_t kTexturesSet[2] = {2, 3};  // the translator's descriptor sets for each stage's textures
inline const char* const kSignaturePrefix[2] = {"xe_vs_", "xe_ps_"};
// Whether the game pipelines use the signature (native_x_renderer_resource_signature, read at startup).
bool ResourceSignatureOn();

// The buffers every pipeline binds.
struct PipelineBuffers {
  Diligent::IBufferView* shared_memory = nullptr;
  Diligent::IBuffer* system_constants = nullptr;
  Diligent::IBuffer* bool_loop_constants = nullptr;
  Diligent::IBuffer* fetch_constants = nullptr;
  Diligent::IBuffer* motion_constants = nullptr;  // XeMotionConstants, the motion pixel shader's (D48)
};

// A pipeline's textures and samplers as a draw sets them, in a shader resource binding of their own (D111).
struct BindingSet {
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  // The objects, in the order the draw path lists them: per stage, the textures then the samplers, in the order of
  // the shader's bindings (null where DiligentCore did not find the variable). The variables keep references to
  // them, so a pointer cannot belong to another object while the set is kept.
  std::vector<Diligent::IDeviceObject*> objects;
  std::vector<Diligent::ITextureView*> views;  // the texture views among them, for the readable test (D82)
  uint64_t last_used = 0;                      // the frame of the last draw with it
};

struct Pipeline {
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
  // Compiled and its resources set up (PipelineCache::Ready); failed: compilation failed (logged).
  bool ready = false, failed = false;
  std::array<const rex::graphics::SpirvShader*, 2> shaders{};  // owned by the shader cache
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  // Per stage (0 vertex, 1 pixel), in the order of the shader's texture and sampler bindings. An entry
  // is null if DiligentCore did not find the resource.
  std::array<std::vector<Diligent::IShaderResourceVariable*>, 2> textures;
  std::array<std::vector<Diligent::IShaderResourceVariable*>, 2> samplers;
  // What each of those variables holds (D82): a draw sets only the ones that change. The variable keeps a
  // reference to its object, so the pointer cannot be reused by another object while it is here.
  std::array<std::vector<Diligent::IDeviceObject*>, 2> texture_objects;
  std::array<std::vector<Diligent::IDeviceObject*>, 2> sampler_objects;
  // The frame the pipeline last drew in, and whether its texture variables hold views (D55).
  uint64_t last_drawn = 0;
  bool textures_set = false;
  // Per stage: the shader's float constants (16 bytes each, packed as the SDK packs them).
  std::array<Diligent::RefCntAutoPtr<Diligent::IBuffer>, 2> float_constants;
  // Made with the resource signature (D131): its buffers are the signature's, the float constants set in each shader
  // resource binding, its textures and samplers named by their place.
  bool signature = false;
  // A motion pipeline (D48): the motion vertex shader and NativeXRenderer's motion pixel shader; the vertex
  // shader's float constants of the last frame, as float_constants[0].
  bool motion = false;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> previous_float_constants;
  // D111: whether the pipeline draws with binding sets; per stage, each texture and sampler variable's index in a
  // shader resource binding (UINT32_MAX: not found); the sets by the hash of their objects, and the last one used.
  bool binding_sets_on = false;
  std::array<std::vector<uint32_t>, 2> texture_indices;
  std::array<std::vector<uint32_t>, 2> sampler_indices;
  std::unordered_multimap<uint64_t, std::unique_ptr<BindingSet>> binding_sets;
  BindingSet* current_set = nullptr;
};

class PipelineCache {
 public:
  // Reads native_x_renderer_binding_cache (D111).
  void Initialize(Diligent::IRenderDevice* device, const PipelineBuffers& buffers);
  // DiligentCore's pipeline cache the pipelines are compiled with (D42); none by default.
  void SetCache(Diligent::IPipelineStateCache* cache) { cache_ = cache; }

  // The pipeline for a state, its compilation started on first use (`created` set then). nullptr if it
  // could not be created. `motion`: a motion pipeline (D48), whose pixel shader is not a guest shader
  // (`pixel_shader` null).
  Pipeline* Get(const PipelineState& state, const rex::graphics::SpirvShader& vertex_shader,
                const rex::graphics::SpirvShader* pixel_shader, bool* created = nullptr, bool motion = false);
  // Whether the pipeline can draw now: false while it compiles, and if it failed. The first call after
  // the compilation finished sets up its resources.
  bool Ready(Pipeline& pipeline);

  size_t count() const { return pipelines_.size(); }
  // D131: the pipelines made with the resource signature, and those with a layout of their own (a shader with more
  // textures or samplers than the signature has, or the signature off; motion pipelines not counted).
  uint32_t signature_count() const { return signature_count_; }
  uint32_t own_layout_count() const { return own_layout_count_; }
  // Resets the texture variables of the pipelines not drawn for `unused_frames` (D55). A variable holds its
  // view, and a view its texture: a pipeline left with a texture the guest texture cache evicted, or a
  // resolved texture NativeXRenderer freed, would keep its memory. What the variables held is forgotten with
  // them (D82), so the pipeline's next draw sets all of its textures again. Not while a recorded draw may still commit the pipeline's bindings
  // (the object motion draws, D48). Returns the pipelines, and the textures nothing else held.
  // The binding sets (D111) not used for `unused_frames` are dropped the same way.
  struct Released {
    uint32_t pipelines = 0, sets = 0, textures = 0;
    uint64_t bytes = 0;
  };
  Released ReleaseTextures(uint64_t frame, uint64_t unused_frames);
  // D111: the binding set of a pipeline drawing with binding sets for `objects` (as BindingSet::objects lists
  // them), created with its variables set if the pipeline has none for them (`created` then); null if DiligentCore
  // could not create it.
  BindingSet* FindSet(Pipeline& pipeline, const std::vector<Diligent::IDeviceObject*>& objects, uint64_t frame,
                      bool* created);
  // D111: drops the binding sets not used for `unused_frames`; returns how many. Not with a set the draw path may
  // still commit (it forgets the last committed set after this).
  uint32_t ReleaseSets(uint64_t frame, uint64_t unused_frames);
  // The binding sets kept, and the variables set in the sets created since the last call.
  uint32_t binding_set_count() const { return binding_set_count_; }
  uint32_t TakeSetVariables() {
    uint32_t variables = set_variables_;
    set_variables_ = 0;
    return variables;
  }
  // Pipelines created since the last call.
  internal::CreationTime TakeTime() {
    internal::CreationTime time = time_;
    time_ = {};
    return time;
  }

 private:
  uint32_t ReleaseSets(Pipeline& pipeline, uint64_t frame, uint64_t unused_frames,
                       const std::function<void(Diligent::ITexture*)>& let_go);

  // The pipeline's float constant buffers set in a shader resource binding of the signature (D131).
  void SetFloatConstants(Pipeline& pipeline, Diligent::IShaderResourceBinding* srb);

  Diligent::IRenderDevice* device_ = nullptr;
  Diligent::IPipelineStateCache* cache_ = nullptr;
  PipelineBuffers buffers_;
  Diligent::RefCntAutoPtr<Diligent::IPipelineResourceSignature> signature_;  // D131; null when off
  bool binding_cache_ = false;  // D111
  uint32_t binding_set_count_ = 0, set_variables_ = 0;
  uint32_t signature_count_ = 0, own_layout_count_ = 0;
  internal::CreationTime time_;
  std::unordered_map<PipelineState, std::unique_ptr<Pipeline>, PipelineStateHash> pipelines_;
};

}  // namespace native_x_renderer
