// XenosNative - DiligentCore pipelines for guest draws (internal).
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
// The buffers are dynamic buffers, mapped per draw, so they are static variables, set once per pipeline:
// the guest memory and the system, bool/loop and fetch constants are shared by every pipeline; each
// pipeline has its own float constant buffers, the size of its shaders' constants (DiligentCore maps a
// dynamic buffer whole). Textures and samplers are dynamic variables, set per draw in the order of the
// shader's bindings.

#pragma once

#include <rex/graphics/pipeline/shader/spirv.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Buffer.h"
#include "BufferView.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Shader.h"
#include "ShaderResourceBinding.h"

namespace xenos_native {

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

// The buffers every pipeline binds.
struct PipelineBuffers {
  Diligent::IBufferView* shared_memory = nullptr;
  Diligent::IBuffer* system_constants = nullptr;
  Diligent::IBuffer* bool_loop_constants = nullptr;
  Diligent::IBuffer* fetch_constants = nullptr;
};

struct Pipeline {
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  // Per stage (0 vertex, 1 pixel), in the order of the shader's texture and sampler bindings. An entry
  // is null if DiligentCore did not find the resource.
  std::array<std::vector<Diligent::IShaderResourceVariable*>, 2> textures;
  std::array<std::vector<Diligent::IShaderResourceVariable*>, 2> samplers;
  // Per stage: the shader's float constants (16 bytes each, packed as the SDK packs them).
  std::array<Diligent::RefCntAutoPtr<Diligent::IBuffer>, 2> float_constants;
};

class PipelineCache {
 public:
  void Initialize(Diligent::IRenderDevice* device, const PipelineBuffers& buffers);

  // The pipeline for a state, created on first use. nullptr if creation failed (logged once).
  Pipeline* Get(const PipelineState& state, const rex::graphics::SpirvShader& vertex_shader,
                const rex::graphics::SpirvShader* pixel_shader);

  size_t count() const { return pipelines_.size(); }

 private:
  Diligent::IRenderDevice* device_ = nullptr;
  PipelineBuffers buffers_;
  std::unordered_map<PipelineState, std::unique_ptr<Pipeline>, PipelineStateHash> pipelines_;
};

}  // namespace xenos_native
