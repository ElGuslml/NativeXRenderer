// NativeXRenderer - see scaling.h.

#include "scaling.h"

#include "glsl.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "GraphicsAccessories.hpp"
#include "NIS_Config.h"
#include "scaling_sources.h"  // kFfxASource, kFsr1Source, kCasSource, kNisScalerSource: embedded by CMake

namespace native_x_renderer {

using namespace Diligent;

namespace {

// One triangle covering the viewport (the output rectangle); the pixel shaders work from gl_FragCoord.
constexpr const char* kVertexShader = R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The constants every pixel pass reads, and its output. AMD's headers compute their own constants from them
// (FsrEasuCon, FsrRcasCon, CasSetup work on the GPU as well).
constexpr const char* kPixelCommon = R"(
layout(set = 0, binding = 0) uniform ScalingConstants {
  ivec4 origin;  // xy: the output rectangle's first pixel in its target
  vec4 size;     // xy: the input image's size (the top left of its texture), zw: the output rectangle's
  vec4 param;    // x: the pass's sharpness (CAS: 0 to 1; RCAS: stops, 0 the sharpest)
};
layout(set = 0, binding = 1) uniform sampler2D source;
layout(location = 0) out vec4 out_color;
// The output pixel this fragment is, from the rectangle's top left.
uvec2 OutputPixel() { return uvec2(ivec2(gl_FragCoord.xy) - origin.xy); }
// An input pixel, clamped to the image: the passes read their neighbors at its edges.
vec4 LoadClamped(ivec2 p) { return texelFetch(source, clamp(p, ivec2(0), ivec2(size.xy) - 1), 0); }
)";

constexpr const char* kEasuCallbacks = R"(
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(source, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(source, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(source, p, 2); }
)";
constexpr const char* kEasuMain = R"(
void main() {
  AU4 con0, con1, con2, con3;
  vec2 texture_size = vec2(textureSize(source, 0));
  FsrEasuCon(con0, con1, con2, con3, size.x, size.y, texture_size.x, texture_size.y, size.z, size.w);
  AF3 c;
  FsrEasuF(c, OutputPixel(), con0, con1, con2, con3);
  out_color = vec4(c, 1.0);
}
)";

constexpr const char* kRcasCallbacks = R"(
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return LoadClamped(p); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
)";
constexpr const char* kRcasMain = R"(
void main() {
  AU4 con;
  FsrRcasCon(con, param.x);
  AF1 r, g, b;
  FsrRcasF(r, g, b, OutputPixel(), con);
  out_color = vec4(r, g, b, 1.0);
}
)";

constexpr const char* kCasCallbacks = R"(
AF3 CasLoad(ASU2 p) { return LoadClamped(p).rgb; }
void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) {}
)";
constexpr const char* kCasMain = R"(
void main() {
  AU4 const0, const1;
  CasSetup(const0, const1, param.x, size.x, size.y, size.z, size.w);
  AF1 r, g, b;
  CasFilter(r, g, b, OutputPixel(), const0, const1, size.xy == size.zw);
  out_color = vec4(r, g, b, 1.0);
}
)";

// NVIDIA's NIS_Main.glsl, with the output as a 16-bit float storage image and without the 16-bit extensions (full
// precision: NIS_USE_HALF_PRECISION 0).
constexpr const char* kNisDeclarations = R"(
layout(set = 0, binding = 0) uniform const_buffer {
  float kDetectRatio;
  float kDetectThres;
  float kMinContrastRatio;
  float kRatioNorm;
  float kContrastBoost;
  float kEps;
  float kSharpStartY;
  float kSharpScaleY;
  float kSharpStrengthMin;
  float kSharpStrengthScale;
  float kSharpLimitMin;
  float kSharpLimitScale;
  float kScaleX;
  float kScaleY;
  float kDstNormX;
  float kDstNormY;
  float kSrcNormX;
  float kSrcNormY;
  uint kInputViewportOriginX;
  uint kInputViewportOriginY;
  uint kInputViewportWidth;
  uint kInputViewportHeight;
  uint kOutputViewportOriginX;
  uint kOutputViewportOriginY;
  uint kOutputViewportWidth;
  uint kOutputViewportHeight;
  float reserved0;
  float reserved1;
};
layout(set = 0, binding = 1) uniform sampler samplerLinearClamp;
layout(set = 0, binding = 2) uniform texture2D in_texture;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D out_texture;
#if NIS_SCALER
layout(set = 0, binding = 4) uniform texture2D coef_scaler;
layout(set = 0, binding = 5) uniform texture2D coef_usm;
#endif
)";
constexpr const char* kNisMain = R"(
layout(local_size_x = NIS_THREAD_GROUP_SIZE) in;
void main() {
#if NIS_SCALER
  NVScaler(gl_WorkGroupID.xy, gl_LocalInvocationID.x);
#else
  NVSharpen(gl_WorkGroupID.xy, gl_LocalInvocationID.x);
#endif
}
)";

const char* PassName(uint8_t pass) {
  static constexpr const char* kNames[] = {"FSR 1 EASU", "FSR 1 RCAS", "CAS", "NIS scaler", "NIS sharpen"};
  return pass < std::size(kNames) ? kNames[pass] : "?";
}

std::string FfxPixelShader(const char* callbacks, const char* header, const char* main) {
  std::string source = "#version 450\n#define A_GPU 1\n#define A_GLSL 1\n";
  source += kFfxASource;
  source += kPixelCommon;
  source += callbacks;
  source += header;
  source += main;
  return source;
}

RefCntAutoPtr<IShader> CreateShader(IRenderDevice* device, SHADER_TYPE type, const std::string& source,
                                    const char* name, bool combined_samplers) {
  RefCntAutoPtr<IShader> shader;
  GlslStage stage = type == SHADER_TYPE_VERTEX    ? GlslStage::kVertex
                    : type == SHADER_TYPE_COMPUTE ? GlslStage::kCompute
                                                  : GlslStage::kPixel;
  std::vector<uint32_t> spirv = CompileGlsl(stage, source, name);
  if (spirv.empty()) return shader;
  ShaderCreateInfo info;
  info.Desc.Name = name;
  info.Desc.ShaderType = type;
  info.Desc.UseCombinedTextureSamplers = combined_samplers;
  info.ByteCode = spirv.data();
  info.ByteCodeSize = spirv.size() * sizeof(uint32_t);
  device->CreateShader(info, &shader);
  return shader;
}

SamplerDesc LinearClamp() {
  SamplerDesc linear;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  return linear;
}

}  // namespace

Scaling::Filter Scaling::ParseFilter(std::string_view name) {
  if (name == "fsr") return Filter::kFsr;
  if (name == "nis") return Filter::kNis;
  if (name != "bilinear") REXLOG_WARN("[native_x_renderer] scaling \"{}\" unknown: bilinear", name);
  return Filter::kBilinear;
}

const char* Scaling::FilterName(Filter filter) {
  switch (filter) {
    case Filter::kFsr:
      return "AMD FSR 1";
    case Filter::kNis:
      return "NVIDIA Image Scaling";
    default:
      return "bilinear";
  }
}

bool Scaling::Initialize(IRenderDevice* device, Filter filter, float sharpness) {
  device_ = device;
  filter_ = filter;
  switch (filter_) {
    case Filter::kFsr:
      // AMD's default is 0.2 stops; 0 leaves RCAS out, 1 is RCAS's sharpest (0 stops).
      rcas_stops_ = sharpness < 0.0f ? 0.2f : sharpness == 0.0f ? -1.0f : 2.0f * (1.0f - std::min(sharpness, 1.0f));
      break;
    case Filter::kNis:
      nis_sharpness_ = sharpness < 0.0f ? 0.5f : std::min(sharpness, 1.0f);
      break;
    default:
      cas_sharpness_ = sharpness > 0.0f ? std::min(sharpness, 1.0f) : -1.0f;
      break;
  }
  active_ = filter_ != Filter::kBilinear || cas_sharpness_ > 0.0f;
  if (!active_) {
    REXLOG_INFO("[native_x_renderer] scaling to the window: bilinear, not sharpened");
    return true;
  }

  BufferDesc constants;
  constants.Name = "NativeXRenderer scaling constants";
  constants.Size = 48;
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device_->CreateBuffer(constants, nullptr, &constants_);
  vertex_shader_ = CreateShader(device_, SHADER_TYPE_VERTEX, kVertexShader, "NativeXRenderer scaling VS", true);
  bool ok = constants_ && vertex_shader_;
  if (ok && filter_ == Filter::kNis) {
    // NVIDIA's block and thread group sizes for the vendor (NIS_Config.h's NISOptimizer).
    NISGPUArchitecture arch = NISGPUArchitecture::NVIDIA_Generic;
    switch (device_->GetAdapterInfo().Vendor) {
      case ADAPTER_VENDOR_AMD:
        arch = NISGPUArchitecture::AMD_Generic;
        break;
      case ADAPTER_VENDOR_INTEL:
        arch = NISGPUArchitecture::Intel_Generic;
        break;
      default:
        break;
    }
    NISOptimizer scaler(true, arch), sharpen(false, arch);
    nis_block_width_ = scaler.GetOptimalBlockWidth();
    nis_block_height_[0] = scaler.GetOptimalBlockHeight();
    nis_block_height_[1] = sharpen.GetOptimalBlockHeight();
    nis_threads_ = scaler.GetOptimalThreadGroupSize();
    constants.Name = "NativeXRenderer NIS constants";
    constants.Size = sizeof(NISConfig);
    device_->CreateBuffer(constants, nullptr, &nis_constants_);
    // The filter coefficients: kPhaseCount rows of kFilterSize floats, as RGBA32F textures kFilterSize / 4 wide.
    auto coefficients = [&](RefCntAutoPtr<ITexture>& texture, const char* name, const float (*data)[kFilterSize]) {
      TextureDesc desc;
      desc.Name = name;
      desc.Type = RESOURCE_DIM_TEX_2D;
      desc.Width = uint32_t(kFilterSize / 4);
      desc.Height = uint32_t(kPhaseCount);
      desc.Format = TEX_FORMAT_RGBA32_FLOAT;
      desc.Usage = USAGE_IMMUTABLE;
      desc.BindFlags = BIND_SHADER_RESOURCE;
      TextureSubResData subresource{data, uint64_t(kFilterSize * sizeof(float))};
      TextureData initial{&subresource, 1};
      device_->CreateTexture(desc, &initial, &texture);
      return texture != nullptr;
    };
    ok = nis_constants_ && coefficients(nis_coef_scaler_, "NativeXRenderer NIS scaler coefficients", coef_scale) &&
         coefficients(nis_coef_usm_, "NativeXRenderer NIS sharpening coefficients", coef_usm);
  }
  // The shaders are compiled now (their pipelines when first drawn, per target format), so a failure shows at once.
  if (ok) {
    std::vector<Pass> passes;
    if (filter_ == Filter::kFsr) passes = {Pass::kEasu, Pass::kRcas};
    if (filter_ == Filter::kNis) passes = {Pass::kNisScaler, Pass::kNisSharpen};
    if (filter_ == Filter::kBilinear) passes = {Pass::kCas};
    for (Pass pass : passes) {
      if (!Shader(pass)) ok = false;
    }
  }
  if (!ok) {
    REXLOG_ERROR("[native_x_renderer] scaling ({}) could not be set up: bilinear instead", FilterName(filter_));
    active_ = false;
    return false;
  }
  std::string sharpening = filter_ == Filter::kFsr
                               ? (rcas_stops_ < 0.0f ? std::string("without RCAS")
                                                     : fmt::format("RCAS at {:.2f} stops", rcas_stops_))
                           : filter_ == Filter::kNis ? fmt::format("sharpness {:.2f}", nis_sharpness_)
                                                     : fmt::format("AMD CAS at {:.2f}", cas_sharpness_);
  REXLOG_INFO("[native_x_renderer] scaling to the window: {}, {}", FilterName(filter_), sharpening);
  return true;
}

IShader* Scaling::Shader(Pass pass) {
  RefCntAutoPtr<IShader>& shader = shaders_[size_t(pass)];
  if (shader) return shader;
  std::string name = std::string("NativeXRenderer ") + PassName(uint8_t(pass));
  if (pass == Pass::kNisScaler || pass == Pass::kNisSharpen) {
    bool scaler = pass == Pass::kNisScaler;
    std::string source = fmt::format(
        "#version 450\n#define NIS_GLSL 1\n#define NIS_SCALER {}\n#define NIS_USE_HALF_PRECISION 0\n"
        "#define NIS_BLOCK_WIDTH {}\n#define NIS_BLOCK_HEIGHT {}\n#define NIS_THREAD_GROUP_SIZE {}\n",
        scaler ? 1 : 0, nis_block_width_, nis_block_height_[scaler ? 0 : 1], nis_threads_);
    source += kNisDeclarations;
    source += kNisScalerSource;
    source += kNisMain;
    shader = CreateShader(device_, SHADER_TYPE_COMPUTE, source, name.c_str(), false);
  } else {
    std::string source = pass == Pass::kEasu   ? FfxPixelShader(kEasuCallbacks, kFsr1Source, kEasuMain)
                         : pass == Pass::kRcas ? FfxPixelShader(kRcasCallbacks, kFsr1Source, kRcasMain)
                                               : FfxPixelShader(kCasCallbacks, kCasSource, kCasMain);
    shader = CreateShader(device_, SHADER_TYPE_PIXEL, source, name.c_str(), true);
  }
  return shader;
}

Scaling::Pipeline* Scaling::GetPipeline(Pass pass, TEXTURE_FORMAT target_format) {
  bool compute = pass == Pass::kNisScaler || pass == Pass::kNisSharpen;
  if (compute) target_format = TEX_FORMAT_UNKNOWN;  // a storage image, always RGBA16F
  auto key = std::make_tuple(pass, target_format);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second.pso ? &it->second : nullptr;
  Pipeline& pipeline = pipelines_[key];
  std::string pso_name = std::string("NativeXRenderer ") + PassName(uint8_t(pass));
  IShader* shader = Shader(pass);
  if (!shader) return nullptr;

  if (compute) {
    bool scaler = pass == Pass::kNisScaler;
    ComputePipelineStateCreateInfo info;
    info.PSODesc.Name = pso_name.c_str();
    info.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    info.pCS = shader;
    ShaderResourceVariableDesc variables[] = {
        {SHADER_TYPE_COMPUTE, "in_texture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_COMPUTE, "out_texture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
    };
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_COMPUTE, "samplerLinearClamp", LinearClamp()}};
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    info.PSODesc.ResourceLayout.Variables = variables;
    info.PSODesc.ResourceLayout.NumVariables = uint32_t(std::size(variables));
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(std::size(samplers));
    device_->CreateComputePipelineState(info, &pipeline.pso);
    if (!pipeline.pso) {
      REXLOG_ERROR("[native_x_renderer] {}: no pipeline", pso_name);
      return nullptr;
    }
    if (auto* variable = pipeline.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "const_buffer")) {
      variable->Set(nis_constants_);
    }
    if (scaler) {
      if (auto* variable = pipeline.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "coef_scaler")) {
        variable->Set(nis_coef_scaler_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
      }
      if (auto* variable = pipeline.pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "coef_usm")) {
        variable->Set(nis_coef_usm_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
      }
    }
  } else {
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = pso_name.c_str();
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = 1;
    info.GraphicsPipeline.RTVFormats[0] = target_format;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    info.pVS = vertex_shader_;
    info.pPS = shader;
    ShaderResourceVariableDesc variables[] = {{SHADER_TYPE_PIXEL, "source", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "source", LinearClamp()}};
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    info.PSODesc.ResourceLayout.Variables = variables;
    info.PSODesc.ResourceLayout.NumVariables = 1;
    info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
    device_->CreateGraphicsPipelineState(info, &pipeline.pso);
    if (!pipeline.pso) {
      REXLOG_ERROR("[native_x_renderer] {}: no pipeline", pso_name);
      return nullptr;
    }
    if (auto* variable = pipeline.pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "ScalingConstants")) {
      variable->Set(constants_);
    }
  }
  pipeline.pso->CreateShaderResourceBinding(&pipeline.srb, true);
  return pipeline.srb ? &pipeline : nullptr;
}

ITexture* Scaling::Intermediate(uint32_t slot, Size size) {
  RefCntAutoPtr<ITexture>& texture = intermediates_[slot];
  if (texture && texture->GetDesc().Width == size.width && texture->GetDesc().Height == size.height) return texture;
  texture.Release();
  TextureDesc desc;
  desc.Name = "NativeXRenderer scaling intermediate";
  desc.Type = RESOURCE_DIM_TEX_2D;
  desc.Width = size.width;
  desc.Height = size.height;
  desc.Format = TEX_FORMAT_RGBA16_FLOAT;
  desc.Usage = USAGE_DEFAULT;
  desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
  device_->CreateTexture(desc, nullptr, &texture);
  return texture;
}

std::vector<Scaling::Size> Scaling::Steps(Size in, Size out) {
  std::vector<Size> steps;
  if (out.width < in.width || out.height < in.height || out == in) return steps;
  // As few passes as keep each within 2x per axis, the ratio shared evenly between them.
  double rx = double(out.width) / in.width, ry = double(out.height) / in.height;
  int passes = std::max(1, int(std::ceil(std::log2(std::max(rx, ry)) - 1e-9)));
  for (int i = 1; i < passes; ++i) {
    double t = double(i) / passes;
    steps.push_back({uint32_t(std::lround(in.width * std::pow(rx, t))), uint32_t(std::lround(in.height * std::pow(ry, t)))});
  }
  steps.push_back(out);
  return steps;
}

void Scaling::DrawPass(IDeviceContext* context, Pass pass, ITextureView* source, Size in, ITextureView* target,
                       const BlitRect& to, float param) {
  Pipeline* pipeline = GetPipeline(pass, target->GetDesc().Format);
  if (!pipeline) return;
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    struct {
      int32_t origin[4];
      float size[4];
      float param[4];
    } c{{to.x, to.y, 0, 0},
        {float(in.width), float(in.height), float(to.width), float(to.height)},
        {param, 0.0f, 0.0f, 0.0f}};
    std::memcpy(data, &c, sizeof(c));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  // The target first: an intermediate this pass reads may be the one the last pass wrote.
  context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  pipeline->srb->GetVariableByName(SHADER_TYPE_PIXEL, "source")->Set(source);
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
  context->CommitShaderResources(pipeline->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
}

void Scaling::DispatchNis(IDeviceContext* context, ITextureView* source, Size in, Size out, ITexture* target,
                          float sharpness) {
  bool scaler = !(in == out);
  Pipeline* pipeline = GetPipeline(scaler ? Pass::kNisScaler : Pass::kNisSharpen, TEX_FORMAT_UNKNOWN);
  if (!pipeline) return;
  const TextureDesc& source_desc = source->GetTexture()->GetDesc();
  NISConfig config{};
  if (scaler) {
    NVScalerUpdateConfig(config, sharpness, 0, 0, in.width, in.height, source_desc.Width, source_desc.Height, 0, 0,
                         out.width, out.height, out.width, out.height, NISHDRMode::None);
  } else {
    NVSharpenUpdateConfig(config, sharpness, 0, 0, in.width, in.height, source_desc.Width, source_desc.Height, 0, 0,
                          NISHDRMode::None);
  }
  {
    void* data = nullptr;
    context->MapBuffer(nis_constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &config, sizeof(config));
    context->UnmapBuffer(nis_constants_, MAP_WRITE);
  }
  // Nothing left bound as a render target: the storage image may have been one.
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  pipeline->srb->GetVariableByName(SHADER_TYPE_COMPUTE, "in_texture")->Set(source);
  pipeline->srb->GetVariableByName(SHADER_TYPE_COMPUTE, "out_texture")
      ->Set(target->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
  context->SetPipelineState(pipeline->pso);
  context->CommitShaderResources(pipeline->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  uint32_t block_height = nis_block_height_[scaler ? 0 : 1];
  DispatchComputeAttribs dispatch((out.width + nis_block_width_ - 1) / nis_block_width_,
                                  (out.height + block_height - 1) / block_height, 1);
  context->DispatchCompute(dispatch);
}

bool Scaling::Apply(IDeviceContext* context, Blitter& blit, ITextureView* source, uint32_t width, uint32_t height,
                    ITextureView* target, const BlitRect& to) {
  if (!active_ || !source || !target || !width || !height || !to.width || !to.height) return false;
  Size in{width, height}, out{to.width, to.height};
  bool same = in == out;
  bool shrinks = out.width < in.width || out.height < in.height;
  std::string passes;  // for the log
  switch (filter_) {
    case Filter::kFsr: {
      if (shrinks || (same && rcas_stops_ < 0.0f)) return false;
      std::vector<Size> steps = Steps(in, out);
      ITextureView* image = source;
      Size size = in;
      for (size_t i = 0; i < steps.size(); ++i) {
        if (i + 1 == steps.size() && rcas_stops_ < 0.0f) {
          DrawPass(context, Pass::kEasu, image, size, target, to, 0.0f);
          image = nullptr;
          break;
        }
        ITexture* intermediate = Intermediate(uint32_t(i % 3), steps[i]);
        if (!intermediate) return false;
        DrawPass(context, Pass::kEasu, image, size, intermediate->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET),
                 {0, 0, steps[i].width, steps[i].height}, 0.0f);
        image = intermediate->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        size = steps[i];
      }
      if (image) DrawPass(context, Pass::kRcas, image, size, target, to, rcas_stops_);
      passes = fmt::format("{} EASU pass{}{}", steps.size(), steps.size() == 1 ? "" : "es",
                           rcas_stops_ < 0.0f ? "" : ", RCAS");
      break;
    }
    case Filter::kNis: {
      if (shrinks || (same && nis_sharpness_ <= 0.0f)) return false;
      std::vector<Size> steps = same ? std::vector<Size>{in} : Steps(in, out);
      ITextureView* image = source;
      Size size = in;
      for (size_t i = 0; i < steps.size(); ++i) {
        ITexture* intermediate = Intermediate(uint32_t(i % 3), steps[i]);
        if (!intermediate) return false;
        // Sharpened in the last pass only.
        DispatchNis(context, image, size, steps[i], intermediate, i + 1 == steps.size() ? nis_sharpness_ : 0.0f);
        image = intermediate->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        size = steps[i];
      }
      blit.Copy(context, image, Blitter::Source::kColor, {0, 0, size.width, size.height}, target, to);
      passes = same ? std::string("NVSharpen")
                    : fmt::format("{} NVScaler pass{}", steps.size(), steps.size() == 1 ? "" : "es");
      break;
    }
    default: {
      // CAS scales by itself up to 4x the pixels (CAS_AREA_LIMIT); beyond that the image is stretched first.
      if (double(out.width) * out.height <= 4.0 * double(in.width) * in.height) {
        DrawPass(context, Pass::kCas, source, in, target, to, cas_sharpness_);
        passes = same ? "CAS" : "CAS, scaling";
      } else {
        ITexture* intermediate = Intermediate(0, out);
        if (!intermediate) return false;
        blit.Copy(context, source, Blitter::Source::kColorScaled, {0, 0, in.width, in.height},
                  intermediate->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET), {0, 0, out.width, out.height});
        DrawPass(context, Pass::kCas, intermediate->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), out, target, to,
                 cas_sharpness_);
        passes = "bilinear, then CAS";
      }
      break;
    }
  }
  if (!logged_ || !(logged_in_ == in) || !(logged_out_ == out)) {
    logged_ = true;
    logged_in_ = in;
    logged_out_ = out;
    REXLOG_INFO("[native_x_renderer] scaling: {}x{} into {}x{} with {}: {}", in.width, in.height, out.width, out.height,
                FilterName(filter_), passes);
  }
  ++applied_;
  return true;
}

}  // namespace native_x_renderer
