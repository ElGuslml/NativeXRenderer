// NativeXRenderer - see overlay_motion.h.

#include "overlay_motion.h"

#include <rex/logging.h>

#include <algorithm>
#include <cstring>

#include "Buffer.h"
#include "PipelineState.h"
#include "glsl.h"

namespace native_x_renderer {

using namespace Diligent;

namespace {

// The guest's primitive types kept (VGT_DRAW_INITIATOR's): a quad list (sprites) and a triangle list.
constexpr uint32_t kTriangleList = 4, kQuadList = 13;
// The farthest a draw's center moves between two frames and is still the same draw, in canvas pixels: GH Metallica's
// notes move up to 9 pixels a frame at 80 FPS near the strike line, 12 at 60.
constexpr float kMaxDistance = 64.0f;

// The triangles: the position in clip space (DiligentCore's Vulkan backend flips Y, D19: y = 1 at the top), the
// motion in the motion texture's pixels.
constexpr const char* kVertexShader = R"(#version 450
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_motion;
layout(location = 0) out vec2 v_motion;
void main() {
  v_motion = in_motion;
  gl_Position = vec4(in_position, 0.0, 1.0);
}
)";
constexpr const char* kPixelShader = R"(#version 450
layout(location = 0) in vec2 v_motion;
layout(location = 0) out vec2 out_motion;
layout(location = 1) out float out_depth;
void main() {
  out_motion = v_motion;
  out_depth = 0.0;
}
)";

RefCntAutoPtr<IShader> CreateShader(IRenderDevice* device, SHADER_TYPE type, const char* source, const char* name) {
  RefCntAutoPtr<IShader> shader;
  std::vector<uint32_t> spirv =
      CompileGlsl(type == SHADER_TYPE_VERTEX ? GlslStage::kVertex : GlslStage::kPixel, source, name);
  if (spirv.empty()) return shader;
  ShaderCreateInfo info;
  info.Desc.Name = name;
  info.Desc.ShaderType = type;
  info.Desc.UseCombinedTextureSamplers = true;
  info.ByteCode = spirv.data();
  info.ByteCodeSize = spirv.size() * sizeof(uint32_t);
  device->CreateShader(info, &shader);
  return shader;
}

}  // namespace

void OverlayMotion::Add(uint64_t key, uint64_t vertex_shader, uint32_t primitive, const float* positions,
                        uint32_t vertex_count) {
  bool whole = primitive == kQuadList ? vertex_count % 4 == 0 : primitive == kTriangleList && vertex_count % 3 == 0;
  if (!vertex_count || !whole) return;
  Frame& frame = frames_[current_];
  Entry entry{key, vertex_shader, primitive, uint32_t(frame.positions.size()), vertex_count, {0.0f, 0.0f}, false};
  for (uint32_t i = 0; i < vertex_count; ++i) {
    entry.center[0] += positions[2 * i];
    entry.center[1] += positions[2 * i + 1];
  }
  entry.center[0] /= float(vertex_count);
  entry.center[1] /= float(vertex_count);
  frame.positions.insert(frame.positions.end(), positions, positions + 2 * size_t(vertex_count));
  ++stats_.draws;

  // The closest of the last frame's draws of this kind: its positions are this draw's last ones (else these).
  const Frame& last = frames_[current_ ^ 1];
  const float* previous = nullptr;
  float best = kMaxDistance * kMaxDistance;
  if (auto it = last.by_key.find(key); it != last.by_key.end()) {
    for (uint32_t index : it->second) {
      const Entry& candidate = last.entries[index];
      if (candidate.count != vertex_count || candidate.primitive != primitive) continue;
      float dx = candidate.center[0] - entry.center[0], dy = candidate.center[1] - entry.center[1];
      if (dx * dx + dy * dy < best) {
        best = dx * dx + dy * dy;
        previous = last.positions.data() + candidate.offset;
      }
    }
  }
  entry.matched = previous != nullptr;
  if (entry.matched) ++stats_.matched;
  const float* source = previous ? previous : positions;
  frame.previous.insert(frame.previous.end(), source, source + 2 * size_t(vertex_count));
  frame.by_key[key].push_back(uint32_t(frame.entries.size()));
  frame.entries.push_back(entry);
}

bool OverlayMotion::CreatePipeline(IRenderDevice* device) {
  auto vs = CreateShader(device, SHADER_TYPE_VERTEX, kVertexShader, "NativeXRenderer overlay motion VS");
  auto ps = CreateShader(device, SHADER_TYPE_PIXEL, kPixelShader, "NativeXRenderer overlay motion PS");
  if (!vs || !ps) return false;
  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = "NativeXRenderer overlay motion";
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 2;
  info.GraphicsPipeline.RTVFormats[0] = TEX_FORMAT_RG16_FLOAT;
  info.GraphicsPipeline.RTVFormats[1] = TEX_FORMAT_R32_FLOAT;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  info.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = False;
  LayoutElement layout[] = {
      {0, 0, 2, VT_FLOAT32, False},
      {1, 0, 2, VT_FLOAT32, False},
  };
  info.GraphicsPipeline.InputLayout.LayoutElements = layout;
  info.GraphicsPipeline.InputLayout.NumElements = 2;
  info.pVS = vs;
  info.pPS = ps;
  device->CreateGraphicsPipelineState(info, &pso_);
  return pso_ != nullptr;
}

bool OverlayMotion::Draw(IRenderDevice* device, IDeviceContext* context, ITexture* motion, ITexture* depth,
                         uint32_t canvas_width, uint32_t canvas_height,
                         void (*adjust)(OverlayDraw* draws, size_t count)) {
  Frame& frame = frames_[current_];
  if (adjust && !frame.entries.empty()) {
    draws_.clear();
    for (const Entry& entry : frame.entries) {
      draws_.push_back({entry.vertex_shader, entry.count, frame.positions.data() + entry.offset,
                        frame.previous.data() + entry.offset, entry.matched});
    }
    adjust(draws_.data(), draws_.size());
  }
  // The triangles: each vertex with its movement since the last frame.
  for (const Entry& entry : frame.entries) {
    const float* now = frame.positions.data() + entry.offset;
    const float* last = frame.previous.data() + entry.offset;
    auto vertex = [&](uint32_t i) {
      vertices_.insert(vertices_.end(), {now[2 * i], now[2 * i + 1], last[2 * i] - now[2 * i],
                                         last[2 * i + 1] - now[2 * i + 1]});
    };
    if (entry.primitive == kQuadList) {
      for (uint32_t q = 0; q < entry.count; q += 4) {
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) vertex(q + i);
      }
    } else {
      for (uint32_t i = 0; i < entry.count; ++i) vertex(i);
    }
  }
  bool drawn = false;
  if (!vertices_.empty() && motion && depth && canvas_width && canvas_height && !failed_) {
    if (!pso_ && !CreatePipeline(device)) {
      failed_ = true;
      REXLOG_ERROR("[native_x_renderer] frame generation: the 2D layer's motion pass could not be set up (D124)");
    }
    uint64_t bytes = vertices_.size() * sizeof(float);
    if (pso_ && (!buffer_ || buffer_size_ < bytes)) {
      buffer_.Release();
      buffer_size_ = std::max<uint64_t>(bytes * 2, 64 * 1024);
      BufferDesc desc;
      desc.Name = "NativeXRenderer overlay motion vertices";
      desc.Size = buffer_size_;
      desc.Usage = USAGE_DYNAMIC;
      desc.BindFlags = BIND_VERTEX_BUFFER;
      desc.CPUAccessFlags = CPU_ACCESS_WRITE;
      device->CreateBuffer(desc, nullptr, &buffer_);
    }
    if (pso_ && buffer_) {
      const TextureDesc& size = motion->GetDesc();
      float to_x = float(size.Width) / float(canvas_width), to_y = float(size.Height) / float(canvas_height);
      void* data = nullptr;
      context->MapBuffer(buffer_, MAP_WRITE, MAP_FLAG_DISCARD, data);
      auto* out = static_cast<float*>(data);
      for (size_t i = 0; i < vertices_.size(); i += 4) {
        out[i] = vertices_[i] / float(canvas_width) * 2.0f - 1.0f;
        out[i + 1] = 1.0f - vertices_[i + 1] / float(canvas_height) * 2.0f;
        out[i + 2] = vertices_[i + 2] * to_x;
        out[i + 3] = vertices_[i + 3] * to_y;
      }
      context->UnmapBuffer(buffer_, MAP_WRITE);
      ITextureView* targets[] = {motion->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET),
                                 depth->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET)};
      context->SetRenderTargets(2, targets, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      Viewport viewport;
      viewport.Width = float(size.Width);
      viewport.Height = float(size.Height);
      viewport.MaxDepth = 1.0f;
      context->SetViewports(1, &viewport, 0, 0);
      context->SetPipelineState(pso_);
      IBuffer* buffers[] = {buffer_};
      uint64_t offsets[] = {0};
      context->SetVertexBuffers(0, 1, buffers, offsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                SET_VERTEX_BUFFERS_FLAG_RESET);
      DrawAttribs draw;
      draw.NumVertices = uint32_t(vertices_.size() / 4);
      context->Draw(draw);
      context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
      drawn = true;
    }
  }
  // The frame's draws become the last frame's.
  current_ ^= 1;
  frames_[current_].Clear();
  vertices_.clear();
  return drawn;
}

void OverlayMotion::Clear() {
  for (Frame& frame : frames_) frame.Clear();
  vertices_.clear();
}

}  // namespace native_x_renderer
