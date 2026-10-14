// XenosNative - see blit.h.

#include "blit.h"

#include "glsl.h"

#include <rex/logging.h>

#include <cmath>
#include <cstring>
#include <string>

namespace xenos_native {

using namespace Diligent;

namespace {

// One triangle covering the viewport; uv is 0..1 across it (top left 0, 0). DiligentCore's Vulkan backend
// flips Y (D19): NDC y = -1 is the bottom of the viewport.
constexpr const char* kVertexShader = R"(#version 450
layout(location = 0) out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_uv = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The constants every pixel shader reads.
constexpr const char* kConstants = R"(
layout(set = 0, binding = 1) uniform BlitConstants {
  ivec4 source_origin;   // xy: first source texel
  ivec4 target_origin;   // xy: first target texel (the viewport's origin)
  ivec4 options;         // x: swap red and blue, y: samples, z: sample (-1 = average), w: swizzle (scaled)
  vec4 scale;            // x: color factor
  ivec4 gamma;           // x: the gamma ramp to apply, 0: the 256-entry table, 1: piecewise linear
};
)";

constexpr const char* kColorBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) out vec4 color;
void main() {
  vec4 c = texelFetch(source, ivec2(gl_FragCoord.xy) - target_origin.xy + source_origin.xy, 0);
  if (options.x != 0) c = c.bgra;
  color = c * scale.x;
}
)";

constexpr const char* kColorMsaaBody = R"(
layout(set = 0, binding = 0) uniform sampler2DMS source;
layout(location = 0) out vec4 color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) - target_origin.xy + source_origin.xy;
  vec4 c;
  if (options.z >= 0) {
    c = texelFetch(source, p, options.z);
  } else {
    c = vec4(0.0);
    for (int i = 0; i < options.y; ++i) c += texelFetch(source, p, i);
    c /= float(options.y);
  }
  if (options.x != 0) c = c.bgra;
  color = c * scale.x;
}
)";

constexpr const char* kDepthBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) out vec4 color;
void main() {
  color = vec4(texelFetch(source, ivec2(gl_FragCoord.xy) - target_origin.xy + source_origin.xy, 0).r);
}
)";

constexpr const char* kDepthMsaaBody = R"(
layout(set = 0, binding = 0) uniform sampler2DMS source;
layout(location = 0) out vec4 color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) - target_origin.xy + source_origin.xy;
  color = vec4(texelFetch(source, p, max(options.z, 0)).r);
}
)";

constexpr const char* kScaledBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;
void main() {
  vec2 size = vec2(textureSize(source, 0));
  vec2 uv = (vec2(source_origin.xy) + v_uv * vec2(source_origin.zw)) / size;
  vec4 c = textureLod(source, uv, 0.0);
  if (options.x != 0) c = c.bgra;
  vec4 swizzled;
  for (int i = 0; i < 4; ++i) {
    int component = (options.w >> (3 * i)) & 7;
    swizzled[i] = component < 4 ? c[component] : float(component & 1);
  }
  color = swizzled * scale.x;
}
)";

// The guest's display gamma ramp (gamma.h), as the SDK's apply_gamma shaders apply it: the table maps each
// 8-bit channel to 10 bits (red in bits 20:29, green 10:19, blue 0:9); the piecewise linear ramp takes the
// 10-bit value, its 7 high bits choose an entry per channel (base in bits 0:15, delta in 16:31) and the low
// 3 bits interpolate within it.
constexpr const char* kGammaBody = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 2) uniform usampler2D ramp;
layout(location = 0) out vec4 color;
float ApplyRamp(float value, int channel) {
  value = clamp(value, 0.0, 1.0);
  if (gamma.x == 0) {
    uint entry = texelFetch(ramp, ivec2(int(value * 255.0 + 0.5), 0), 0).r;
    return float((entry >> uint(20 - 10 * channel)) & 1023u) / 1023.0;
  }
  uint v = uint(value * 1023.0 + 0.5);
  uint entry = texelFetch(ramp, ivec2(int(v >> 3u), 1 + channel), 0).r;
  float result = float(entry & 0xFFFFu) + float((v & 7u) * (entry >> 16u)) * 0.125;
  return clamp(result * 1.52737048e-05, 0.0, 1.0);
}
void main() {
  vec4 c = texelFetch(source, ivec2(gl_FragCoord.xy) - target_origin.xy + source_origin.xy, 0);
  vec4 swizzled;
  for (int i = 0; i < 4; ++i) {
    int component = (options.w >> (3 * i)) & 7;
    swizzled[i] = component < 4 ? c[component] : float(component & 1);
  }
  color = vec4(ApplyRamp(swizzled.r, 0), ApplyRamp(swizzled.g, 1), ApplyRamp(swizzled.b, 2), 1.0);
}
)";

struct Constants {
  int32_t source_origin[4];
  int32_t target_origin[4];
  int32_t options[4];
  float scale[4];
  int32_t gamma[4];
};

RefCntAutoPtr<IShader> CreateShader(IRenderDevice* device, SHADER_TYPE type, const std::vector<uint32_t>& spirv,
                                    const char* name) {
  RefCntAutoPtr<IShader> shader;
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

bool Blitter::Initialize(IRenderDevice* device) {
  device_ = device;
  vertex_shader_ = CreateShader(device, SHADER_TYPE_VERTEX, CompileGlsl(GlslStage::kVertex, kVertexShader, "blit vs"),
                                "XenosNative blit VS");
  struct {
    Source kind;
    const char* body;
    const char* name;
  } pixel[] = {
      {Source::kColor, kColorBody, "XenosNative blit color PS"},
      {Source::kColorMsaa, kColorMsaaBody, "XenosNative blit color MSAA PS"},
      {Source::kDepth, kDepthBody, "XenosNative blit depth PS"},
      {Source::kDepthMsaa, kDepthMsaaBody, "XenosNative blit depth MSAA PS"},
      {Source::kColorScaled, kScaledBody, "XenosNative blit scaled PS"},
      {Source::kColorGamma, kGammaBody, "XenosNative blit gamma PS"},
  };
  for (const auto& p : pixel) {
    std::string source = std::string("#version 450\n") + kConstants + p.body;
    pixel_shaders_[p.kind] = CreateShader(device, SHADER_TYPE_PIXEL, CompileGlsl(GlslStage::kPixel, source, p.name), p.name);
    if (!pixel_shaders_[p.kind]) return false;
  }
  BufferDesc constants;
  constants.Name = "XenosNative blit constants";
  constants.Size = sizeof(Constants);
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  return vertex_shader_ && constants_;
}

Blitter::Pipeline* Blitter::GetPipeline(Source kind, TEXTURE_FORMAT target_format) {
  auto key = std::make_tuple(kind, target_format);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second.pso ? &it->second : nullptr;
  Pipeline& pipeline = pipelines_[key];

  GraphicsPipelineStateCreateInfo info;
  std::string name = "XenosNative blit " + std::to_string(int(kind)) + "/" + std::to_string(int(target_format));
  info.PSODesc.Name = name.c_str();
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = target_format;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  info.pVS = vertex_shader_;
  info.pPS = pixel_shaders_[kind];

  // kColorGamma also reads the ramp, an integer texture: point sampling (it is only fetched).
  bool gamma = kind == Source::kColorGamma;
  ShaderResourceVariableDesc variables[] = {
      {SHADER_TYPE_PIXEL, "source", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
      {SHADER_TYPE_PIXEL, "ramp", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
  };
  SamplerDesc linear;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  SamplerDesc point;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "source", linear}, {SHADER_TYPE_PIXEL, "ramp", point}};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables;
  info.PSODesc.ResourceLayout.NumVariables = gamma ? 2 : 1;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = gamma ? 2 : 1;
  device_->CreateGraphicsPipelineState(info, &pipeline.pso);
  if (!pipeline.pso) {
    REXLOG_ERROR("[xenos_native] blit: no pipeline for source {} and target format {}", int(kind),
                 int(target_format));
    return nullptr;
  }
  if (auto* variable = pipeline.pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "BlitConstants")) {
    variable->Set(constants_);
  }
  pipeline.pso->CreateShaderResourceBinding(&pipeline.srb, true);
  return &pipeline;
}

void Blitter::Copy(IDeviceContext* context, ITextureView* source, Source kind, const Rect& from,
                   ITextureView* target, const Rect& to, const Options& options) {
  if (!source || !target || !to.width || !to.height) return;
  if (kind == Source::kColorGamma && !options.ramp) return;
  Pipeline* pipeline = GetPipeline(kind, target->GetDesc().Format);
  if (!pipeline) return;

  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    Constants c{};
    c.source_origin[0] = from.x;
    c.source_origin[1] = from.y;
    c.source_origin[2] = int32_t(from.width);
    c.source_origin[3] = int32_t(from.height);
    c.target_origin[0] = to.x;
    c.target_origin[1] = to.y;
    c.options[0] = options.swap_red_blue ? 1 : 0;
    c.options[1] = int32_t(options.samples);
    c.options[2] = options.sample;
    c.options[3] = int32_t(options.swizzle);
    c.scale[0] = options.scale;
    c.gamma[0] = options.gamma_pwl ? 1 : 0;
    std::memcpy(data, &c, sizeof(c));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }

  context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Viewport viewport;
  viewport.TopLeftX = float(to.x);
  viewport.TopLeftY = float(to.y);
  viewport.Width = float(to.width);
  viewport.Height = float(to.height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  Diligent::Rect scissor{to.x, to.y, to.x + int32_t(to.width), to.y + int32_t(to.height)};
  context->SetScissorRects(1, &scissor, 0, 0);
  context->SetPipelineState(pipeline->pso);
  pipeline->srb->GetVariableByName(SHADER_TYPE_PIXEL, "source")->Set(source);
  if (kind == Source::kColorGamma) pipeline->srb->GetVariableByName(SHADER_TYPE_PIXEL, "ramp")->Set(options.ramp);
  context->CommitShaderResources(pipeline->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
}

}  // namespace xenos_native
