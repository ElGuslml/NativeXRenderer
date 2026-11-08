// NativeXRenderer - see pipelines.h.

#include "pipelines.h"

#include "targets.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <string>
#include <string_view>

#include <xxhash.h>

REXCVAR_DEFINE_BOOL(native_x_renderer_binding_cache, true, "NativeXRenderer",
                    "Each set of textures and samplers a pipeline draws with keeps a shader resource binding of its "
                    "own, written once, instead of a descriptor set written at every commit (read at startup, D111)");

REXCVAR_DEFINE_BOOL(native_x_renderer_resource_signature, true, "NativeXRenderer",
                    "The game pipelines share one resource signature, their textures and samplers named by their place "
                    "(read at startup, D131): one pipeline layout for all of them. False: each pipeline's own layout "
                    "from its shaders, as before");

namespace native_x_renderer {

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

const char* const kBufferNames[] = {"XeSharedMemory", "XeSystemConstants", "XeFloatConstants", "XeBoolLoopConstants",
                                    "XeFetchConstants"};

std::string SignatureName(uint32_t stage, bool texture, uint32_t index) {
  return std::string(kSignaturePrefix[stage]) + (texture ? "t" : "s") + std::to_string(index);
}

// Whether a host shader's textures and samplers were named by their place (D131): none keeps the translator's name.
bool HasPositionalNames(const IShader* shader) {
  if (!shader) return true;
  for (Uint32 i = 0, count = shader->GetResourceCount(); i < count; ++i) {
    ShaderResourceDesc desc;
    shader->GetResourceDesc(i, desc);
    std::string_view name = desc.Name ? desc.Name : "";
    if (name.rfind("xe_texture", 0) == 0 || name.rfind("xe_sampler", 0) == 0) return false;
  }
  return true;
}

}  // namespace

bool ResourceSignatureOn() { return REXCVAR_GET(native_x_renderer_resource_signature); }

size_t PipelineStateHash::operator()(const PipelineState& state) const {
  return size_t(XXH3_64bits(&state, sizeof(state)));
}

void PipelineCache::Initialize(IRenderDevice* device, const PipelineBuffers& buffers, bool link_from_libraries) {
  device_ = device;
  buffers_ = buffers;
  binding_cache_ = REXCVAR_GET(native_x_renderer_binding_cache);
  link_from_libraries_ = link_from_libraries;
  if (!ResourceSignatureOn()) return;
  // D131: the buffers per stage (static, but each pipeline's float constants, set in each shader resource binding), then
  // the textures and samplers by their place: mutable with binding sets (D111), else dynamic.
  const SHADER_RESOURCE_VARIABLE_TYPE texture_type =
      binding_cache_ ? SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE : SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  std::vector<std::string> names;
  for (uint32_t stage = 0; stage < 2; ++stage) {
    for (uint32_t i = 0; i < kSignatureTextures[stage]; ++i) names.push_back(SignatureName(stage, true, i));
    for (uint32_t i = 0; i < kSignatureSamplers[stage]; ++i) names.push_back(SignatureName(stage, false, i));
  }
  std::vector<PipelineResourceDesc> resources;
  size_t name = 0;
  for (uint32_t stage = 0; stage < 2; ++stage) {
    for (const char* buffer : kBufferNames) {
      const std::string_view b = buffer;
      resources.emplace_back(kStages[stage], buffer, 1,
                             b == "XeSharedMemory" ? SHADER_RESOURCE_TYPE_BUFFER_SRV : SHADER_RESOURCE_TYPE_CONSTANT_BUFFER,
                             b == "XeFloatConstants" ? SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE
                                                     : SHADER_RESOURCE_VARIABLE_TYPE_STATIC);
    }
    for (uint32_t i = 0; i < kSignatureTextures[stage]; ++i) {
      resources.emplace_back(kStages[stage], names[name++].c_str(), 1, SHADER_RESOURCE_TYPE_TEXTURE_SRV, texture_type);
    }
    for (uint32_t i = 0; i < kSignatureSamplers[stage]; ++i) {
      resources.emplace_back(kStages[stage], names[name++].c_str(), 1, SHADER_RESOURCE_TYPE_SAMPLER, texture_type);
    }
  }
  PipelineResourceSignatureDesc desc;
  desc.Name = "NativeXRenderer game pipelines";
  desc.Resources = resources.data();
  desc.NumResources = uint32_t(resources.size());
  desc.BindingIndex = 0;
  desc.UseCombinedTextureSamplers = false;
  device->CreatePipelineResourceSignature(desc, &signature_);
  if (!signature_) {
    REXLOG_ERROR("[native_x_renderer] could not create the game pipelines' resource signature (D131); each pipeline "
                 "gets its own layout");
    return;
  }
  for (uint32_t stage = 0; stage < 2; ++stage) {
    auto set = [&](const char* buffer, IDeviceObject* object) {
      if (!object) return;  // the pipeline preparation tool draws nothing (D129)
      if (IShaderResourceVariable* variable = signature_->GetStaticVariableByName(kStages[stage], buffer)) {
        variable->Set(object);
      }
    };
    set("XeSharedMemory", buffers.shared_memory);
    set("XeSystemConstants", buffers.system_constants);
    set("XeBoolLoopConstants", buffers.bool_loop_constants);
    set("XeFetchConstants", buffers.fetch_constants);
  }
  REXLOG_INFO("[native_x_renderer] game pipelines: one resource signature, {} textures and {} samplers per pixel shader, "
              "{} and {} per vertex shader (D131)",
              kSignatureTextures[1], kSignatureSamplers[1], kSignatureTextures[0], kSignatureSamplers[0]);
}

void PipelineCache::SetFloatConstants(Pipeline& p, IShaderResourceBinding* srb) {
  for (uint32_t stage = 0; stage < 2; ++stage) {
    if (!p.shaders[stage]) continue;
    if (IShaderResourceVariable* variable = srb->GetVariableByName(kStages[stage], "XeFloatConstants")) {
      variable->Set(p.float_constants[stage]);
    }
  }
}

Pipeline* PipelineCache::Get(const PipelineState& s, const rg::SpirvShader& vertex_shader,
                             const rg::SpirvShader* pixel_shader, bool* created, bool motion) {
  if (created) *created = false;
  auto it = pipelines_.find(s);
  if (it != pipelines_.end()) return it->second.get();
  std::unique_ptr<Pipeline>& slot = pipelines_[s];
  internal::ScopedCreationTime time(time_);

  std::string name = std::string(motion ? "NativeXRenderer motion pipeline " : "NativeXRenderer draw pipeline ") +
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

  // D131: the game pipelines' resource signature, when both shaders' textures and samplers are named by their place.
  const bool signature =
      signature_ && !motion && HasPositionalNames(s.vertex_shader) && HasPositionalNames(s.pixel_shader);
  IPipelineResourceSignature* signatures[1] = {signature_};
  if (signature) {
    create_info.ppResourceSignatures = signatures;
    create_info.ResourceSignaturesCount = 1;
    // D132: linked from the device's parts at once, the whole pipeline compiled behind it.
    if (link_from_libraries_) create_info.Flags |= PSO_CREATE_FLAG_VK_LINK_FROM_LIBRARIES;
  }
  // The buffers: static, per stage (the two XeFloatConstants differ; the others are kept apart too, so
  // DiligentCore never has to merge a resource across stages).
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
  // Textures and samplers: dynamic, set per draw; mutable, set once in each binding set (D111).
  const bool binding_sets = binding_cache_ && !motion;
  create_info.PSODesc.ResourceLayout.DefaultVariableType =
      binding_sets ? SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE : SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  if (!signature) {
    create_info.PSODesc.ResourceLayout.Variables = variables.data();
    create_info.PSODesc.ResourceLayout.NumVariables = uint32_t(variables.size());
  }

  auto pipeline = std::make_unique<Pipeline>();
  device_->CreateGraphicsPipelineState(create_info, &pipeline->pso);
  if (!pipeline->pso) {
    REXLOG_ERROR("[native_x_renderer] could not create {} (VS {:016X}, PS {:016X})", name,
                 vertex_shader.ucode_data_hash(), pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    return nullptr;
  }
  pipeline->shaders = {&vertex_shader, pixel_shader};
  const auto& shaders = pipeline->shaders;
  for (uint32_t stage = 0; stage < 2; ++stage) {
    uint32_t float_count = shaders[stage] ? shaders[stage]->constant_register_map().float_count : 0;
    BufferDesc desc;
    desc.Name = stage ? "NativeXRenderer PS float constants" : "NativeXRenderer VS float constants";
    desc.Size = 16 * std::max(float_count, 1u);
    desc.Usage = USAGE_DYNAMIC;
    desc.BindFlags = BIND_UNIFORM_BUFFER;
    desc.CPUAccessFlags = CPU_ACCESS_WRITE;
    device_->CreateBuffer(desc, nullptr, &pipeline->float_constants[stage]);
    if (motion && !stage) {
      desc.Name = "NativeXRenderer VS float constants, last frame";
      device_->CreateBuffer(desc, nullptr, &pipeline->previous_float_constants);
    }
  }
  pipeline->motion = motion;
  pipeline->signature = signature;
  if (!motion) {
    if (signature) {
      ++signature_count_;
    } else if (signature_ && !own_layout_count_++) {
      REXLOG_WARN("[native_x_renderer] {} has a layout of its own: VS {:016X} or PS {:016X} has more textures or "
                  "samplers than the game pipelines' resource signature (D131)",
                  name, vertex_shader.ucode_data_hash(), pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    }
  }
  pipeline->binding_sets_on = binding_sets;
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
    REXLOG_ERROR("[native_x_renderer] {} failed to compile (VS {:016X}, PS {:016X})", p.pso->GetDesc().Name,
                 p.shaders[0]->ucode_data_hash(), p.shaders[1] ? p.shaders[1]->ucode_data_hash() : 0);
    return false;
  }
  Pipeline* pipeline = &p;
  const auto& shaders = p.shaders;
  for (uint32_t stage = 0; stage < 2 && !p.signature; ++stage) {
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
  // With the resource signature its shader resource bindings are the signature's (D131).
  if (p.signature) {
    signature_->CreateShaderResourceBinding(&pipeline->srb, true);
    if (pipeline->srb) SetFloatConstants(p, pipeline->srb);
  } else {
    pipeline->pso->CreateShaderResourceBinding(&pipeline->srb, true);
  }
  if (!pipeline->srb) {
    p.failed = true;
    REXLOG_ERROR("[native_x_renderer] {}: no shader resource binding", p.pso->GetDesc().Name);
    return false;
  }

  for (uint32_t stage = 0; stage < 2; ++stage) {
    if (!shaders[stage]) continue;
    const auto& textures = shaders[stage]->GetTextureBindingsAfterTranslation();
    for (uint32_t i = 0; i < uint32_t(textures.size()); ++i) {
      std::string name = p.signature ? SignatureName(stage, true, i) : TextureName(textures[i]);
      pipeline->textures[stage].push_back(pipeline->srb->GetVariableByName(kStages[stage], name.c_str()));
    }
    const auto& samplers = shaders[stage]->GetSamplerBindingsAfterTranslation();
    for (uint32_t i = 0; i < uint32_t(samplers.size()); ++i) {
      std::string name = p.signature ? SignatureName(stage, false, i) : SamplerName(samplers[i]);
      pipeline->samplers[stage].push_back(pipeline->srb->GetVariableByName(kStages[stage], name.c_str()));
    }
    pipeline->texture_objects[stage].assign(pipeline->textures[stage].size(), nullptr);
    pipeline->sampler_objects[stage].assign(pipeline->samplers[stage].size(), nullptr);
    // D111: the variables' indices, the same in every shader resource binding of the pipeline.
    auto index = [](IShaderResourceVariable* variable) { return variable ? variable->GetIndex() : UINT32_MAX; };
    for (IShaderResourceVariable* variable : pipeline->textures[stage]) {
      pipeline->texture_indices[stage].push_back(index(variable));
    }
    for (IShaderResourceVariable* variable : pipeline->samplers[stage]) {
      pipeline->sampler_indices[stage].push_back(index(variable));
    }
  }
  p.ready = true;
  return true;
}

BindingSet* PipelineCache::FindSet(Pipeline& p, const std::vector<IDeviceObject*>& objects, uint64_t frame,
                                   bool* created) {
  *created = false;
  BindingSet* set = p.current_set;
  if (!set || set->objects != objects) {
    set = nullptr;
    const uint64_t key = XXH3_64bits(objects.data(), objects.size() * sizeof(objects[0]));
    auto [first, last] = p.binding_sets.equal_range(key);
    for (auto it = first; it != last; ++it) {
      if (it->second->objects == objects) {
        set = it->second.get();
        break;
      }
    }
    if (!set) {
      auto made = std::make_unique<BindingSet>();
      if (p.signature) {
        signature_->CreateShaderResourceBinding(&made->srb, true);
      } else {
        p.pso->CreateShaderResourceBinding(&made->srb, true);
      }
      if (!made->srb) return nullptr;
      if (p.signature) SetFloatConstants(p, made->srb);
      // In the order the draw path lists the objects: per stage, the textures then the samplers.
      size_t k = 0;
      auto set_variable = [&](SHADER_TYPE stage, uint32_t index, IDeviceObject* object) {
        if (index == UINT32_MAX || !object) return;
        if (IShaderResourceVariable* variable = made->srb->GetVariableByIndex(stage, index)) {
          variable->Set(object);
          ++set_variables_;
        }
      };
      for (uint32_t stage = 0; stage < 2; ++stage) {
        for (uint32_t index : p.texture_indices[stage]) {
          IDeviceObject* object = k < objects.size() ? objects[k] : nullptr;
          set_variable(kStages[stage], index, object);
          if (object && index != UINT32_MAX) made->views.push_back(static_cast<ITextureView*>(object));
          ++k;
        }
        for (uint32_t index : p.sampler_indices[stage]) {
          set_variable(kStages[stage], index, k < objects.size() ? objects[k] : nullptr);
          ++k;
        }
      }
      made->objects = objects;
      set = made.get();
      p.binding_sets.emplace(key, std::move(made));
      ++binding_set_count_;
      *created = true;
    }
    p.current_set = set;
  }
  set->last_used = frame;
  return set;
}

uint32_t PipelineCache::ReleaseSets(Pipeline& pipeline, uint64_t frame, uint64_t unused_frames,
                                    const std::function<void(ITexture*)>& let_go) {
  uint32_t released = 0;
  for (auto it = pipeline.binding_sets.begin(); it != pipeline.binding_sets.end();) {
    BindingSet& set = *it->second;
    if (set.last_used + unused_frames > frame) {
      ++it;
      continue;
    }
    if (let_go) {
      for (ITextureView* view : set.views) let_go(view->GetTexture());
    }
    if (pipeline.current_set == &set) pipeline.current_set = nullptr;
    it = pipeline.binding_sets.erase(it);
    --binding_set_count_;
    ++released;
  }
  return released;
}

uint32_t PipelineCache::ReleaseSets(uint64_t frame, uint64_t unused_frames) {
  uint32_t released = 0;
  for (auto& [state, pipeline] : pipelines_) released += ReleaseSets(*pipeline, frame, unused_frames, nullptr);
  return released;
}

PipelineCache::Released PipelineCache::ReleaseTextures(uint64_t frame, uint64_t unused_frames) {
  Released released;
  // The textures let go, each once, watched to see which are destroyed: those only the bindings held.
  std::unordered_map<ITexture*, std::pair<RefCntWeakPtr<ITexture>, uint64_t>> let_go;
  auto let_go_texture = [&let_go](ITexture* texture) {
    if (!let_go.count(texture)) {
      let_go.emplace(texture, std::make_pair(RefCntWeakPtr<ITexture>(texture), TextureBytes(texture->GetDesc())));
    }
  };
  for (auto& [state, pipeline] : pipelines_) {
    // D111: the binding sets not used for as long.
    released.sets += ReleaseSets(*pipeline, frame, unused_frames, let_go_texture);
    if (!pipeline->textures_set || pipeline->last_drawn + unused_frames > frame) continue;
    for (auto& objects : pipeline->texture_objects) std::fill(objects.begin(), objects.end(), nullptr);
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

}  // namespace native_x_renderer
