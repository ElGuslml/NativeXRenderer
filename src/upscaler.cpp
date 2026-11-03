// NativeXRenderer - temporal antialiasing through NVIDIA DLSS (see upscaler.h).

#include "upscaler.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "GraphicsAccessories.hpp"
#include "fsr.h"
#include "glsl.h"
#include "motion.h"

#if NATIVE_X_RENDERER_DLSS
#include "SuperResolution.h"
#include "SuperResolutionFactory.h"
#endif

namespace native_x_renderer {

using namespace Diligent;

namespace {

constexpr TEXTURE_FORMAT kMotionFormat = kMotionVectorFormat;

// The motion vectors: from a pixel's depth (the jitter taken out), where the last frame's matrix put the same point. In
// host pixels, from this frame's position to the last frame's, as DLSS takes them. One matrix takes this frame's clip
// position to the last frame's: this frame's matrix inverted, then the last frame's, multiplied on the CPU in double
// precision (D117). The game's matrices are rows for a row vector (clip = position * M); loaded as a GLSL mat4
// (columns), M * v computes that product.
constexpr const char* kMotionVS = R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
constexpr const char* kMotionPS = R"(#version 450
layout(set = 0, binding = 0) uniform MotionConstants {
  mat4 reprojection;  // clip, this frame -> clip, the last frame
  vec4 size;          // width, height, 1 / width, 1 / height
  vec4 jitter;        // this frame's, in pixels (x right, y down)
  vec4 depth_range;   // the depth at the near plane, at the far plane (reversed if near > far)
};
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(location = 0) out vec2 out_motion;
void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  // The stored depth back to the projection's z (0 at the near plane, 1 at the far one).
  float depth = (texelFetch(depthTex, pixel, 0).r - depth_range.x) / (depth_range.y - depth_range.x);
  vec2 uv = (vec2(pixel) + 0.5 - jitter.xy) * size.zw;
  vec4 last = reprojection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  vec2 last_uv = vec2(last.x / last.w * 0.5 + 0.5, 0.5 - last.y / last.w * 0.5);
  out_motion = (last_uv - uv) * size.xy;
}
)";

// FSR 3.1 (D116): the view's depth as R32_FLOAT, FidelityFX's Vulkan backend making its own views of the images it
// gets (a depth-stencil image would need its depth aspect). Brought to the projection's z with the view's depth range,
// as the motion vectors read it: 0 at the near plane, 1 at the far one, whichever way the game stores it.
constexpr const char* kDepthCopyPS = R"(#version 450
layout(set = 0, binding = 0) uniform MotionConstants {
  mat4 reprojection;
  vec4 size;
  vec4 jitter;
  vec4 depth_range;
};
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(location = 0) out float out_depth;
void main() {
  out_depth = (texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r - depth_range.x) / (depth_range.y - depth_range.x);
}
)";

struct MotionConstants {
  float reprojection[16];
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
bool Invert(const std::array<float, 16>& m, double out[16]) {
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
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
  }
  return true;
}

// The camera's near and far planes and vertical field of view (FSR 3.1's heuristics), from the view-projection matrix
// (rows for a row vector: column c is m[c], m[4 + c], m[8 + c], m[12 + c]). With a perspective projection after a
// rigid view, the w column is the view's z axis, the z column a times that plus b in w (clip z = a view z + b: D3D's
// a = f / (f - n), b = -n f / (f - n); reversed depth swaps n and f), and the y column's length over the w column's
// is the projection's cot(fov / 2). Left as they are when the matrix is not one.
void CameraFromViewProjection(const std::array<float, 16>& m, float& near_plane, float& far_plane, float& fov_y) {
  double y[4], z[4], w[4];
  for (int r = 0; r < 4; ++r) {
    y[r] = m[r * 4 + 1];
    z[r] = m[r * 4 + 2];
    w[r] = m[r * 4 + 3];
  }
  double ww = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
  double yy = y[0] * y[0] + y[1] * y[1] + y[2] * y[2];
  if (ww < 1e-12 || yy < 1e-12) return;
  double a = (z[0] * w[0] + z[1] * w[1] + z[2] * w[2]) / ww;
  double b = z[3] - a * w[3];
  if (std::abs(a) < 1e-9 || std::abs(1.0 - a) < 1e-9) return;
  double n = -b / a, f = b / (1.0 - a);
  if (n > f) std::swap(n, f);
  if (!(n > 0.0) || !(f > n) || !std::isfinite(f)) return;
  near_plane = float(n);
  far_plane = float(f);
  fov_y = float(2.0 * std::atan(std::sqrt(ww / yy)));
}

RefCntAutoPtr<ITexture> CreateTexture(IRenderDevice* device, const char* name, uint32_t width, uint32_t height,
                                      TEXTURE_FORMAT format, BIND_FLAGS bind) {
  TextureDesc desc;
  desc.Name = name;
  desc.Type = RESOURCE_DIM_TEX_2D;
  desc.Width = width;
  desc.Height = height;
  desc.Format = format;
  desc.BindFlags = bind;
  RefCntAutoPtr<ITexture> texture;
  device->CreateTexture(desc, nullptr, &texture);
  return texture;
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

const char* Upscaler::ModeName(Provider provider, Mode mode) {
  if (provider == Provider::kFsr) {
    switch (mode) {
      case Mode::kDlaa: return "FSR 3.1 Native AA";
      case Mode::kQuality: return "FSR 3.1 Quality";
      case Mode::kBalanced: return "FSR 3.1 Balanced";
      case Mode::kPerformance: return "FSR 3.1 Performance";
      case Mode::kUltraPerformance: return "FSR 3.1 Ultra Performance";
    }
    return "FSR 3.1";
  }
  switch (mode) {
    case Mode::kDlaa: return "DLAA";
    case Mode::kQuality: return "DLSS Quality";
    case Mode::kBalanced: return "DLSS Balanced";
    case Mode::kPerformance: return "DLSS Performance";
    case Mode::kUltraPerformance: return "DLSS Ultra Performance";
  }
  return "DLSS";
}

bool Upscaler::Initialize(IRenderDevice* device, uint32_t output_width, uint32_t output_height, Mode mode,
                          Provider provider) {
  device_ = device;
  mode_ = mode;
  provider_ = provider;
  output_size_[0] = input_size_[0] = output_width;
  output_size_[1] = input_size_[1] = output_height;
  if (!(provider_ == Provider::kFsr ? InitializeFsr(output_width, output_height)
                                    : InitializeDlss(output_width, output_height))) {
    return false;
  }

  // The motion vector pass, and FSR's depth copy: full-screen passes reading the view's depth (point sampled).
  BufferDesc constants;
  constants.Name = "NativeXRenderer motion vector constants";
  constants.Size = sizeof(MotionConstants);
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  RefCntAutoPtr<IShader> vs = CreateShader(device, SHADER_TYPE_VERTEX, kMotionVS, "NativeXRenderer motion vectors VS");
  RefCntAutoPtr<IShader> ps = CreateShader(device, SHADER_TYPE_PIXEL, kMotionPS, "NativeXRenderer motion vectors PS");
  auto depth_pass = [&](RefCntAutoPtr<IPipelineState>& pso, RefCntAutoPtr<IShaderResourceBinding>& srb,
                        const char* name, IShader* pixel, TEXTURE_FORMAT format) {
    if (!constants_ || !vs || !pixel) return;
    GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = name;
    info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    info.GraphicsPipeline.NumRenderTargets = 1;
    info.GraphicsPipeline.RTVFormats[0] = format;
    info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    info.pVS = vs;
    info.pPS = pixel;
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
    device->CreateGraphicsPipelineState(info, &pso);
    if (pso) {
      pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "MotionConstants")->Set(constants_);
      pso->CreateShaderResourceBinding(&srb, true);
    }
  };
  depth_pass(motion_pso_, motion_srb_, "NativeXRenderer motion vectors", ps, kMotionFormat);
  if (!motion_srb_) {
    REXLOG_ERROR("[native_x_renderer] {}: the motion vector pass could not be set up", Name());
    return false;
  }
  if (provider_ == Provider::kFsr) {
    RefCntAutoPtr<IShader> depth_ps =
        CreateShader(device, SHADER_TYPE_PIXEL, kDepthCopyPS, "NativeXRenderer FSR depth copy PS");
    depth_pass(depth_copy_pso_, depth_copy_srb_, "NativeXRenderer FSR depth copy", depth_ps, TEX_FORMAT_R32_FLOAT);
    if (!depth_copy_srb_) {
      REXLOG_ERROR("[native_x_renderer] {}: the depth copy pass could not be set up", Name());
      return false;
    }
  }
  available_ = true;
  NextFrame();
  return true;
}

bool Upscaler::InitializeFsr(uint32_t output_width, uint32_t output_height) {
  fsr_ = std::make_unique<Fsr>();
  if (!fsr_->Load(device_)) return false;
  // AMD's quality modes are in Mode's order: native AA, quality, balanced, performance, ultra performance.
  static constexpr const char* kNames[] = {"native AA", "quality", "balanced", "performance", "ultra performance"};
  std::string modes;
  for (int quality = 0; quality < 5; ++quality) {
    uint32_t width = 0, height = 0;
    Fsr::RenderSize(output_width, output_height, quality, width, height);
    if (!modes.empty()) modes += ", ";
    modes += fmt::format("{} {}x{}", kNames[quality], width, height);
    if (quality == int(mode_)) {
      input_size_[0] = std::min(width, output_width);
      input_size_[1] = std::min(height, output_height);
    }
  }
  REXLOG_INFO("[native_x_renderer] FSR 3.1 available; for a {}x{} output: {}", output_width, output_height, modes);
  // AMD's jitter phases for the ratio (8 for native AA).
  jitter_phases_ = Fsr::JitterPhases(input_size_[0], output_size_[0]);
  return true;
}

bool Upscaler::InitializeDlss(uint32_t output_width, uint32_t output_height) {
#if NATIVE_X_RENDERER_DLSS
  IRenderDevice* device = device_;
  Mode mode = mode_;
  SuperResolutionFactoryCreateInfo create_info;
  create_info.pDevice = device;
  CreateSuperResolutionFactory(create_info, &factory_);
  if (!factory_) {
    REXLOG_WARN("[native_x_renderer] DLSS: DiligentCore's super resolution factory could not be created");
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
    REXLOG_INFO("[native_x_renderer] DLSS: not available on this GPU or driver ({} other upscalers)", count);
    return false;
  }
  static_assert(sizeof(variant_) == sizeof(INTERFACE_ID));
  std::memcpy(variant_.data(), &dlss->VariantId, sizeof(variant_));
  // The input sizes NGX recommends for each quality mode, for this output (the upscaling modes, later).
  static constexpr struct {
    Mode mode;
    SUPER_RESOLUTION_OPTIMIZATION_TYPE type;
    const char* name;
  } kModes[] = {{Mode::kQuality, SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_QUALITY, "quality"},
                {Mode::kBalanced, SUPER_RESOLUTION_OPTIMIZATION_TYPE_BALANCED, "balanced"},
                {Mode::kPerformance, SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_PERFORMANCE, "performance"},
                {Mode::kUltraPerformance, SUPER_RESOLUTION_OPTIMIZATION_TYPE_MAX_PERFORMANCE, "ultra performance"}};
  std::string modes;
  for (const auto& entry : kModes) {
    SuperResolutionSourceSettingsAttribs attribs;
    attribs.VariantId = dlss->VariantId;
    attribs.OutputWidth = output_width;
    attribs.OutputHeight = output_height;
    attribs.OptimizationType = entry.type;
    SuperResolutionSourceSettings settings;
    factory_->GetSourceSettings(attribs, settings);
    if (!modes.empty()) modes += ", ";
    modes += fmt::format("{} {}x{}", entry.name, settings.OptimalInputWidth, settings.OptimalInputHeight);
    if (entry.mode == mode && settings.OptimalInputWidth && settings.OptimalInputHeight) {
      input_size_[0] = std::min(settings.OptimalInputWidth, output_width);
      input_size_[1] = std::min(settings.OptimalInputHeight, output_height);
      optimization_type_ = uint8_t(entry.type);
    }
  }
  bool native = (dlss->TemporalCapFlags & SUPER_RESOLUTION_TEMPORAL_CAP_FLAG_NATIVE) != 0;
  REXLOG_INFO("[native_x_renderer] DLSS available ({}{}); for a {}x{} output: {}", dlss->Name,
              native ? ", native resolution (DLAA)" : "", output_width, output_height, modes);
  if (mode == Mode::kDlaa && !native) return false;
  if (mode != Mode::kDlaa && !upscales()) {
    REXLOG_WARN("[native_x_renderer] DLSS: no input resolution for {}; DLAA instead", Name());
    mode_ = Mode::kDlaa;
    if (!native) return false;
  }
  // The jitter's phases: 8 per output pixel per input pixel (NVIDIA's guide), and 64 at least.
  double ratio = double(output_size_[0]) * output_size_[1] / (double(input_size_[0]) * input_size_[1]);
  jitter_phases_ = std::max(64u, uint32_t(std::ceil(8.0 * ratio)));
  return true;
#else
  (void)output_width;
  (void)output_height;
  REXLOG_INFO("[native_x_renderer] DLSS: not in this build");
  return false;
#endif
}

void Upscaler::NextFrame() {
  ++frame_;
  uint32_t phase = frame_ % jitter_phases_ + 1;
  jitter_[0] = Halton(phase, 2) - 0.5f;
  jitter_[1] = Halton(phase, 3) - 0.5f;
}

bool Upscaler::EnsureResources(ITexture* color, ITexture* depth, ITexture* output) {
  if (provider_ == Provider::kFsr) {
    const TextureDesc& color_desc = color->GetDesc();
    const TextureDesc& output_desc = output->GetDesc();
    if (motion_ && output_ && depth_copy_ && motion_->GetDesc().Width == color_desc.Width &&
        motion_->GetDesc().Height == color_desc.Height && output_->GetDesc().Width == output_desc.Width &&
        output_->GetDesc().Height == output_desc.Height && output_->GetDesc().Format == output_desc.Format) {
      return true;
    }
    // FSR's own context is made again by its next upscale at these sizes (fsr.cpp).
    motion_ = CreateTexture(device_, "NativeXRenderer motion vectors", color_desc.Width, color_desc.Height,
                            kMotionFormat, BIND_SHADER_RESOURCE | BIND_RENDER_TARGET);
    depth_copy_ = CreateTexture(device_, "NativeXRenderer FSR depth", color_desc.Width, color_desc.Height,
                                TEX_FORMAT_R32_FLOAT, BIND_SHADER_RESOURCE | BIND_RENDER_TARGET);
    output_ = CreateTexture(device_, "NativeXRenderer FSR output", output_desc.Width, output_desc.Height,
                            output_desc.Format, BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS);
    bool ok = motion_ && depth_copy_ && output_;
    if (ok) {
      REXLOG_INFO("[native_x_renderer] {}: {}x{} to {}x{}, color {}, depth {}", Name(), color_desc.Width,
                  color_desc.Height, output_desc.Width, output_desc.Height,
                  GetTextureFormatAttribs(color_desc.Format).Name, GetTextureFormatAttribs(depth->GetDesc().Format).Name);
    }
    last_applied_frame_ = UINT32_MAX;  // new resources: no history
    return ok;
  }
#if NATIVE_X_RENDERER_DLSS
  const TextureDesc& color_desc = color->GetDesc();
  const TextureDesc& output_desc = output->GetDesc();
  TEXTURE_FORMAT depth_format = depth->GetDesc().Format;
  if (dlss_) {
    const SuperResolutionDesc& desc = dlss_->GetDesc();
    if (desc.InputWidth == color_desc.Width && desc.InputHeight == color_desc.Height &&
        desc.OutputWidth == output_desc.Width && desc.OutputHeight == output_desc.Height &&
        desc.ColorFormat == color_desc.Format && desc.OutputFormat == output_desc.Format &&
        desc.DepthFormat == depth_format) {
      return true;
    }
    dlss_.Release();
  }
  SuperResolutionDesc desc;
  desc.Name = "NativeXRenderer DLSS";
  std::memcpy(&desc.VariantId, variant_.data(), sizeof(variant_));
  desc.InputWidth = color_desc.Width;
  desc.InputHeight = color_desc.Height;
  desc.OutputWidth = output_desc.Width;
  desc.OutputHeight = output_desc.Height;
  desc.ColorFormat = color_desc.Format;
  desc.OutputFormat = output_desc.Format;
  desc.DepthFormat = depth_format;
  desc.MotionFormat = kMotionFormat;
  // The model NGX picks: by the quality mode when upscaling (DiligentCore patch 0005), DLAA's otherwise.
  desc.OptimizationType = SUPER_RESOLUTION_OPTIMIZATION_TYPE(optimization_type_);
  // The scene before the game's post-processing is HDR, with no exposure of its own to give.
  desc.Flags = SUPER_RESOLUTION_FLAG_AUTO_EXPOSURE;
  factory_->CreateSuperResolution(desc, &dlss_);

  auto create = [&](RefCntAutoPtr<ITexture>& texture, const char* name, const TextureDesc& size, TEXTURE_FORMAT format,
                    BIND_FLAGS bind) {
    texture.Release();
    TextureDesc texture_desc;
    texture_desc.Name = name;
    texture_desc.Type = RESOURCE_DIM_TEX_2D;
    texture_desc.Width = size.Width;
    texture_desc.Height = size.Height;
    texture_desc.Format = format;
    texture_desc.BindFlags = bind;
    device_->CreateTexture(texture_desc, nullptr, &texture);
    return texture != nullptr;
  };
  // The motion vectors at the input resolution, in its pixels (DiligentCore creates DLSS with MVLowRes).
  bool ok = dlss_ &&
            create(motion_, "NativeXRenderer motion vectors", color_desc, kMotionFormat,
                   BIND_SHADER_RESOURCE | BIND_RENDER_TARGET) &&
            create(output_, "NativeXRenderer DLSS output", output_desc, output_desc.Format,
                   BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS);
  if (ok) {
    REXLOG_INFO("[native_x_renderer] {}: {}x{} to {}x{}, color {}, depth {}", Name(), color_desc.Width,
                color_desc.Height, output_desc.Width, output_desc.Height,
                GetTextureFormatAttribs(color_desc.Format).Name, GetTextureFormatAttribs(depth_format).Name);
  }
  last_applied_frame_ = UINT32_MAX;  // new resources: no history
  return ok;
#else
  (void)color;
  (void)depth;
  (void)output;
  return false;
#endif
}

bool Upscaler::ApplyFsr(IDeviceContext* context, ITexture* color, ITextureView* depth_srv,
                        const std::array<float, 16>& view_projection, bool reset) {
  // The depth copy reads the motion vector pass's constants (this frame's depth range).
  ITextureView* depth_rtv = depth_copy_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  context->SetRenderTargets(1, &depth_rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Viewport viewport;
  viewport.Width = float(depth_copy_->GetDesc().Width);
  viewport.Height = float(depth_copy_->GetDesc().Height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(depth_copy_pso_);
  depth_copy_srb_->GetVariableByName(SHADER_TYPE_PIXEL, "depthTex")->Set(depth_srv);
  context->CommitShaderResources(depth_copy_srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);

  Fsr::Inputs in;
  in.color = color;
  in.depth = depth_copy_;
  in.motion = motion_;
  in.output = output_;
  in.jitter_x = jitter_[0];
  in.jitter_y = jitter_[1];
  in.reset = reset;
  in.inverted_depth = false;  // the copy is 0 at the near plane, 1 at the far one
  CameraFromViewProjection(view_projection, in.camera_near, in.camera_far, in.fov_y);
  return fsr_->Dispatch(context, in);
}

bool Upscaler::Apply(IDeviceContext* context, ITexture* color, ITexture* depth, ITextureView* depth_srv,
                     ITexture* output, const std::array<float, 16>& view_projection, float z_min, float z_max,
                     const std::function<void(ITextureView*)>& object_motion, const Mat4d* camera_reprojection) {
  if (!available_ || failed_ || !color || !depth || !depth_srv || !output) return false;
  if (!EnsureResources(color, depth, output)) {
    REXLOG_ERROR("[native_x_renderer] {}: could not be set up; the scene is not antialiased", Name());
    failed_ = true;
    return false;
  }
  const TextureDesc& desc = color->GetDesc();
  // History is kept only from the frame before: after a frame without the view (menus, loads), DLSS starts
  // again, and the motion vectors are zero.
  bool reset = last_applied_frame_ == UINT32_MAX || last_applied_frame_ + 1 != frame_;

  // The reprojection: this frame's matrix inverted, then the last frame's, as one matrix (rows, in double). Done on the
  // GPU in single precision, a pixel's way to the world and back left a static camera's far pixels a fraction of a
  // pixel off at random, frame to frame, which DLSS and FSR took for motion and blurred (D117). The same matrix both
  // frames then makes the identity here, and no motion.
  // The camera vote's (D118) is the transpose of that (column form: last clip = r x this clip), and replaces it.
  MotionConstants constants{};
  double inverse[16];
  Mat4d from_view{}, from_vote{};
  bool has_view = !reset && Invert(view_projection, inverse);
  if (has_view) {
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        double sum = 0.0;
        for (int k = 0; k < 4; ++k) sum += inverse[r * 4 + k] * double(last_view_projection_[k * 4 + c]);
        from_view[r * 4 + c] = sum;
      }
    }
  }
  bool has_vote = !reset && camera_reprojection;
  if (has_vote) {
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) from_vote[r * 4 + c] = (*camera_reprojection)[c * 4 + r];
    }
  }
  // Measured on their column forms (the transposes), as the vote's are.
  auto transpose = [](const Mat4d& m) {
    Mat4d out;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) out[r * 4 + c] = m[c * 4 + r];
    }
    return out;
  };
  last_vote_difference_px_ = has_view && has_vote ? CameraVote::DistancePx(transpose(from_view), transpose(from_vote),
                                                                           output_size_[0], output_size_[1])
                                                  : -1.0;
  if (!has_view && !has_vote) reset = true;
  for (int i = 0; i < 16; ++i) {
    constants.reprojection[i] = reset ? (i % 5 == 0 ? 1.0f : 0.0f) : float(has_vote ? from_vote[i] : from_view[i]);
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
  // The objects that moved on their own, over the camera's motion (D48). Not after a reset: DLSS takes no
  // motion then.
  if (!reset && object_motion) object_motion(motion_rtv);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);

  bool ok = false;
  if (provider_ == Provider::kFsr) {
    ok = ApplyFsr(context, color, depth_srv, view_projection, reset);
  } else {
#if NATIVE_X_RENDERER_DLSS
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
  ok = true;
#endif
  }
  // NGX and FSR record their own compute pipelines and bindings into the command buffer, behind DiligentCore's
  // back: its cache of what is bound is no longer true (the texture loads also use compute pipelines). The cache is
  // cleared, the draws that follow bind everything again. The commands stay in the command buffer, submitted with
  // the frame's (DiligentCore patch 0010, D122: a submission here cost 1.5 % of the renderer thread).
  context->InvalidateState();
  if (!ok) {
    REXLOG_ERROR("[native_x_renderer] {}: the upscale failed; the scene is not antialiased", Name());
    failed_ = true;
    return false;
  }
  CopyTextureAttribs copy{output_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, output,
                          RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
  context->CopyTexture(copy);

  last_view_projection_ = view_projection;
  last_applied_frame_ = frame_;
  ++applied_;
  return true;
}

}  // namespace native_x_renderer
