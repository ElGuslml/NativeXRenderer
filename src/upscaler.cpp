// XenosNative - temporal antialiasing through NVIDIA DLSS (see upscaler.h).

#include "upscaler.h"

#include <rex/logging.h>

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "GraphicsAccessories.hpp"
#include "glsl.h"

#if XENOS_NATIVE_DLSS
#include "SuperResolution.h"
#include "SuperResolutionFactory.h"
#endif

namespace xenos_native {

using namespace Diligent;

namespace {

constexpr TEXTURE_FORMAT kMotionFormat = TEX_FORMAT_RG16_FLOAT;
constexpr uint32_t kJitterPhases = 64;

// The motion vectors: from a pixel's depth, its position in the world (this frame's matrix inverted, the
// jitter taken out), then where the last frame's matrix put that point. In host pixels, from this frame's
// position to the last frame's, as DLSS takes them. The game's matrices are rows for a row vector
// (clip = position * M); loaded as a GLSL mat4 (columns), M * v computes that product.
constexpr const char* kMotionVS = R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
constexpr const char* kMotionPS = R"(#version 450
layout(set = 0, binding = 0) uniform MotionConstants {
  mat4 inverse_current;  // clip -> world, this frame
  mat4 previous;         // world -> clip, the last frame
  vec4 size;             // width, height, 1 / width, 1 / height
  vec4 jitter;           // this frame's, in pixels (x right, y down)
  vec4 depth_range;      // the depth at the near plane, at the far plane (reversed if near > far)
};
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(location = 0) out vec2 out_motion;
void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  // The stored depth back to the projection's z (0 at the near plane, 1 at the far one).
  float depth = (texelFetch(depthTex, pixel, 0).r - depth_range.x) / (depth_range.y - depth_range.x);
  vec2 uv = (vec2(pixel) + 0.5 - jitter.xy) * size.zw;
  vec4 world = inverse_current * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  vec4 last = previous * (world / world.w);
  vec2 last_uv = vec2(last.x / last.w * 0.5 + 0.5, 0.5 - last.y / last.w * 0.5);
  out_motion = (last_uv - uv) * size.xy;
}
)";

struct MotionConstants {
  float inverse_current[16];
  float previous[16];
  float size[4];
  float jitter[4];
  float depth_range[4];
};

float Halton(uint32_t index, uint32_t base) {
  float result = 0.0f, fraction = 1.0f;
  while (index) {
    fraction /= float(base);
    result += fraction * float(index % base);
    index /= base;
  }
  return result;
}

// The inverse of a 4x4 matrix (rows); false if it is singular.
bool Invert(const std::array<float, 16>& m, float out[16]) {
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[r * 4 + c];
      a[r][c + 4] = r == c ? 1.0 : 0.0;
    }
  }
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r) {
      if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
    }
    if (std::abs(a[pivot][c]) < 1e-12) return false;
    if (pivot != c) std::swap(a[pivot], a[c]);
    double scale = 1.0 / a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] *= scale;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      double factor = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= factor * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = float(a[r][c + 4]);
  }
  return true;
}

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

Upscaler::Upscaler() = default;
Upscaler::~Upscaler() = default;

bool Upscaler::Initialize(IRenderDevice* device, uint32_t output_width, uint32_t output_height) {
  device_ = device;
#if XENOS_NATIVE_DLSS
  SuperResolutionFactoryCreateInfo create_info;
  create_info.pDevice = device;
  CreateSuperResolutionFactory(create_info, &factory_);
  if (!factory_) {
    REXLOG_WARN("[xenos_native] DLSS: DiligentCore's super resolution factory could not be created");
    return false;
  }
  Uint32 count = 0;
  factory_->EnumerateVariants(count, nullptr);
  std::vector<SuperResolutionInfo> variants(count);
  factory_->EnumerateVariants(count, variants.data());
  const SuperResolutionInfo* dlss = nullptr;
  for (const SuperResolutionInfo& variant : variants) {
    if (variant.Type == SUPER_RESOLUTION_TYPE_TEMPORAL &&
        std::string_view(variant.Name).find("DLSS") != std::string_view::npos) {
      dlss = &variant;
    }
  }
  if (!dlss) {
    REXLOG_INFO("[xenos_native] DLSS: not available on this GPU or driver ({} other upscalers)", count);
    return false;
  }
  static_assert(sizeof(variant_) == sizeof(INTERFACE_ID));
  std::memcpy(variant_.data(), &dlss->VariantId, sizeof(variant_));
  // The input sizes NGX recommends for each quality mode, for this output (the upscaling modes, later).
  static constexpr struct {
    SUPER_RESOLUTION_OPTIMIZATION_TYPE type;
    const char* name;
  } kModes[] = {{SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_QUALITY, "quality"},
                {SUPER_RESOLUTION_OPTIMIZATION_TYPE_BALANCED, "balanced"},
                {SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_PERFORMANCE, "performance"},
                {SUPER_RESOLUTION_OPTIMIZATION_TYPE_MAX_PERFORMANCE, "ultra performance"}};
  std::string modes;
  for (const auto& mode : kModes) {
    SuperResolutionSourceSettingsAttribs attribs;
    attribs.VariantId = dlss->VariantId;
    attribs.OutputWidth = output_width;
    attribs.OutputHeight = output_height;
    attribs.OptimizationType = mode.type;
    SuperResolutionSourceSettings settings;
    factory_->GetSourceSettings(attribs, settings);
    if (!modes.empty()) modes += ", ";
    modes += fmt::format("{} {}x{}", mode.name, settings.OptimalInputWidth, settings.OptimalInputHeight);
  }
  bool native = (dlss->TemporalCapFlags & SUPER_RESOLUTION_TEMPORAL_CAP_FLAG_NATIVE) != 0;
  REXLOG_INFO("[xenos_native] DLSS available ({}{}); for a {}x{} output: {}", dlss->Name,
              native ? ", native resolution (DLAA)" : "", output_width, output_height, modes);
  if (!native) return false;

  // The motion vector pass.
  BufferDesc constants;
  constants.Name = "XenosNative motion vector constants";
  constants.Size = sizeof(MotionConstants);
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  RefCntAutoPtr<IShader> vs = CreateShader(device, SHADER_TYPE_VERTEX, kMotionVS, "XenosNative motion vectors VS");
  RefCntAutoPtr<IShader> ps = CreateShader(device, SHADER_TYPE_PIXEL, kMotionPS, "XenosNative motion vectors PS");
  if (constants_ && vs && ps) {
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "XenosNative motion vectors";
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = 1;
    info.GraphicsPipeline.RTVFormats[0] = kMotionFormat;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    info.pVS = vs;
    info.pPS = ps;
    ShaderResourceVariableDesc variable{SHADER_TYPE_PIXEL, "depthTex", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC};
    SamplerDesc point;
    point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
    point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
    ImmutableSamplerDesc sampler{SHADER_TYPE_PIXEL, "depthTex", point};
    info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    info.PSODesc.ResourceLayout.Variables = &variable;
    info.PSODesc.ResourceLayout.NumVariables = 1;
    info.PSODesc.ResourceLayout.ImmutableSamplers = &sampler;
    info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
    device->CreateGraphicsPipelineState(info, &motion_pso_);
    if (motion_pso_) {
      motion_pso_->GetStaticVariableByName(SHADER_TYPE_PIXEL, "MotionConstants")->Set(constants_);
      motion_pso_->CreateShaderResourceBinding(&motion_srb_, true);
    }
  }
  if (!motion_srb_) {
    REXLOG_ERROR("[xenos_native] DLAA: the motion vector pass could not be set up");
    return false;
  }
  available_ = true;
  NextFrame();
  return true;
#else
  (void)output_width;
  (void)output_height;
  REXLOG_INFO("[xenos_native] DLSS: not in this build");
  return false;
#endif
}

void Upscaler::NextFrame() {
  ++frame_;
  uint32_t phase = frame_ % kJitterPhases + 1;
  jitter_[0] = Halton(phase, 2) - 0.5f;
  jitter_[1] = Halton(phase, 3) - 0.5f;
}

bool Upscaler::EnsureResources(ITexture* color, ITexture* depth) {
#if XENOS_NATIVE_DLSS
  const TextureDesc& color_desc = color->GetDesc();
  TEXTURE_FORMAT depth_format = depth->GetDesc().Format;
  if (dlss_) {
    const SuperResolutionDesc& desc = dlss_->GetDesc();
    if (desc.InputWidth == color_desc.Width && desc.InputHeight == color_desc.Height &&
        desc.ColorFormat == color_desc.Format && desc.DepthFormat == depth_format) {
      return true;
    }
    dlss_.Release();
  }
  SuperResolutionDesc desc;
  desc.Name = "XenosNative DLAA";
  std::memcpy(&desc.VariantId, variant_.data(), sizeof(variant_));
  desc.InputWidth = desc.OutputWidth = color_desc.Width;
  desc.InputHeight = desc.OutputHeight = color_desc.Height;
  desc.ColorFormat = desc.OutputFormat = color_desc.Format;
  desc.DepthFormat = depth_format;
  desc.MotionFormat = kMotionFormat;
  // The scene before the game's post-processing is HDR, with no exposure of its own to give.
  desc.Flags = SUPER_RESOLUTION_FLAG_AUTO_EXPOSURE;
  factory_->CreateSuperResolution(desc, &dlss_);

  auto create = [&](RefCntAutoPtr<ITexture>& texture, const char* name, TEXTURE_FORMAT format, BIND_FLAGS bind) {
    texture.Release();
    TextureDesc texture_desc;
    texture_desc.Name = name;
    texture_desc.Type = RESOURCE_DIM_TEX_2D;
    texture_desc.Width = color_desc.Width;
    texture_desc.Height = color_desc.Height;
    texture_desc.Format = format;
    texture_desc.BindFlags = bind;
    device_->CreateTexture(texture_desc, nullptr, &texture);
    return texture != nullptr;
  };
  bool ok = dlss_ && create(motion_, "XenosNative motion vectors", kMotionFormat, BIND_SHADER_RESOURCE | BIND_RENDER_TARGET) &&
            create(output_, "XenosNative DLAA output", color_desc.Format, BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS);
  if (ok) {
    REXLOG_INFO("[xenos_native] DLAA: {}x{}, color {}, depth {}", color_desc.Width, color_desc.Height,
                GetTextureFormatAttribs(color_desc.Format).Name, GetTextureFormatAttribs(depth_format).Name);
  }
  last_applied_frame_ = UINT32_MAX;  // new resources: no history
  return ok;
#else
  (void)color;
  (void)depth;
  return false;
#endif
}

bool Upscaler::Apply(IDeviceContext* context, ITexture* color, ITexture* depth, ITextureView* depth_srv,
                     const std::array<float, 16>& view_projection, float z_min, float z_max) {
#if XENOS_NATIVE_DLSS
  if (!available_ || failed_ || !color || !depth || !depth_srv) return false;
  if (!EnsureResources(color, depth)) {
    REXLOG_ERROR("[xenos_native] DLAA: could not be set up; the scene is not antialiased");
    failed_ = true;
    return false;
  }
  const TextureDesc& desc = color->GetDesc();
  // History is kept only from the frame before: after a frame without the view (menus, loads), DLSS starts
  // again, and the motion vectors are zero.
  bool reset = last_applied_frame_ == UINT32_MAX || last_applied_frame_ + 1 != frame_;

  MotionConstants constants{};
  if (reset || !Invert(view_projection, constants.inverse_current)) {
    reset = true;
    Invert(view_projection, constants.inverse_current);
    std::memcpy(constants.previous, view_projection.data(), sizeof(constants.previous));
  } else {
    std::memcpy(constants.previous, last_view_projection_.data(), sizeof(constants.previous));
  }
  constants.size[0] = float(desc.Width);
  constants.size[1] = float(desc.Height);
  constants.size[2] = 1.0f / float(desc.Width);
  constants.size[3] = 1.0f / float(desc.Height);
  constants.jitter[0] = jitter_[0];
  constants.jitter[1] = jitter_[1];
  constants.depth_range[0] = z_min;
  constants.depth_range[1] = z_max != z_min ? z_max : z_min + 1.0f;
  {
    void* data = nullptr;
    context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, &constants, sizeof(constants));
    context->UnmapBuffer(constants_, MAP_WRITE);
  }
  ITextureView* motion_rtv = motion_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  context->SetRenderTargets(1, &motion_rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Viewport viewport;
  viewport.Width = float(desc.Width);
  viewport.Height = float(desc.Height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(motion_pso_);
  motion_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "depthTex")->Set(depth_srv);
  context->CommitShaderResources(motion_srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);

  ExecuteSuperResolutionAttribs attribs;
  attribs.pContext = context;
  attribs.pColorTextureSRV = color->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  attribs.pDepthTextureSRV = depth_srv;
  attribs.pMotionVectorsSRV = motion_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  attribs.pOutputTextureView = output_->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
  attribs.JitterX = jitter_[0];
  attribs.JitterY = jitter_[1];
  attribs.ResetHistory = reset ? True : False;
  // Reversed depth (the near plane above the far one): DiligentCore tells NGX when CameraNear > CameraFar.
  if (z_min > z_max) {
    attribs.CameraNear = 1.0f;
    attribs.CameraFar = 0.0f;
  }
  dlss_->Execute(attribs);
  // NGX recorded its own compute pipelines and bindings into the command buffer, behind DiligentCore's
  // back: its cache of what is bound is no longer true (the texture loads also use compute pipelines). The
  // commands are submitted first (DiligentCore warns when a context with pending commands is invalidated),
  // then the cache is cleared; the draws that follow bind everything again. A duration query (the GPU
  // timer) may span the submission: on Vulkan it is two timestamps.
  context->Flush();
  context->InvalidateState();
  CopyTextureAttribs copy{output_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, color,
                          RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
  context->CopyTexture(copy);

  last_view_projection_ = view_projection;
  last_applied_frame_ = frame_;
  ++applied_;
  return true;
#else
  (void)context;
  (void)color;
  (void)depth;
  (void)depth_srv;
  (void)view_projection;
  (void)z_min;
  (void)z_max;
  return false;
#endif
}

}  // namespace xenos_native
