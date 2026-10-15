// XenosNative - see pipelines.h.

#include "pipelines.h"

#include "targets.h"

#include <rex/logging.h>

#include <algorithm>
#include <string>

#include <xxhash.h>

namespace xenos_native {

using namespace Diligent;
namespace rg = rex::graphics;

namespace {

// The names the SDK's translator gives its texture and sampler variables (spirv_translator_fetch.cpp,
// FindOrAddTextureBinding and FindOrAddSamplerBinding).
std::string TextureName(const rg::SpirvShader::TextureBinding& binding) {
  const char* dimension = "2d";
  if (binding.dimension == rg::xenos::FetchOpDimension::k3DOrStacked) dimension = "3d";
  if (binding.dimension == rg::xenos::FetchOpDimension::kCube) dimension = "cube";
  return "xe_texture" + std::to_string(binding.fetch_constant) + "_" + dimension + (binding.is_signed ? "_s" : "_u");
}

std::string SamplerName(const rg::SpirvShader::SamplerBinding& binding) {
  static const char kFilterSuffixes[] = {'p', 'l', 'b', 'f'};
  std::string name = "xe_sampler" + std::to_string(binding.fetch_constant) + "_";
  name += kFilterSuffixes[uint32_t(binding.mag_filter)];
  name += kFilterSuffixes[uint32_t(binding.min_filter)];
  name += kFilterSuffixes[uint32_t(binding.mip_filter)];
  if (binding.aniso_filter != rg::xenos::AnisoFilter::kUseFetchConst) {
    if (binding.aniso_filter == rg::xenos::AnisoFilter::kDisabled) {
      name += "_a0";
    } else {
      name += "_a" + std::to_string(1u << (uint32_t(binding.aniso_filter) - 1));
    }
  }
  return name;
}

constexpr SHADER_TYPE kStages[2] = {SHADER_TYPE_VERTEX, SHADER_TYPE_PIXEL};

}  // namespace

size_t PipelineStateHash::operator()(const PipelineState& state) const {
  return size_t(XXH3_64bits(&state, sizeof(state)));
}

void PipelineCache::Initialize(IRenderDevice* device, const PipelineBuffers& buffers) {
  device_ = device;
  buffers_ = buffers;
}

Pipeline* PipelineCache::Get(const PipelineState& s, const rg::SpirvShader& vertex_shader,
                             const rg::SpirvShader* pixel_shader, bool* created, bool motion) {
  if (created) *created = false;
  auto it = pipelines_.find(s);
  if (it != pipelines_.end()) return it->second.get();
  std::unique_ptr<Pipeline>& slot = pipelines_[s];
  internal::ScopedCreationTime time(time_);

  std::string name = std::string(motion ? "XenosNative motion pipeline " : "XenosNative draw pipeline ") +
                     std::to_string(pipelines_.size());
  GraphicsPipelineStateCreateInfo create_info;
  create_info.PSODesc.Name = name.c_str();
  create_info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  create_info.Flags = PSO_CREATE_FLAG_IGNORE_MISSING_VARIABLES | PSO_CREATE_FLAG_ASYNCHRONOUS;
  create_info.pPSOCache = cache_;
  create_info.pVS = const_cast<IShader*>(s.vertex_shader);
  create_info.pPS = const_cast<IShader*>(s.pixel_shader);

  GraphicsPipelineDesc& g = create_info.GraphicsPipeline;
  g.NumRenderTargets = s.render_target_count;
  for (uint32_t i = 0; i < 4; ++i) g.RTVFormats[i] = TEXTURE_FORMAT(s.rtv_formats[i]);
  g.DSVFormat = TEXTURE_FORMAT(s.dsv_format);
  g.SmplDesc.Count = s.samples;
  g.PrimitiveTopology = PRIMITIVE_TOPOLOGY(s.topology);

  RasterizerStateDesc& r = g.RasterizerDesc;
  r.FillMode = s.wireframe ? FILL_MODE_WIREFRAME : FILL_MODE_SOLID;
  r.CullMode = CULL_MODE(s.cull_mode);
  r.FrontCounterClockwise = s.front_counter_clockwise != 0;
  r.DepthClipEnable = s.depth_clip != 0;
  r.ScissorEnable = True;
  r.DepthBias = s.depth_bias;
  r.SlopeScaledDepthBias = s.slope_scaled_depth_bias;

  DepthStencilStateDesc& d = g.DepthStencilDesc;
  d.DepthEnable = s.depth_enable != 0;
  d.DepthWriteEnable = s.depth_write != 0;
  d.DepthFunc = COMPARISON_FUNCTION(s.depth_func);
  d.StencilEnable = s.stencil_enable != 0;
  d.StencilReadMask = s.stencil_read_mask;
  d.StencilWriteMask = s.stencil_write_mask;
  auto face = [](const uint8_t (&ops)[4]) {
    StencilOpDesc desc;
    desc.StencilFailOp = STENCIL_OP(ops[0]);
    desc.StencilDepthFailOp = STENCIL_OP(ops[1]);
    desc.StencilPassOp = STENCIL_OP(ops[2]);
    desc.StencilFunc = COMPARISON_FUNCTION(ops[3]);
    return desc;
  };
  if (s.stencil_enable) {
    d.FrontFace = face(s.stencil_front);
    d.BackFace = face(s.stencil_back);
  }

  BlendStateDesc& b = g.BlendDesc;
  b.IndependentBlendEnable = True;
  for (uint32_t i = 0; i < 4; ++i) {
    const PipelineState::Target& t = s.targets[i];
    RenderTargetBlendDesc& target = b.RenderTargets[i];
    target.BlendEnable = t.blend_enable != 0;
    if (t.blend_enable) {
      target.SrcBlend = BLEND_FACTOR(t.src);
      target.DestBlend = BLEND_FACTOR(t.dst);
      target.BlendOp = BLEND_OPERATION(t.op);
      target.SrcBlendAlpha = BLEND_FACTOR(t.src_alpha);
      target.DestBlendAlpha = BLEND_FACTOR(t.dst_alpha);
      target.BlendOpAlpha = BLEND_OPERATION(t.op_alpha);
    }
    target.RenderTargetWriteMask = COLOR_MASK(t.write_mask);
  }

  // The buffers: static, per stage (the two XeFloatConstants differ; the others are kept apart too, so
  // DiligentCore never has to merge a resource across stages).
  static const char* kBufferNames[] = {"XeSharedMemory", "XeSystemConstants", "XeFloatConstants",
                                       "XeBoolLoopConstants", "XeFetchConstants"};
  std::vector<ShaderResourceVariableDesc> variables;
  for (uint32_t stage = 0; stage < (pixel_shader ? 2u : 1u); ++stage) {
    for (const char* buffer_name : kBufferNames) {
      variables.emplace_back(kStages[stage], buffer_name, SHADER_RESOURCE_VARIABLE_TYPE_STATIC);
    }
  }
  if (motion) {
    variables.emplace_back(SHADER_TYPE_VERTEX, "XeFloatConstantsPrevious", SHADER_RESOURCE_VARIABLE_TYPE_STATIC);
    variables.emplace_back(SHADER_TYPE_PIXEL, "XeMotionConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC);
  }
  create_info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  create_info.PSODesc.ResourceLayout.Variables = variables.data();
  create_info.PSODesc.ResourceLayout.NumVariables = uint32_t(variables.size());

  auto pipeline = std::make_unique<Pipeline>();
  device_->CreateGraphicsPipelineState(create_info, &pipeline->pso);
  if (!pipeline->pso) {
    REXLOG_ERROR("[xenos_native] could not create {} (VS {:016X}, PS {:016X})", name,
                 vertex_shader.ucode_data_hash(), pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    return nullptr;
  }
  pipeline->shaders = {&vertex_shader, pixel_shader};
  const auto& shaders = pipeline->shaders;
  for (uint32_t stage = 0; stage < 2; ++stage) {
    uint32_t float_count = shaders[stage] ? shaders[stage]->constant_register_map().float_count : 0;
    BufferDesc desc;
    desc.Name = stage ? "XenosNative PS float constants" : "XenosNative VS float constants";
    desc.Size = 16 * std::max(float_count, 1u);
    desc.Usage = USAGE_DYNAMIC;
    desc.BindFlags = BIND_UNIFORM_BUFFER;
    desc.CPUAccessFlags = CPU_ACCESS_WRITE;
    device_->CreateBuffer(desc, nullptr, &pipeline->float_constants[stage]);
    if (motion && !stage) {
      desc.Name = "XenosNative VS float constants, last frame";
      device_->CreateBuffer(desc, nullptr, &pipeline->previous_float_constants);
    }
  }
  pipeline->motion = motion;
  slot = std::move(pipeline);
  if (created) *created = true;
  return slot.get();
}

bool PipelineCache::Ready(Pipeline& p) {
  if (p.ready) return true;
  if (p.failed) return false;
  PIPELINE_STATE_STATUS status = p.pso->GetStatus();
  if (status == PIPELINE_STATE_STATUS_COMPILING) return false;
  if (status != PIPELINE_STATE_STATUS_READY) {
    p.failed = true;
    REXLOG_ERROR("[xenos_native] {} failed to compile (VS {:016X}, PS {:016X})", p.pso->GetDesc().Name,
                 p.shaders[0]->ucode_data_hash(), p.shaders[1] ? p.shaders[1]->ucode_data_hash() : 0);
    return false;
  }
  Pipeline* pipeline = &p;
  const auto& shaders = p.shaders;
  for (uint32_t stage = 0; stage < 2; ++stage) {
    if (!shaders[stage]) continue;  // depth-only pipelines have no pixel stage
    auto set = [&](const char* buffer_name, IDeviceObject* object) {
      if (IShaderResourceVariable* variable = pipeline->pso->GetStaticVariableByName(kStages[stage], buffer_name)) {
        variable->Set(object);
      }
    };
    set("XeSharedMemory", buffers_.shared_memory);
    set("XeSystemConstants", buffers_.system_constants);
    set("XeFloatConstants", pipeline->float_constants[stage]);
    set("XeBoolLoopConstants", buffers_.bool_loop_constants);
    set("XeFetchConstants", buffers_.fetch_constants);
  }
  if (p.motion) {
    if (IShaderResourceVariable* variable =
            pipeline->pso->GetStaticVariableByName(SHADER_TYPE_VERTEX, "XeFloatConstantsPrevious")) {
      variable->Set(pipeline->previous_float_constants);
    }
    if (IShaderResourceVariable* variable =
            pipeline->pso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "XeMotionConstants")) {
      variable->Set(buffers_.motion_constants);
    }
  }
  pipeline->pso->CreateShaderResourceBinding(&pipeline->srb, true);

  for (uint32_t stage = 0; stage < 2; ++stage) {
    if (!shaders[stage]) continue;
    for (const auto& binding : shaders[stage]->GetTextureBindingsAfterTranslation()) {
      pipeline->textures[stage].push_back(
          pipeline->srb->GetVariableByName(kStages[stage], TextureName(binding).c_str()));
    }
    for (const auto& binding : shaders[stage]->GetSamplerBindingsAfterTranslation()) {
      pipeline->samplers[stage].push_back(
          pipeline->srb->GetVariableByName(kStages[stage], SamplerName(binding).c_str()));
    }
  }
  p.ready = true;
  return true;
}

PipelineCache::Released PipelineCache::ReleaseTextures(uint64_t frame, uint64_t unused_frames) {
  Released released;
  // The textures let go, each once, watched to see which are destroyed: those only the bindings held.
  std::unordered_map<ITexture*, std::pair<RefCntWeakPtr<ITexture>, uint64_t>> let_go;
  for (auto& [state, pipeline] : pipelines_) {
    if (!pipeline->textures_set || pipeline->last_drawn + unused_frames > frame) continue;
    for (const auto& variables : pipeline->textures) {
      for (IShaderResourceVariable* variable : variables) {
        if (!variable) continue;
        if (auto* view = static_cast<ITextureView*>(variable->Get())) {
          ITexture* texture = view->GetTexture();
          if (!let_go.count(texture)) {
            let_go.emplace(texture, std::make_pair(RefCntWeakPtr<ITexture>(texture), TextureBytes(texture->GetDesc())));
          }
        }
        variable->Set(nullptr);
      }
    }
    pipeline->textures_set = false;
    ++released.pipelines;
  }
  for (const auto& [texture, watched] : let_go) {
    if (!watched.first.IsValid()) {
      ++released.textures;
      released.bytes += watched.second;
    }
  }
  return released;
}

}  // namespace xenos_native
