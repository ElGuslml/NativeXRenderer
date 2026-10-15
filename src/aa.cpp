// NativeXRenderer - see aa.h.

#include "aa.h"

#include "glsl.h"

#include <rex/logging.h>

#include <cstring>
#include <vector>

#include "AreaTex.h"
#include "SearchTex.h"
#include "smaa_source.h"  // kSmaaSource: thirdparty/smaa/SMAA.hlsl, embedded by CMake

namespace native_x_renderer {

using namespace Diligent;

namespace {

// One triangle over the viewport; v_texcoord is 0..1 across it, top left 0, 0 (DiligentCore's Vulkan
// backend flips Y, D19).
constexpr const char* kFullScreen = R"(
layout(location = 0) out vec2 v_texcoord;
void FullScreen() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_texcoord = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kConstants = R"(
layout(set = 0, binding = 0) uniform AAConstants {
  vec4 rt_metrics;  // 1 / width, 1 / height, width, height
};
)";

std::string SmaaShader(bool vertex, const char* body) {
  std::string source = "#version 450\n#define SMAA_GLSL_4\n#define SMAA_PRESET_HIGH\n#define SMAA_RT_METRICS rt_metrics\n";
  source += vertex ? "#define SMAA_INCLUDE_PS 0\n" : "#define SMAA_INCLUDE_VS 0\n";
  source += kConstants;
  source += kSmaaSource;
  source += "\n";
  if (vertex) source += kFullScreen;
  source += body;
  return source;
}

constexpr const char* kSmaaEdgesVS = R"(
layout(location = 1) out vec4 v_offset[3];
void main() {
  FullScreen();
  SMAAEdgeDetectionVS(v_texcoord, v_offset);
}
)";
constexpr const char* kSmaaEdgesPS = R"(
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec4 v_offset[3];
layout(location = 0) out vec4 out_edges;
void main() {
  out_edges = vec4(SMAALumaEdgeDetectionPS(v_texcoord, v_offset, colorTex), 0.0, 0.0);
}
)";
constexpr const char* kSmaaWeightsVS = R"(
layout(location = 1) out vec4 v_offset[3];
layout(location = 4) out vec2 v_pixcoord;
void main() {
  FullScreen();
  SMAABlendingWeightCalculationVS(v_texcoord, v_pixcoord, v_offset);
}
)";
constexpr const char* kSmaaWeightsPS = R"(
layout(set = 0, binding = 1) uniform sampler2D edgesTex;
layout(set = 0, binding = 2) uniform sampler2D areaTex;
layout(set = 0, binding = 3) uniform sampler2D searchTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec4 v_offset[3];
layout(location = 4) in vec2 v_pixcoord;
layout(location = 0) out vec4 out_weights;
void main() {
  out_weights = SMAABlendingWeightCalculationPS(v_texcoord, v_pixcoord, v_offset, edgesTex, areaTex, searchTex,
                                                vec4(0.0));
}
)";
constexpr const char* kSmaaBlendVS = R"(
layout(location = 1) out vec4 v_offset;
void main() {
  FullScreen();
  SMAANeighborhoodBlendingVS(v_texcoord, v_offset);
}
)";
constexpr const char* kSmaaBlendPS = R"(
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D blendTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec4 v_offset;
layout(location = 0) out vec4 out_color;
void main() {
  out_color = SMAANeighborhoodBlendingPS(v_texcoord, v_offset, colorTex, blendTex);
}
)";

constexpr const char* kFxaaVS = R"(
void main() {
  FullScreen();
}
)";
// The SDK's FXAA (its Vulkan presenter's GetSwapFxaaComputeSource, the normal quality), as a pixel shader
// that computes the luma itself (the SDK's gamma pass puts it in alpha) and keeps the source's alpha.
constexpr const char* kFxaaPS = R"(
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 out_color;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);
const float kEdgeThreshold = 0.166;
const float kEdgeThresholdMin = 0.0833;
const float kSpanMax = 8.0;
const float kDirReduceMul = 0.125;
const float kDirReduceMin = 1.0 / 128.0;

float SampleLuma(vec2 uv) {
  return dot(textureLod(colorTex, uv, 0.0).rgb, kLumaWeights);
}

void main() {
  vec2 uv = v_texcoord;
  vec2 texel = rt_metrics.xy;
  vec4 rgbm = textureLod(colorTex, uv, 0.0);

  float lumaM = dot(rgbm.rgb, kLumaWeights);
  float lumaNW = SampleLuma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = SampleLuma(uv + vec2( texel.x, -texel.y));
  float lumaSW = SampleLuma(uv + vec2(-texel.x,  texel.y));
  float lumaSE = SampleLuma(uv + vec2( texel.x,  texel.y));

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  float lumaRange = lumaMax - lumaMin;

  vec3 result = rgbm.rgb;
  if (lumaRange >= max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kDirReduceMul), kDirReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * texel;

    vec3 rgbA = 0.5 * (textureLod(colorTex, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
                       textureLod(colorTex, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (textureLod(colorTex, uv + dir * -0.5, 0.0).rgb +
                                     textureLod(colorTex, uv + dir * 0.5, 0.0).rgb);
    float lumaB = dot(rgbB, kLumaWeights);
    result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }
  out_color = vec4(result, rgbm.a);
}
)";

std::string FxaaShader(bool vertex) {
  std::string source = "#version 450\n";
  source += kConstants;
  if (vertex) {
    source += kFullScreen;
    source += kFxaaVS;
  } else {
    source += kFxaaPS;
  }
  return source;
}

RefCntAutoPtr<IShader> CreateShader(IRenderDevice* device, SHADER_TYPE type, const std::string& source,
                                    const char* name) {
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

RefCntAutoPtr<ITexture> CreateLookupTexture(IRenderDevice* device, const char* name, uint32_t width, uint32_t height,
                                            TEXTURE_FORMAT format, const void* data, uint32_t pitch) {
  TextureDesc desc;
  desc.Name = name;
  desc.Type = RESOURCE_DIM_TEX_2D;
  desc.Width = width;
  desc.Height = height;
  desc.Format = format;
  desc.Usage = USAGE_IMMUTABLE;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  TextureSubResData subresource{data, pitch};
  TextureData initial{&subresource, 1};
  RefCntAutoPtr<ITexture> texture;
  device->CreateTexture(desc, &initial, &texture);
  return texture;
}

}  // namespace

Antialiasing::Mode Antialiasing::ParseMode(std::string_view name) {
  if (name == "off") return Mode::kOff;
  if (name == "fxaa") return Mode::kFxaa;
  if (name != "smaa") REXLOG_WARN("[native_x_renderer] antialiasing \"{}\" unknown: SMAA", name);
  return Mode::kSmaa;
}

const char* Antialiasing::ModeName(Mode mode) {
  switch (mode) {
    case Mode::kFxaa:
      return "FXAA";
    case Mode::kSmaa:
      return "SMAA";
    default:
      return "off";
  }
}

bool Antialiasing::Initialize(IRenderDevice* device, Mode mode) {
  device_ = device;
  mode_ = mode;
  if (mode_ == Mode::kOff) return true;
  BufferDesc constants;
  constants.Name = "NativeXRenderer antialiasing constants";
  constants.Size = 16;
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  bool ok = constants_ != nullptr;
  if (ok && mode_ == Mode::kSmaa) {
    area_ = CreateLookupTexture(device, "NativeXRenderer SMAA area", AREATEX_WIDTH, AREATEX_HEIGHT, TEX_FORMAT_RG8_UNORM,
                                areaTexBytes, AREATEX_PITCH);
    search_ = CreateLookupTexture(device, "NativeXRenderer SMAA search", SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
                                  TEX_FORMAT_R8_UNORM, searchTexBytes, SEARCHTEX_PITCH);
    ok = area_ && search_ &&
         CreatePass(smaa_edges_, "NativeXRenderer SMAA edges", SmaaShader(true, kSmaaEdgesVS),
                    SmaaShader(false, kSmaaEdgesPS), {{"colorTex", true}}, TEX_FORMAT_RG8_UNORM) &&
         CreatePass(smaa_weights_, "NativeXRenderer SMAA weights", SmaaShader(true, kSmaaWeightsVS),
                    SmaaShader(false, kSmaaWeightsPS), {{"edgesTex", true}, {"areaTex", true}, {"searchTex", false}},
                    TEX_FORMAT_RGBA8_UNORM);
    if (ok) {
      smaa_weights_.srb->GetVariableByName(SHADER_TYPE_PIXEL, "areaTex")
          ->Set(area_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
      smaa_weights_.srb->GetVariableByName(SHADER_TYPE_PIXEL, "searchTex")
          ->Set(search_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    }
  }
  if (!ok) {
    REXLOG_ERROR("[native_x_renderer] antialiasing ({}) could not be set up: the image is not antialiased",
                 ModeName(mode_));
    mode_ = Mode::kOff;
    return false;
  }
  REXLOG_INFO("[native_x_renderer] antialiasing: {}", ModeName(mode_));
  return true;
}

bool Antialiasing::CreatePass(Pass& pass, const char* name, const std::string& vertex, const std::string& pixel,
                              std::initializer_list<Input> inputs, TEXTURE_FORMAT target_format) {
  std::string vs_name = std::string(name) + " VS", ps_name = std::string(name) + " PS";
  RefCntAutoPtr<IShader> vs = CreateShader(device_, SHADER_TYPE_VERTEX, vertex, vs_name.c_str());
  RefCntAutoPtr<IShader> ps = CreateShader(device_, SHADER_TYPE_PIXEL, pixel, ps_name.c_str());
  if (!vs || !ps) return false;

  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = name;
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = target_format;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  info.pVS = vs;
  info.pPS = ps;

  std::vector<ShaderResourceVariableDesc> variables;
  std::vector<ImmutableSamplerDesc> samplers;
  SamplerDesc linear, point;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  for (const Input& input : inputs) {
    variables.push_back({SHADER_TYPE_PIXEL, input.name, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC});
    samplers.push_back({SHADER_TYPE_PIXEL, input.name, input.linear ? linear : point});
  }
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables.data();
  info.PSODesc.ResourceLayout.NumVariables = uint32_t(variables.size());
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers.data();
  info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(samplers.size());
  device_->CreateGraphicsPipelineState(info, &pass.pso);
  if (!pass.pso) {
    REXLOG_ERROR("[native_x_renderer] {}: no pipeline", name);
    return false;
  }
  for (SHADER_TYPE stage : {SHADER_TYPE_VERTEX, SHADER_TYPE_PIXEL}) {
    if (auto* variable = pass.pso->GetStaticVariableByName(stage, "AAConstants")) variable->Set(constants_);
  }
  pass.pso->CreateShaderResourceBinding(&pass.srb, true);
  return pass.srb != nullptr;
}

Antialiasing::Pass* Antialiasing::OutputPass(TEXTURE_FORMAT format) {
  auto it = output_passes_.find(format);
  if (it != output_passes_.end()) return it->second.pso ? &it->second : nullptr;
  Pass& pass = output_passes_[format];
  bool ok = mode_ == Mode::kSmaa
                ? CreatePass(pass, "NativeXRenderer SMAA blending", SmaaShader(true, kSmaaBlendVS),
                             SmaaShader(false, kSmaaBlendPS), {{"colorTex", true}, {"blendTex", true}}, format)
                : CreatePass(pass, "NativeXRenderer FXAA", FxaaShader(true), FxaaShader(false), {{"colorTex", true}}, format);
  return ok ? &pass : nullptr;
}

bool Antialiasing::EnsureTextures(uint32_t width, uint32_t height, TEXTURE_FORMAT format) {
  auto fits = [&](ITexture* texture, TEXTURE_FORMAT texture_format) {
    return texture && texture->GetDesc().Width == width && texture->GetDesc().Height == height &&
           texture->GetDesc().Format == texture_format;
  };
  auto create = [&](RefCntAutoPtr<ITexture>& texture, const char* name, TEXTURE_FORMAT texture_format,
                    BIND_FLAGS bind) {
    texture.Release();
    TextureDesc desc;
    desc.Name = name;
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = width;
    desc.Height = height;
    desc.Format = texture_format;
    desc.Usage = USAGE_DEFAULT;
    desc.BindFlags = bind;
    device_->CreateTexture(desc, nullptr, &texture);
    return texture != nullptr;
  };
  if (!fits(color_, format) &&
      !create(color_, "NativeXRenderer antialiasing input", format, BIND_SHADER_RESOURCE)) {
    return false;
  }
  if (mode_ != Mode::kSmaa) return true;
  if (!fits(edges_, TEX_FORMAT_RG8_UNORM) &&
      !create(edges_, "NativeXRenderer SMAA edges", TEX_FORMAT_RG8_UNORM, BIND_SHADER_RESOURCE | BIND_RENDER_TARGET)) {
    return false;
  }
  if (!fits(weights_, TEX_FORMAT_RGBA8_UNORM) &&
      !create(weights_, "NativeXRenderer SMAA weights", TEX_FORMAT_RGBA8_UNORM, BIND_SHADER_RESOURCE | BIND_RENDER_TARGET)) {
    return false;
  }
  return true;
}

void Antialiasing::Draw(IDeviceContext* context, Pass& pass, ITextureView* target, bool clear) {
  context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  if (clear) {
    const float zero[4] = {};
    context->ClearRenderTarget(target, zero, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  }
  const auto& desc = target->GetTexture()->GetDesc();
  Viewport viewport;
  viewport.Width = float(desc.Width);
  viewport.Height = float(desc.Height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(pass.pso);
  context->CommitShaderResources(pass.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
}

void Antialiasing::Apply(IDeviceContext* context, ITexture* texture, ITextureView* target) {
  if (mode_ == Mode::kOff || !texture || !target) return;
  const TextureDesc& desc = texture->GetDesc();
  Pass* output = OutputPass(desc.Format);
  if (!output || !EnsureTextures(desc.Width, desc.Height, desc.Format)) return;

  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    const float metrics[4] = {1.0f / float(desc.Width), 1.0f / float(desc.Height), float(desc.Width),
                              float(desc.Height)};
    std::memcpy(data, metrics, sizeof(metrics));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  // The input: a copy of the image, since the last pass writes over it. The resolve that made the image left
  // it bound as the render target: unbound first, or DiligentCore unbinds it with a message every frame.
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  CopyTextureAttribs copy{texture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, color_,
                          RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
  context->CopyTexture(copy);
  ITextureView* color = color_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);

  if (mode_ == Mode::kSmaa) {
    smaa_edges_.srb->GetVariableByName(SHADER_TYPE_PIXEL, "colorTex")->Set(color);
    Draw(context, smaa_edges_, edges_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), true);
    smaa_weights_.srb->GetVariableByName(SHADER_TYPE_PIXEL, "edgesTex")
        ->Set(edges_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    Draw(context, smaa_weights_, weights_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), true);
    output->srb->GetVariableByName(SHADER_TYPE_PIXEL, "colorTex")->Set(color);
    output->srb->GetVariableByName(SHADER_TYPE_PIXEL, "blendTex")
        ->Set(weights_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  } else {
    output->srb->GetVariableByName(SHADER_TYPE_PIXEL, "colorTex")->Set(color);
  }
  Draw(context, *output, target, false);
  // The image is sampled next (the composite): not left bound as the render target either.
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  ++applied_;
}

}  // namespace native_x_renderer
