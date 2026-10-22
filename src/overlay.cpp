// NativeXRenderer - the SDK's ImGui overlays on NativeXRenderer's swap chain (D75).

#include "overlay.h"

#include "glsl.h"

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Diligent;

namespace native_x_renderer {
namespace {

// Dear ImGui's vertices (rex::ui::ImmediateVertex): position and texture coordinates in the coordinate space, an
// RGBA8 color. Diligent's clip space has y up (D3D's convention), the coordinate space y down.
constexpr const char* kVertexShader = R"(#version 450
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(set = 0, binding = 0) uniform OverlayConstants {
  vec4 scale_offset;  // clip = position * xy + zw
};
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
  v_uv = in_uv;
  v_color = in_color;
  gl_Position = vec4(in_position * scale_offset.xy + scale_offset.zw, 0.0, 1.0);
}
)";

constexpr const char* kPixelShader = R"(#version 450
layout(set = 0, binding = 1) uniform sampler2D overlay_texture;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 color;
void main() {
  color = v_color * texture(overlay_texture, v_uv);
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

// Grows a dynamic buffer to hold `bytes`.
bool Reserve(IRenderDevice* device, RefCntAutoPtr<IBuffer>& buffer, uint32_t& capacity, uint32_t bytes,
             Diligent::BIND_FLAGS bind, const char* name) {
  if (buffer && capacity >= bytes) return true;
  capacity = std::max<uint32_t>(bytes, std::max<uint32_t>(capacity * 2, 64 * 1024));
  BufferDesc desc;
  desc.Name = name;
  desc.Size = capacity;
  desc.Usage = USAGE_DYNAMIC;
  desc.BindFlags = bind;
  desc.CPUAccessFlags = CPU_ACCESS_WRITE;
  buffer.Release();
  device->CreateBuffer(desc, nullptr, &buffer);
  return buffer != nullptr;
}

}  // namespace

bool Overlay::Initialize(IRenderDevice* device, TEXTURE_FORMAT target_format) {
  device_ = device;
  auto vs = CreateShader(device, SHADER_TYPE_VERTEX, kVertexShader, "NativeXRenderer overlay VS");
  auto ps = CreateShader(device, SHADER_TYPE_PIXEL, kPixelShader, "NativeXRenderer overlay PS");
  if (!vs || !ps) return false;

  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = "NativeXRenderer overlay";
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = target_format;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  info.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = False;
  auto& blend = info.GraphicsPipeline.BlendDesc.RenderTargets[0];
  blend.BlendEnable = True;
  blend.SrcBlend = BLEND_FACTOR_SRC_ALPHA;
  blend.DestBlend = BLEND_FACTOR_INV_SRC_ALPHA;
  blend.BlendOp = BLEND_OPERATION_ADD;
  blend.SrcBlendAlpha = BLEND_FACTOR_ONE;
  blend.DestBlendAlpha = BLEND_FACTOR_INV_SRC_ALPHA;
  blend.BlendOpAlpha = BLEND_OPERATION_ADD;
  LayoutElement layout[] = {
      {0, 0, 2, VT_FLOAT32, False},
      {1, 0, 2, VT_FLOAT32, False},
      {2, 0, 4, VT_UINT8, True},
  };
  info.GraphicsPipeline.InputLayout.LayoutElements = layout;
  info.GraphicsPipeline.InputLayout.NumElements = 3;
  info.pVS = vs;
  info.pPS = ps;
  ShaderResourceVariableDesc variables[] = {
      {SHADER_TYPE_PIXEL, "overlay_texture", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
  };
  // Dear ImGui's font atlas, the only texture the SDK's dialogs draw with: filtered, clamped.
  SamplerDesc linear;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "overlay_texture", linear}};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables;
  info.PSODesc.ResourceLayout.NumVariables = 1;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
  device->CreateGraphicsPipelineState(info, &pipeline_);
  if (!pipeline_) return false;

  BufferDesc constants;
  constants.Name = "NativeXRenderer overlay constants";
  constants.Size = 16;
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  if (!constants_) return false;
  if (auto* variable = pipeline_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "OverlayConstants")) {
    variable->Set(constants_);
  }

  white_ = std::make_shared<TextureEntry>();
  white_->width = white_->height = 1;
  white_->rgba = {255, 255, 255, 255};
  return true;
}

void Overlay::Attach(rex::ui::Window* window) {
  // The runtime's ImGuiDrawer exists once the SDK set up its presentation, before the game's first frame.
  auto* kernel = rex::system::kernel_state();
  rex::ui::ImGuiDrawer* imgui = kernel && kernel->emulator() ? kernel->emulator()->imgui_drawer() : nullptr;
  if (!imgui || !window) return;
  imgui_ = imgui;
  // The SDK's presenter is detached from the window and never paints: the dialogs are drawn here. Like every call
  // into the ImGuiDrawer, on the UI thread.
  window->app_context().CallInUIThreadDeferred(
      [imgui, this] { imgui->SetPresenterAndImmediateDrawer(nullptr, this); });
  REXLOG_INFO("[native_x_renderer] the SDK's dialogs (ImGui) are drawn over NativeXRenderer's frames");
}

void Overlay::Present(IDeviceContext* context, ITextureView* target, uint32_t width, uint32_t height,
                      rex::ui::Window* window) {
  if (!pipeline_) return;
  if (!imgui_) {
    Attach(window);
    if (!imgui_) return;
  }
  // One draw of the dialogs per presented frame, on the UI thread, where Dear ImGui takes the window's input. The
  // record it leaves is drawn from the next frame on.
  if (!draw_requested_.exchange(true)) {
    rex::ui::ImGuiDrawer* imgui = imgui_;
    window->app_context().CallInUIThreadDeferred([this, imgui, window] {
      draw_requested_.store(false);
      if (!imgui->HasDialogs()) {
        dialogs_open_.store(false);
        return;
      }
      rex::ui::AppUIDrawContext ui_context(window->GetActualPhysicalWidth(), window->GetActualPhysicalHeight());
      imgui->Draw(ui_context);
      // After the record is published: a new dialog is never drawn with the last record of the one before.
      dialogs_open_.store(true);
    });
  }
  if (!dialogs_open_.load()) {
    drawing_.draws.clear();
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (has_published_) {
      std::swap(drawing_, published_);
      has_published_ = false;
    }
  }
  if (!drawing_.draws.empty() && width && height) Render(context, target, width, height, drawing_);
}

bool Overlay::Prepare(TextureEntry& data) {
  if (data.srb) return true;
  if (!data.width || !data.height || data.rgba.size() < size_t(data.width) * data.height * 4) return false;
  TextureDesc desc;
  desc.Name = "NativeXRenderer overlay texture";
  desc.Type = RESOURCE_DIM_TEX_2D;
  desc.Width = data.width;
  desc.Height = data.height;
  desc.Format = TEX_FORMAT_RGBA8_UNORM;
  desc.Usage = USAGE_IMMUTABLE;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  TextureSubResData level;
  level.pData = data.rgba.data();
  level.Stride = uint64_t(data.width) * 4;
  Diligent::TextureData init{&level, 1};
  device_->CreateTexture(desc, &init, &data.texture);
  if (!data.texture) return false;
  pipeline_->CreateShaderResourceBinding(&data.srb, true);
  if (auto* variable = data.srb->GetVariableByName(SHADER_TYPE_PIXEL, "overlay_texture")) {
    variable->Set(data.texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  }
  data.rgba.clear();
  data.rgba.shrink_to_fit();
  return true;
}

void Overlay::Render(IDeviceContext* context, ITextureView* target, uint32_t width, uint32_t height,
                     const Frame& frame) {
  if (!(frame.width > 0.0f) || !(frame.height > 0.0f) || frame.vertices.empty() || frame.indices.empty()) return;
  uint32_t vertex_bytes = uint32_t(frame.vertices.size() * sizeof(rex::ui::ImmediateVertex));
  uint32_t index_bytes = uint32_t(frame.indices.size() * sizeof(uint16_t));
  if (!Reserve(device_, vertices_, vertex_capacity_, vertex_bytes, Diligent::BIND_VERTEX_BUFFER,
               "NativeXRenderer overlay vertices") ||
      !Reserve(device_, indices_, index_capacity_, index_bytes, Diligent::BIND_INDEX_BUFFER,
               "NativeXRenderer overlay indices")) {
    return;
  }
  void* data = nullptr;
  context->MapBuffer(vertices_, MAP_WRITE, MAP_FLAG_DISCARD, data);
  std::memcpy(data, frame.vertices.data(), vertex_bytes);
  context->UnmapBuffer(vertices_, MAP_WRITE);
  context->MapBuffer(indices_, MAP_WRITE, MAP_FLAG_DISCARD, data);
  std::memcpy(data, frame.indices.data(), index_bytes);
  context->UnmapBuffer(indices_, MAP_WRITE);
  context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
  float scale_offset[4] = {2.0f / frame.width, -2.0f / frame.height, -1.0f, 1.0f};
  std::memcpy(data, scale_offset, sizeof(scale_offset));
  context->UnmapBuffer(constants_, MAP_WRITE);

  context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->SetPipelineState(pipeline_);
  Viewport viewport;
  viewport.Width = float(width);
  viewport.Height = float(height);
  context->SetViewports(1, &viewport, 0, 0);
  IBuffer* buffers[] = {vertices_};
  uint64_t offsets[] = {0};
  context->SetVertexBuffers(0, 1, buffers, offsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                            SET_VERTEX_BUFFERS_FLAG_RESET);
  context->SetIndexBuffer(indices_, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  float scale_x = float(width) / frame.width, scale_y = float(height) / frame.height;
  TextureEntry* bound = nullptr;
  for (const DrawCommand& draw : frame.draws) {
    if (!draw.index_count) continue;
    TextureEntry* texture = draw.texture ? draw.texture.get() : white_.get();
    if (!Prepare(*texture)) continue;
    // The scissor, from the coordinate space to the target's pixels.
    Rect scissor{0, 0, int32_t(width), int32_t(height)};
    if (draw.scissor) {
      auto clamp = [](float v, uint32_t limit) {
        return int32_t(std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, float(limit)));
      };
      scissor.left = clamp(std::floor(draw.left * scale_x), width);
      scissor.top = clamp(std::floor(draw.top * scale_y), height);
      scissor.right = clamp(std::ceil(draw.right * scale_x), width);
      scissor.bottom = clamp(std::ceil(draw.bottom * scale_y), height);
      if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
    }
    context->SetScissorRects(1, &scissor, 0, 0);
    if (texture != bound) {
      context->CommitShaderResources(texture->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      bound = texture;
    }
    DrawIndexedAttribs attribs;
    attribs.NumIndices = draw.index_count;
    attribs.IndexType = VT_UINT16;
    attribs.FirstIndexLocation = draw.first_index;
    attribs.BaseVertex = uint32_t(draw.base_vertex);
    context->DrawIndexed(attribs);
  }
}

std::unique_ptr<rex::ui::ImmediateTexture> Overlay::CreateTexture(uint32_t width, uint32_t height,
                                                                  rex::ui::ImmediateTextureFilter filter,
                                                                  bool is_repeated, const uint8_t* data) {
  (void)filter;
  (void)is_repeated;
  auto texture = std::make_unique<Texture>(width, height);
  texture->data = std::make_shared<TextureEntry>();
  texture->data->width = width;
  texture->data->height = height;
  if (data) texture->data->rgba.assign(data, data + size_t(width) * height * 4);
  return texture;
}

void Overlay::Begin(rex::ui::UIDrawContext& ui_draw_context, float width, float height) {
  ImmediateDrawer::Begin(ui_draw_context, width, height);
  recording_ = Frame{};
  recording_.width = coordinate_space_width();
  recording_.height = coordinate_space_height();
}

void Overlay::BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) {
  batch_vertex_base_ = uint32_t(recording_.vertices.size());
  batch_index_base_ = uint32_t(recording_.indices.size());
  if (batch.vertices && batch.vertex_count > 0) {
    recording_.vertices.insert(recording_.vertices.end(), batch.vertices, batch.vertices + batch.vertex_count);
  }
  if (batch.indices && batch.index_count > 0) {
    recording_.indices.insert(recording_.indices.end(), batch.indices, batch.indices + batch.index_count);
  } else {
    // Not indexed: the vertices in order.
    for (int i = 0; i < batch.vertex_count && i < 65536; ++i) recording_.indices.push_back(uint16_t(i));
  }
}

void Overlay::Draw(const rex::ui::ImmediateDraw& draw) {
  if (draw.primitive_type != rex::ui::ImmediatePrimitiveType::kTriangles || draw.count <= 0) return;
  DrawCommand command;
  command.first_index = batch_index_base_ + uint32_t(draw.index_offset);
  command.index_count = uint32_t(draw.count);
  command.base_vertex = int32_t(batch_vertex_base_) + draw.base_vertex;
  if (draw.texture) {
    // Only the textures made here. The SDK makes its dialogs' textures (the font atlas, the achievement icons) with
    // the ImGuiDrawer's current drawer, this one (D91); one made with another drawer is not drawn.
    auto* texture = dynamic_cast<Texture*>(draw.texture);
    if (!texture) return;
    command.texture = texture->data;
  }
  command.scissor = draw.scissor;
  command.left = draw.scissor_left;
  command.top = draw.scissor_top;
  command.right = draw.scissor_right;
  command.bottom = draw.scissor_bottom;
  recording_.draws.push_back(std::move(command));
}

void Overlay::EndDrawBatch() {}

void Overlay::End() {
  ImmediateDrawer::End();
  std::lock_guard lock(mutex_);
  published_ = std::move(recording_);
  has_published_ = true;
  recording_ = Frame{};
}

}  // namespace native_x_renderer
