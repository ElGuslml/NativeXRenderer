// XenosNative - see draw.h.

#include "draw.h"

#include "device.h"
#include "guest_memory.h"
#include "guest_textures.h"
#include "pipelines.h"
#include "primitives.h"
#include "shaders.h"
#include "state.h"
#include "storage.h"
#include "targets.h"
#include "textures.h"

#include <rex/bit.h>
#include <rex/types.h>
#include <rex/cvar.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

REXCVAR_DEFINE_INT32(xenos_native_draw_log, 0, "XenosNative",
                     "Bring-up: log the first N draws XenosNative processes (primitive, indices, shaders)");

namespace xenos_native {

using namespace Diligent;
namespace rg = rex::graphics;
namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
namespace draw_util = rex::graphics::draw_util;
using Translator = rg::SpirvShaderTranslator;

// What one draw computed, for the system constants.
struct DrawContext {
  const rg::RegisterFile* regs;
  const rg::PrimitiveProcessor::ProcessingResult* primitives;
  const draw_util::ViewportInfo* viewport;
  bool polygonal;
  uint32_t used_texture_mask;
};

namespace {

// Registers one draw changes for itself, restored when it returns.
class ScopedRegisters {
 public:
  explicit ScopedRegisters(rg::RegisterFile& registers) : registers_(registers) {}
  ~ScopedRegisters() {
    for (uint32_t i = count_; i-- > 0;) registers_[saved_[i].first] = saved_[i].second;
  }
  void Set(uint32_t index, uint32_t value) {
    saved_[count_++] = {index, registers_[index]};
    registers_[index] = value;
  }

 private:
  rg::RegisterFile& registers_;
  std::array<std::pair<uint32_t, uint32_t>, 4> saved_{};
  uint32_t count_ = 0;
};

// Xenos blend factors to DiligentCore, as the SDK's D3D12 backend maps them: there are no constant alpha
// factors (the constant color is used), and the alpha channel takes the alpha version of color factors.
BLEND_FACTOR BlendFactor(xenos::BlendFactor factor, bool alpha) {
  switch (uint32_t(factor)) {
    case 1: return BLEND_FACTOR_ONE;
    case 4: return alpha ? BLEND_FACTOR_SRC_ALPHA : BLEND_FACTOR_SRC_COLOR;
    case 5: return alpha ? BLEND_FACTOR_INV_SRC_ALPHA : BLEND_FACTOR_INV_SRC_COLOR;
    case 6: return BLEND_FACTOR_SRC_ALPHA;
    case 7: return BLEND_FACTOR_INV_SRC_ALPHA;
    case 8: return alpha ? BLEND_FACTOR_DEST_ALPHA : BLEND_FACTOR_DEST_COLOR;
    case 9: return alpha ? BLEND_FACTOR_INV_DEST_ALPHA : BLEND_FACTOR_INV_DEST_COLOR;
    case 10: return BLEND_FACTOR_DEST_ALPHA;
    case 11: return BLEND_FACTOR_INV_DEST_ALPHA;
    case 12: case 14: return BLEND_FACTOR_BLEND_FACTOR;
    case 13: case 15: return BLEND_FACTOR_INV_BLEND_FACTOR;
    case 16: return BLEND_FACTOR_SRC_ALPHA_SAT;
    default: return BLEND_FACTOR_ZERO;
  }
}

BLEND_OPERATION BlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract: return BLEND_OPERATION_SUBTRACT;
    case xenos::BlendOp::kMin: return BLEND_OPERATION_MIN;
    case xenos::BlendOp::kMax: return BLEND_OPERATION_MAX;
    case xenos::BlendOp::kRevSubtract: return BLEND_OPERATION_REV_SUBTRACT;
    default: return BLEND_OPERATION_ADD;
  }
}

// Xenos compare functions and stencil operations are DiligentCore's, minus one.
uint8_t Compare(xenos::CompareFunction function) { return uint8_t(uint32_t(function) + 1); }
uint8_t Stencil(xenos::StencilOp op) { return uint8_t(uint32_t(op) + 1); }

constexpr uint32_t kBoolLoopConstantsSize = (8 + 32) * 4;
constexpr uint32_t kFetchConstantsSize = 32 * 6 * 4;

RefCntAutoPtr<IBuffer> CreateConstantBuffer(IRenderDevice* device, const char* name, uint32_t size) {
  BufferDesc desc;
  desc.Name = name;
  desc.Size = size;
  desc.Usage = USAGE_DYNAMIC;
  desc.BindFlags = BIND_UNIFORM_BUFFER;
  desc.CPUAccessFlags = CPU_ACCESS_WRITE;
  RefCntAutoPtr<IBuffer> buffer;
  device->CreateBuffer(desc, nullptr, &buffer);
  return buffer;
}

// A dynamic buffer mapped for writing (new memory each time) until the end of the scope.
class MappedBuffer {
 public:
  MappedBuffer(IDeviceContext* context, IBuffer* buffer) : context_(context), buffer_(buffer) {
    context->MapBuffer(buffer, MAP_WRITE, MAP_FLAG_DISCARD, data_);
  }
  ~MappedBuffer() {
    if (data_) context_->UnmapBuffer(buffer_, MAP_WRITE);
  }
  uint8_t* data() const { return static_cast<uint8_t*>(data_); }

 private:
  IDeviceContext* context_;
  IBuffer* buffer_;
  void* data_ = nullptr;
};

// A warning logged the first time only (per call site).
#define XENOS_NATIVE_WARN_ONCE(...)   do {                                  static bool logged = false;         if (!logged) {                        logged = true;                      REXLOG_WARN(__VA_ARGS__);         }                                 } while (false)

}  // namespace

DrawPath::DrawPath() = default;
DrawPath::~DrawPath() {
  if (save_thread_.joinable()) save_thread_.join();
}

bool DrawPath::Initialize(Device& device, rg::RegisterFile& registers, rex::memory::Memory& memory,
                          ResolvedTextures& resolved, const RenderScale& scale,
                          const std::filesystem::path& storage) {
  device_ = &device;
  registers_ = &registers;
  scale_ = scale;
  IRenderDevice* render_device = device.device();
  IDeviceContext* context = device.context();

  memory_ = std::make_unique<GuestMemory>(memory, render_device, context);
  if (!memory_->Initialize()) return false;
  primitives_ = std::make_unique<Primitives>(registers, memory, *memory_, render_device, context);
  if (!primitives_->Initialize()) {
    REXLOG_ERROR("[xenos_native] could not initialize primitive processing");
    return false;
  }
  shaders_ = std::make_unique<ShaderCache>();
  if (!shaders_->Initialize(render_device, scale_)) return false;
  guest_textures_ = std::make_unique<GuestTextures>(registers, *memory_, resolved, render_device, context);
  if (!guest_textures_->Initialize()) return false;
  // Frame numbers are the device's: frame N is recorded after N - 1 presents and complete when the device's
  // frame fence reaches N.
  guest_textures_->BeginFrame(device.frames_presented() + 1, device.frames_completed());
  textures_ = std::make_unique<Textures>();
  if (!textures_->Initialize(render_device, resolved, *guest_textures_)) return false;

  system_constants_buffer_ =
      CreateConstantBuffer(render_device, "XenosNative system constants", sizeof(Translator::SystemConstants));
  bool_loop_constants_buffer_ =
      CreateConstantBuffer(render_device, "XenosNative bool/loop constants", kBoolLoopConstantsSize);
  fetch_constants_buffer_ = CreateConstantBuffer(render_device, "XenosNative fetch constants", kFetchConstantsSize);
  if (!system_constants_buffer_ || !bool_loop_constants_buffer_ || !fetch_constants_buffer_) {
    return false;
  }

  PipelineBuffers buffers;
  buffers.shared_memory = memory_->view();
  buffers.system_constants = system_constants_buffer_;
  buffers.bool_loop_constants = bool_loop_constants_buffer_;
  buffers.fetch_constants = fetch_constants_buffer_;
  pipelines_ = std::make_unique<PipelineCache>();
  pipelines_->Initialize(render_device, buffers);
  if (!storage.empty()) OpenStorage(storage);
  return true;
}

void DrawPath::OpenStorage(const std::filesystem::path& storage) {
  IRenderDevice* render_device = device_->device();
  // DiligentCore's pipeline cache: the driver's binaries of the pipelines compiled before. Vulkan ignores
  // data from another device or driver.
  pipeline_cache_path_ = storage;
  pipeline_cache_path_ += ".vkcache";
  std::vector<uint8_t> blob;
  {
    std::ifstream file(pipeline_cache_path_, std::ios::binary);
    if (file) blob.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  PipelineStateCacheCreateInfo cache_info;
  cache_info.Desc.Name = "XenosNative pipeline cache";
  cache_info.Desc.Mode = PSO_CACHE_MODE_LOAD_STORE;
  cache_info.pCacheData = blob.empty() ? nullptr : blob.data();
  cache_info.CacheDataSize = uint32_t(blob.size());
  render_device->CreatePipelineStateCache(cache_info, &pipeline_cache_);
  if (pipeline_cache_) pipelines_->SetCache(pipeline_cache_);

  // The shaders and pipelines of earlier runs: analyzed now, translated by the workers, and the pipelines
  // created as their shaders get ready (PrepareStored, every frame).
  std::filesystem::path records = storage;
  records += ".pipelines";
  storage_ = std::make_unique<PipelineStorage>();
  std::vector<PipelineStorage::Shader> shaders;
  std::vector<PipelineStorage::Translation> translations;
  std::vector<PipelineStorage::PipelineRecord> pipelines;
  if (!storage_->Open(records, shaders, translations, pipelines)) {
    storage_.reset();
    return;
  }
  for (const auto& shader : shaders) {
    shaders_->Get(xenos::ShaderType(shader.type), shader.ucode.data(), uint32_t(shader.ucode.size()));
  }
  bool pending = false;
  // Every stored translation, also those of draws that make no pipeline (no target, for one).
  for (const auto& translation : translations) {
    if (rg::SpirvShader* shader = shaders_->Find(xenos::ShaderType(translation.type), translation.hash)) {
      shaders_->GetHostShader(*shader, translation.modification, pending);
    }
  }
  for (const auto& record : pipelines) {
    Stored stored{shaders_->Find(xenos::ShaderType::kVertex, record.vertex_hash), record.vertex_modification,
                  record.pixel_hash ? shaders_->Find(xenos::ShaderType::kPixel, record.pixel_hash) : nullptr,
                  record.pixel_modification, record.state};
    if (!stored.vertex_shader || (record.pixel_hash && !stored.pixel_shader)) continue;
    // Queue the translations now; the workers start on them right away.
    shaders_->GetHostShader(*stored.vertex_shader, stored.vertex_modification, pending);
    if (stored.pixel_shader) shaders_->GetHostShader(*stored.pixel_shader, stored.pixel_modification, pending);
    stored_.push_back(stored);
  }
  shaders_->SetStorage(storage_.get());
  stored_total_ = uint32_t(stored_.size());
  stored_start_ = std::chrono::steady_clock::now();
  REXLOG_INFO("[xenos_native] pipeline storage {}: {} shaders, {} translations and {} pipelines from earlier runs; "
              "pipeline cache {} KB",
              records.string(), shaders.size(), translations.size(), pipelines.size(), blob.size() >> 10);
}

void DrawPath::PrepareStored() {
  if (stored_.empty()) return;
  // A bounded number per frame: each creation is quick (DiligentCore compiles them on its threads).
  constexpr uint32_t kPerFrame = 64;
  uint32_t created = 0;
  for (size_t i = 0; i < stored_.size() && created < kPerFrame;) {
    Stored& s = stored_[i];
    bool vertex_pending = false, pixel_pending = false;
    IShader* vertex = shaders_->GetHostShader(*s.vertex_shader, s.vertex_modification, vertex_pending);
    IShader* pixel =
        s.pixel_shader ? shaders_->GetHostShader(*s.pixel_shader, s.pixel_modification, pixel_pending) : nullptr;
    if (vertex_pending || pixel_pending) {
      ++i;
      continue;
    }
    if (vertex && (pixel || !s.pixel_shader)) {
      s.state.vertex_shader = vertex;
      s.state.pixel_shader = pixel;
      pipelines_->Get(s.state, *s.vertex_shader, s.pixel_shader);
      ++created;
    }
    s = stored_.back();
    stored_.pop_back();
  }
  if (stored_.empty()) {
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stored_start_).count();
    REXLOG_INFO("[xenos_native] pipeline storage: the {} stored pipelines created in {:.0f} ms after the first draw "
                "(their compilation continues on DiligentCore's threads)",
                stored_total_, ms);
  }
}

void DrawPath::SavePipelineCache() {
  if (!pipeline_cache_ || pipeline_cache_path_.empty() || saving_) return;
  if (save_thread_.joinable()) save_thread_.join();  // the last save, finished
  // On a thread of its own (D45): the driver's serialization and the write. Vulkan's pipeline cache is
  // synchronized internally, so pipelines can be created meanwhile; if one is added between GetData's two
  // calls, GetData returns nothing and the save is tried again later. The file is written next to the
  // cache and renamed over it, so a process killed while writing leaves the last complete cache.
  saving_ = true;
  save_thread_ = std::thread([cache = pipeline_cache_, path = pipeline_cache_path_, created = pipelines_unsaved_,
                              this]() {
    auto start = std::chrono::steady_clock::now();
    RefCntAutoPtr<IDataBlob> blob;
    cache->GetData(&blob);
    bool saved = false;
    if (blob) {
      std::filesystem::path temporary = path;
      temporary += ".tmp";
      {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(blob->GetConstDataPtr()), std::streamsize(blob->GetSize()));
        saved = bool(file);
      }
      std::error_code error;
      if (saved) std::filesystem::rename(temporary, path, error);
      saved = saved && !error;
    }
    if (saved) {
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      REXLOG_INFO("[xenos_native] pipeline cache saved: {} KB, {} new pipelines, in {:.0f} ms on its own thread",
                  blob->GetSize() >> 10, created, ms);
    } else {
      save_failed_ = true;
    }
    saving_ = false;
  });
  pipelines_unsaved_ = 0;
}

void DrawPath::Draw(const StateTracker& tracker, const uint8_t* base, const std::array<HostTarget*, 5>& targets,
                    bool tiled) {
  ++stats_.draws;
  ScopedRegisters overrides(*registers_);
  if (tiled) {
    // The tile's window offset and scissor are in bin-predicated packets, which the tracker skips. The
    // host target is the whole surface: no offset, no window scissor (the screen scissor still applies).
    rg::RegisterFile& regs = *registers_;
    overrides.Set(rg::XE_GPU_REG_PA_SC_WINDOW_OFFSET, 0);
    overrides.Set(rg::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, regs[rg::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] & 0x80000000u);
    overrides.Set(rg::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, 0x2000u | (0x2000u << 16));
  }
  if (DrawInternal(tracker, base, targets)) {
    ++stats_.drawn;
  }
}

bool DrawPath::DrawInternal(const StateTracker& tracker, const uint8_t* base,
                            const std::array<HostTarget*, 5>& targets) {
  const rg::RegisterFile& regs = *registers_;
  IDeviceContext* context = device_->context();

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) return false;

  // Shaders.
  // A program the SDK cannot analyze: where it came from, once (bring-up diagnostics).
  auto log_bad_program = [](const char* stage, const StateTracker::Program& program) {
    std::string dwords;
    for (size_t i = 0; i < program.ucode.size() && i < 512; ++i) {
      dwords += fmt::format(" {:08X}", rex::byte_swap(program.ucode[i]));
    }
    REXLOG_ERROR("[xenos_native] the {} program came from {} at {:#x} (packet {:#x}) in {}:{}", stage,
                 program.immediate ? "IM_LOAD_IMMEDIATE" : "IM_LOAD", program.address, program.packet,
                 CallName(program.loaded_by), dwords);
  };
  const StateTracker::Program& vertex_program = tracker.vertex_program();
  if (vertex_program.ucode.empty()) return false;
  bool new_failure = false;
  rg::SpirvShader* vertex_shader = shaders_->Get(xenos::ShaderType::kVertex, vertex_program.ucode.data(),
                                                 uint32_t(vertex_program.ucode.size()), &new_failure);
  if (!vertex_shader) {
    if (new_failure) log_bad_program("vertex", vertex_program);
    ++stats_.failed;
    return false;
  }
  if (vertex_shader->memexport_eM_written()) {
    XENOS_NATIVE_WARN_ONCE("[xenos_native] a vertex shader uses memory export: not supported yet");
    return false;
  }
  bool polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, polygonal)) return false;
  rg::SpirvShader* pixel_shader = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth) {
    const StateTracker::Program& pixel_program = tracker.pixel_program();
    if (!pixel_program.ucode.empty()) {
      pixel_shader = shaders_->Get(xenos::ShaderType::kPixel, pixel_program.ucode.data(),
                                   uint32_t(pixel_program.ucode.size()), &new_failure);
      if (!pixel_shader) {
        if (new_failure) log_bad_program("pixel", pixel_program);
        ++stats_.failed;
        return false;
      }
      if (pixel_shader->memexport_eM_written()) {
        XENOS_NATIVE_WARN_ONCE("[xenos_native] a pixel shader uses memory export: not supported yet");
        return false;
      }
      if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) pixel_shader = nullptr;
    }
  }
  reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t param_gen_position = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? vertex_shader->writes_interpolators() &
                         pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                                regs.Get<reg::SQ_CONTEXT_MISC>(), param_gen_position)
                   : 0;

  // Primitives and indices (this may upload the index range).
  rg::PrimitiveProcessor::ProcessingResult primitives;
  if (!primitives_->Process(primitives)) {
    XENOS_NATIVE_WARN_ONCE("[xenos_native] primitive processing failed (primitive type {})",
            uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type));
    ++stats_.failed;
    return false;
  }
  static int32_t logged_draws = 0;
  if (logged_draws < REXCVAR_GET(xenos_native_draw_log)) {
    ++logged_draws;
    REXLOG_INFO("[xenos_native] draw {}: VGT_DRAW_INITIATOR {:08X} DMA base {:08X} size {:08X}; guest {} x{} -> host {} x{}, "
                "index buffer type {}, VS type {}, VS {:016X} PS {:016X}",
                logged_draws, regs[rg::XE_GPU_REG_VGT_DRAW_INITIATOR], regs[rg::XE_GPU_REG_VGT_DMA_BASE],
                regs[rg::XE_GPU_REG_VGT_DMA_SIZE], uint32_t(primitives.guest_primitive_type),
                primitives.guest_draw_vertex_count, uint32_t(primitives.host_primitive_type),
                primitives.host_draw_vertex_count, uint32_t(primitives.index_buffer_type),
                uint32_t(primitives.host_vertex_shader_type), vertex_shader->ucode_data_hash(),
                pixel_shader ? pixel_shader->ucode_data_hash() : 0);
  }
  if (!primitives.host_draw_vertex_count) return false;
  PRIMITIVE_TOPOLOGY topology;
  switch (primitives.host_primitive_type) {
    case xenos::PrimitiveType::kPointList: topology = PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case xenos::PrimitiveType::kLineList: topology = PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case xenos::PrimitiveType::kLineStrip: topology = PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case xenos::PrimitiveType::kTriangleList: topology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case xenos::PrimitiveType::kTriangleStrip: topology = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    default:
      XENOS_NATIVE_WARN_ONCE("[xenos_native] host primitive type {} not supported",
              uint32_t(primitives.host_primitive_type));
      ++stats_.failed;
      return false;
  }

  // Shader modifications (VulkanPipelineCache::GetCurrent*ShaderModification, for a device without clip
  // and cull distances).
  Translator& translator = shaders_->translator();
  auto program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  auto clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  xenos::PrimitiveType guest_primitive_type = regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type;
  Translator::Modification vertex_modification(translator.GetDefaultVertexShaderModification(
      vertex_shader->GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg), primitives.host_vertex_shader_type));
  vertex_modification.vertex.interpolator_mask = interpolator_mask;
  vertex_modification.vertex.tessellation_mode = 0;
  vertex_modification.vertex.user_clip_plane_count = 0;
  vertex_modification.vertex.user_clip_plane_cull = 0;
  vertex_modification.vertex.point_ps_ucp_mode = clip_cntl.ps_ucp_mode;
  if (primitives.host_vertex_shader_type == rg::Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    vertex_modification.vertex.output_point_parameters = uint32_t(param_gen_position != UINT32_MAX);
  } else {
    vertex_modification.vertex.output_point_parameters =
        uint32_t((vertex_shader->writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 guest_primitive_type == xenos::PrimitiveType::kPointList);
  }
  vertex_modification.vertex.vertex_kill_and = 0;
  // Translated on a worker thread (D34): until then the draw is skipped.
  bool translating = false;
  IShader* host_vertex_shader = shaders_->GetHostShader(*vertex_shader, vertex_modification.value, translating);
  if (!host_vertex_shader && !translating) {
    ++stats_.failed;
    return false;
  }
  IShader* host_pixel_shader = nullptr;
  uint64_t pixel_modification_value = 0;  // for the storage (D42)
  if (pixel_shader) {
    Translator::Modification pixel_modification(translator.GetDefaultPixelShaderModification(
        pixel_shader->GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg)));
    pixel_modification.pixel.interpolator_mask = interpolator_mask;
    pixel_modification.pixel.interpolators_centroid =
        interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                                                                   regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                                                   regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (param_gen_position < xenos::kMaxInterpolators) {
      pixel_modification.pixel.param_gen_enable = 1;
      pixel_modification.pixel.param_gen_interpolator = param_gen_position;
      pixel_modification.pixel.param_gen_point = uint32_t(guest_primitive_type == xenos::PrimitiveType::kPointList);
    } else {
      pixel_modification.pixel.param_gen_enable = 0;
      pixel_modification.pixel.param_gen_interpolator = 0;
      pixel_modification.pixel.param_gen_point = 0;
    }
    using DepthStencilMode = Translator::Modification::DepthStencilMode;
    pixel_modification.pixel.depth_stencil_mode =
        pixel_shader->implicit_early_z_write_allowed() &&
                (!pixel_shader->writes_color_target(0) ||
                 !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))
            ? DepthStencilMode::kEarlyHint
            : DepthStencilMode::kNoModifiers;
    bool pixel_translating = false;
    pixel_modification_value = pixel_modification.value;
    host_pixel_shader = shaders_->GetHostShader(*pixel_shader, pixel_modification.value, pixel_translating);
    if (!host_pixel_shader && !pixel_translating) {
      ++stats_.failed;
      return false;
    }
    translating |= pixel_translating;
  }
  if (translating) {
    ++stats_.translating;
    return false;
  }

  // Render targets: the colors the pixel shader writes with a non-empty mask, depth if tested.
  uint32_t color_mask = pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets()) : 0;
  ITextureView* render_target_views[4] = {};
  uint32_t render_target_count = 0, samples = 0;
  PipelineState state;
  state.Reset();
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((color_mask >> (4 * i)) & 0xF)) continue;
    HostTarget* target = targets[i];
    if (!target) {
      color_mask &= ~(0xFu << (4 * i));
      continue;
    }
    render_target_views[i] = target->view;
    state.rtv_formats[i] = uint16_t(target->format);
    render_target_count = i + 1;
    samples = target->samples;
  }
  HostTarget* depth = (depth_control.z_enable || depth_control.stencil_enable) ? targets[4] : nullptr;
  if (!render_target_count && !depth) {
    ++stats_.no_targets;
    return false;
  }
  if (depth) {
    if (samples && depth->samples != samples) {
      XENOS_NATIVE_WARN_ONCE("[xenos_native] color and depth targets with different sample counts");
      ++stats_.failed;
      return false;
    }
    samples = depth->samples;
    state.dsv_format = uint16_t(depth->format);
  }

  // Pipeline state (VulkanPipelineCache::GetCurrentStateDescription, in DiligentCore terms).
  state.vertex_shader = host_vertex_shader;
  state.pixel_shader = host_pixel_shader;
  state.render_target_count = uint8_t(render_target_count);
  state.samples = uint8_t(samples);
  state.topology = uint8_t(topology);
  auto mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  state.cull_mode = uint8_t(CULL_MODE_NONE);
  if (polygonal) {
    if (mode_cntl.cull_front) state.cull_mode = uint8_t(CULL_MODE_FRONT);
    if (mode_cntl.cull_back) state.cull_mode = uint8_t(CULL_MODE_BACK);
    xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
    if (mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
      if (!mode_cntl.cull_front) polygon_type = std::min(polygon_type, mode_cntl.polymode_front_ptype);
      if (!mode_cntl.cull_back) polygon_type = std::min(polygon_type, mode_cntl.polymode_back_ptype);
    }
    state.wireframe = polygon_type != xenos::PolygonType::kTriangles &&
                      device_->device()->GetDeviceInfo().Features.WireframeFill == DEVICE_FEATURE_STATE_ENABLED;
    // The final pixels are the SDK Vulkan path's (see the viewport below), so its winding rule holds.
    state.front_counter_clockwise = mode_cntl.face ? 0 : 1;
  }
  state.depth_clip = !(clip_cntl.clip_disable &&
                       device_->device()->GetDeviceInfo().Features.DepthClamp == DEVICE_FEATURE_STATE_ENABLED);
  uint8_t stencil_reference = 0;
  if (depth) {
    state.depth_enable = depth_control.z_enable;
    state.depth_write = depth_control.z_enable && depth_control.z_write_enable;
    state.depth_func = depth_control.z_enable ? Compare(depth_control.zfunc) : Compare(xenos::CompareFunction::kAlways);
    if (depth_control.stencil_enable) {
      state.stencil_enable = 1;
      state.stencil_front[0] = Stencil(depth_control.stencilfail);
      state.stencil_front[1] = Stencil(depth_control.stencilzfail);
      state.stencil_front[2] = Stencil(depth_control.stencilzpass);
      state.stencil_front[3] = Compare(depth_control.stencilfunc);
      bool back = polygonal && depth_control.backface_enable;
      state.stencil_back[0] = back ? Stencil(depth_control.stencilfail_bf) : state.stencil_front[0];
      state.stencil_back[1] = back ? Stencil(depth_control.stencilzfail_bf) : state.stencil_front[1];
      state.stencil_back[2] = back ? Stencil(depth_control.stencilzpass_bf) : state.stencil_front[2];
      state.stencil_back[3] = back ? Compare(depth_control.stencilfunc_bf) : state.stencil_front[3];
      // One mask and reference for both faces: the back face's only when drawing back faces only (the
      // SDK's fallback for devices without separate masks).
      auto ref_mask = regs.Get<reg::RB_STENCILREFMASK>(back && mode_cntl.cull_front ? rg::XE_GPU_REG_RB_STENCILREFMASK_BF
                                                                                    : rg::XE_GPU_REG_RB_STENCILREFMASK);
      state.stencil_read_mask = uint8_t(ref_mask.stencilmask);
      state.stencil_write_mask = uint8_t(ref_mask.stencilwritemask);
      stencil_reference = uint8_t(ref_mask.stencilref);
    }
    float slope = 0.0f, constant = 0.0f;
    draw_util::GetPreferredFacePolygonOffset(regs, polygonal, slope, constant);
    state.depth_bias = draw_util::GetD3D10IntegerPolygonOffset(regs.Get<reg::RB_DEPTH_INFO>().depth_format, constant);
    state.slope_scaled_depth_bias = slope * xenos::kPolygonOffsetScaleSubpixelUnit;
  } else {
    state.depth_func = Compare(xenos::CompareFunction::kAlways);
  }
  for (uint32_t i = 0; i < render_target_count; ++i) {
    PipelineState::Target& target = state.targets[i];
    target.write_mask = uint8_t((color_mask >> (4 * i)) & 0xF);
    if (!target.write_mask) continue;
    auto blend = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
    target.src = uint8_t(BlendFactor(blend.color_srcblend, false));
    target.dst = uint8_t(BlendFactor(blend.color_destblend, false));
    target.op = uint8_t(BlendOp(blend.color_comb_fcn));
    target.src_alpha = uint8_t(BlendFactor(blend.alpha_srcblend, true));
    target.dst_alpha = uint8_t(BlendFactor(blend.alpha_destblend, true));
    target.op_alpha = uint8_t(BlendOp(blend.alpha_comb_fcn));
    target.blend_enable = !(target.src == BLEND_FACTOR_ONE && target.dst == BLEND_FACTOR_ZERO &&
                            target.op == BLEND_OPERATION_ADD && target.src_alpha == BLEND_FACTOR_ONE &&
                            target.dst_alpha == BLEND_FACTOR_ZERO && target.op_alpha == BLEND_OPERATION_ADD);
    if (!target.blend_enable) target.src = target.dst = target.op = target.src_alpha = target.dst_alpha = target.op_alpha = 0;
  }
  bool created = false;
  Pipeline* pipeline = pipelines_->Get(state, *vertex_shader, pixel_shader, &created);
  if (created) {
    // Kept for the next run (D42).
    if (storage_) {
      storage_->AddPipeline({vertex_shader->ucode_data_hash(), vertex_modification.value,
                             pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                             pixel_modification_value, state});
    }
    ++pipelines_unsaved_;
    frames_since_created_ = 0;
  }
  if (!pipeline) {
    XENOS_NATIVE_WARN_ONCE("[xenos_native] pipeline creation failed (more failures are not logged)");
    ++stats_.failed;
    return false;
  }
  if (!pipelines_->Ready(*pipeline)) {
    // Still compiling (D28), or failed (logged): the draw is skipped.
    ++(pipeline->failed ? stats_.failed : stats_.compiling);
    return false;
  }

  // Viewport and scissor. DiligentCore's Vulkan backend flips Y with a negative viewport height, as
  // Direct3D does, so the translated shaders produce bottom-left-origin NDC (the SDK's D3D12 setting).
  // Both are computed in guest pixels and scaled when they are set (D37): the viewport's NDC transform is
  // relative to it, and Vulkan takes a fractional viewport.
  // Float24 depth keeps the guest's values (full_float24_in_0_to_1 false): the SDK halves it to move EDRAM
  // data through depth and doubles it back in its resolves, but here resolves copy host depth as it is,
  // and shaders compare sampled depth with depth they compute (shadow maps).
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(regs, 1, 1, true, 16384, 16384, true, depth_control, false, false,
                                 pixel_shader && pixel_shader->writes_depth(), viewport_info);
  if (!viewport_info.xy_extent[0] || !viewport_info.xy_extent[1]) return false;
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  if (!scissor.extent[0] || !scissor.extent[1]) return false;

  // Textures: loaded (or found up to date) before the vertex data, as the SDK does; the loads upload the
  // guest memory they read.
  uint32_t used_texture_mask = vertex_shader->GetUsedTextureMaskAfterTranslation() |
                               (pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  guest_textures_->RequestTextures(used_texture_mask);

  // Vertex data: every vertex fetch constant the vertex shader uses.
  const rg::Shader::ConstantRegisterMap& vertex_constants = vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < std::size(vertex_constants.vertex_fetch_bitmap); ++i) {
    uint32_t bits = vertex_constants.vertex_fetch_bitmap[i];
    uint32_t bit;
    while (rex::bit_scan_forward(bits, &bit)) {
      bits &= ~(1u << bit);
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(i * 32 + bit);
      if (fetch.type != xenos::FetchConstantType::kVertex) continue;
      memory_->RequestRange(fetch.address << 2, fetch.size << 2);
    }
  }
  if (primitives_->UploadConverted()) ++stats_.copies;
  stats_.copies += memory_->TakeCopies();
  stats_.uploaded_bytes += memory_->TakeUploadedBytes();
  stats_.unchanged_bytes += memory_->TakeUnchangedBytes();
  memory_->PrepareForRead(context);

  // Constants.
  DrawContext draw{&regs, &primitives, &viewport_info, polygonal, used_texture_mask};
  FillSystemConstants(draw);
  {
    MappedBuffer map(context, system_constants_buffer_);
    std::memcpy(map.data(), &system_constants_, sizeof(system_constants_));
  }
  const rg::SpirvShader* stage_shaders[2] = {vertex_shader, pixel_shader};
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const rg::SpirvShader* shader = stage_shaders[stage];
    if (!shader || !shader->constant_register_map().float_count) continue;
    MappedBuffer map(context, pipeline->float_constants[stage]);
    uint8_t* out = map.data();
    const rg::Shader::ConstantRegisterMap& constants = shader->constant_register_map();
    uint32_t first = stage ? rg::XE_GPU_REG_SHADER_CONSTANT_256_X : rg::XE_GPU_REG_SHADER_CONSTANT_000_X;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = constants.float_bitmap[i];
      uint32_t index;
      while (rex::bit_scan_forward(bits, &index)) {
        bits &= ~(uint64_t(1) << index);
        std::memcpy(out, &regs[first + (i << 8) + (index << 2)], 16);
        out += 16;
      }
    }
  }
  {
    MappedBuffer map(context, bool_loop_constants_buffer_);
    std::memcpy(map.data(), &regs[rg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], kBoolLoopConstantsSize);
  }
  {
    MappedBuffer map(context, fetch_constants_buffer_);
    std::memcpy(map.data(), &regs[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
  }

  // Textures and samplers.
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const rg::SpirvShader* shader = stage_shaders[stage];
    if (!shader) continue;
    const auto& texture_bindings = shader->GetTextureBindingsAfterTranslation();
    for (size_t i = 0; i < texture_bindings.size() && i < pipeline->textures[stage].size(); ++i) {
      if (IShaderResourceVariable* variable = pipeline->textures[stage][i]) {
        const auto& binding = texture_bindings[i];
        variable->Set(textures_->View(binding.fetch_constant, regs.GetTextureFetch(binding.fetch_constant),
                                      binding.dimension, binding.is_signed));
      }
    }
    const auto& sampler_bindings = shader->GetSamplerBindingsAfterTranslation();
    for (size_t i = 0; i < sampler_bindings.size() && i < pipeline->samplers[stage].size(); ++i) {
      if (IShaderResourceVariable* variable = pipeline->samplers[stage][i]) {
        variable->Set(textures_->Sampler(regs.GetTextureFetch(sampler_bindings[i].fetch_constant), sampler_bindings[i]));
      }
    }
  }

  // Record the draw. The targets are bound only when they change (D30).
  std::array<ITextureView*, 5> views{render_target_views[0], render_target_views[1], render_target_views[2],
                                     render_target_views[3], depth ? depth->view : nullptr};
  if (!bound_valid_ || bound_count_ != render_target_count || bound_ != views) {
    context->SetRenderTargets(render_target_count, render_target_views, views[4],
                              RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    bound_ = views;
    bound_count_ = render_target_count;
    bound_valid_ = true;
  }
  context->SetPipelineState(pipeline->pso);
  context->CommitShaderResources(pipeline->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  // Guest pixels to host pixels: the render scale per axis (D37, D38); in a 2D layer kept at the game's
  // aspect ratio (D43), the smaller of the two on both axes, and an offset that centers the target's guest
  // area in its host size.
  float scale_x = scale_.x, scale_y = scale_.y, offset_x = 0.0f, offset_y = 0.0f;
  if (aspect_box_ && scale_.x != scale_.y) {
    const HostTarget* sized = depth;
    for (uint32_t i = 0; i < render_target_count; ++i) {
      if (render_target_views[i]) {
        sized = targets[i];
        break;
      }
    }
    float uniform = std::min(scale_.x, scale_.y);
    offset_x = float(sized->guest_width) * (scale_.x - uniform) * 0.5f;
    offset_y = float(sized->guest_height) * (scale_.y - uniform) * 0.5f;
    scale_x = scale_y = uniform;
  }
  // As RenderScale::HostX/HostY outside a box.
  auto host_x = [&](int64_t x) { return int32_t(std::floor(double(offset_x) + double(x) * scale_x + 0.5)); };
  auto host_y = [&](int64_t y) { return int32_t(std::floor(double(offset_y) + double(y) * scale_y + 0.5)); };
  Viewport viewport;
  viewport.TopLeftX = offset_x + float(viewport_info.xy_offset[0]) * scale_x;
  viewport.TopLeftY = offset_y + float(viewport_info.xy_offset[1]) * scale_y;
  viewport.Width = float(viewport_info.xy_extent[0]) * scale_x;
  viewport.Height = float(viewport_info.xy_extent[1]) * scale_y;
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  context->SetViewports(1, &viewport, 0, 0);
  Rect scissor_rect{host_x(scissor.offset[0]), host_y(scissor.offset[1]),
                    host_x(int64_t(scissor.offset[0]) + scissor.extent[0]),
                    host_y(int64_t(scissor.offset[1]) + scissor.extent[1])};
  context->SetScissorRects(1, &scissor_rect, 0, 0);
  if (state.stencil_enable) context->SetStencilRef(stencil_reference);
  const float blend_factors[4] = {regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_GREEN),
                                  regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_ALPHA)};
  context->SetBlendFactors(blend_factors);

  if (primitives.index_buffer_type == rg::PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    DrawAttribs attribs{primitives.host_draw_vertex_count, DRAW_FLAG_NONE};
    context->Draw(attribs);
    return true;
  }
  IBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  switch (primitives.index_buffer_type) {
    case rg::PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
      index_buffer = memory_->buffer();
      index_offset = primitives.guest_index_base;
      break;
    case rg::PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
      index_buffer = primitives_->converted_buffer();
      index_offset = primitives_->ConvertedOffset(primitives.host_index_buffer_handle);
      break;
    case rg::PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
    case rg::PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
      index_buffer = primitives_->builtin_buffer();
      index_offset = Primitives::BuiltinOffset(primitives.host_index_buffer_handle);
      break;
    default:
      ++stats_.failed;
      return false;
  }
  context->SetIndexBuffer(index_buffer, index_offset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawIndexedAttribs attribs{primitives.host_draw_vertex_count,
                             primitives.host_index_format == xenos::IndexFormat::kInt16 ? VT_UINT16 : VT_UINT32,
                             DRAW_FLAG_NONE};
  context->DrawIndexed(attribs);
  return true;
}

// VulkanCommandProcessor::UpdateSystemConstantValues, for the host render target path (no fragment shader
// interlock) at resolution scale 1. Fields that do not apply to a draw keep their last value, as there.
void DrawPath::FillSystemConstants(const DrawContext& draw) {
  const rg::RegisterFile& regs = *draw.regs;
  const auto& primitives = *draw.primitives;
  const auto& viewport = *draw.viewport;
  Translator::SystemConstants& c = system_constants_;
  using PIBT = rg::PrimitiveProcessor::ProcessedIndexBufferType;

  auto vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  uint32_t flags = 0;
  if (primitives.index_buffer_type == PIBT::kHostBuiltinForDMA || primitives.index_buffer_type == PIBT::kGuestDMA ||
      primitives.index_buffer_type == PIBT::kHostConverted) {
    flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    if (primitives.host_primitive_reset_enabled &&
        primitives.host_vertex_shader_type != rg::Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        primitives.host_vertex_shader_type != rg::Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexReset;
    }
  }
  if (vte_cntl.vtx_xy_fmt) flags |= Translator::kSysFlag_XYDividedByW;
  if (vte_cntl.vtx_z_fmt) flags |= Translator::kSysFlag_ZDividedByW;
  if (vte_cntl.vtx_w0_fmt) flags |= Translator::kSysFlag_WNotReciprocal;
  if (draw.polygonal) flags |= Translator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(regs)) flags |= Translator::kSysFlag_PrimitiveLine;
  flags |= uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples) << Translator::kSysFlag_MsaaSamples_Shift;
  if (regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= Translator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test =
      colorcontrol.alpha_test_enable ? colorcontrol.alpha_func : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test) << Translator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma render targets are 8-bit here (targets.cpp): the shader writes gamma-encoded values.
  reg::RB_COLOR_INFO color_infos[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= Translator::kSysFlag_ConvertColor0ToGamma << i;
    }
  }
  c.flags = flags;

  if (flags & Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad) {
    c.vertex_index_load_address = primitives.guest_index_base;
  }
  c.vertex_index_reset = (flags & Translator::kSysFlag_ComputeOrPrimitiveVertexIndexReset)
                             ? regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx
                             : 0;
  c.compute_memexport_vertex_count = 0;
  // The endian the processed indices need (the SDK uses the guest's whenever the shader may load indices,
  // which differs only for pre-swapped converted indices).
  c.vertex_index_endian = primitives.host_shader_index_endian;
  c.line_loop_closing_index = primitives.line_loop_closing_index;
  c.vertex_base_index = regs.Get<int32_t>(rg::XE_GPU_REG_VGT_INDX_OFFSET);
  c.vertex_index_min = regs.Get<uint32_t>(rg::XE_GPU_REG_VGT_MIN_VTX_INDX);
  c.vertex_index_max = regs.Get<uint32_t>(rg::XE_GPU_REG_VGT_MAX_VTX_INDX);

  // User clip planes: none on this device (the modification says 0 planes).
  (void)clip_cntl;

  c.tessellation_factor_range_min = regs.Get<float>(rg::XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  c.tessellation_factor_range_max = regs.Get<float>(rg::XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;

  for (uint32_t i = 0; i < 3; ++i) {
    c.ndc_scale[i] = viewport.ndc_scale[i];
    c.ndc_offset[i] = viewport.ndc_offset[i];
  }

  if (draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    c.point_vertex_diameter_min = float(point_minmax.min_size) * (2.0f / 16.0f);
    c.point_vertex_diameter_max = float(point_minmax.max_size) * (2.0f / 16.0f);
    c.point_constant_diameter[0] = float(point_size.width) * (2.0f / 16.0f);
    c.point_constant_diameter[1] = float(point_size.height) * (2.0f / 16.0f);
    c.point_screen_diameter_to_ndc_radius[0] = 1.0f / float(std::max(viewport.xy_extent[0], uint32_t(1)));
    c.point_screen_diameter_to_ndc_radius[1] = 1.0f / float(std::max(viewport.xy_extent[1], uint32_t(1)));
  }

  // Texture signs and swizzles (the swizzle is applied in the shader: the views are RGBA).
  uint32_t remaining = draw.used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(remaining, &index)) {
    remaining &= ~(1u << index);
    uint32_t swizzle;
    uint8_t signs;
    textures_->HostSwizzle(index, regs.GetTextureFetch(index), swizzle, signs);
    uint32_t& signs_word = c.texture_swizzled_signs[index >> 2];
    uint32_t signs_shift = 8 * (index & 3);
    signs_word = (signs_word & ~(0xFFu << signs_shift)) | (uint32_t(signs) << signs_shift);
    uint32_t& swizzle_word = c.texture_swizzles[index >> 1];
    uint32_t swizzle_shift = 12 * (index & 1);
    swizzle_word = (swizzle_word & ~(0xFFFu << swizzle_shift)) | (swizzle << swizzle_shift);
  }
  // Resolve results are at the render scale: the shaders revert it in texel offsets and weights (D37).
  c.textures_resolution_scaled = 0;
  if (scale_.scaled()) {
    uint32_t used = draw.used_texture_mask;
    while (rex::bit_scan_forward(used, &index)) {
      used &= ~(1u << index);
      if (textures_->IsResolved(regs.GetTextureFetch(index))) c.textures_resolution_scaled |= 1u << index;
    }
  }

  c.alpha_test_reference = regs.Get<float>(rg::XE_GPU_REG_RB_ALPHA_REF);
  c.alpha_to_mask = colorcontrol.alpha_to_mask_enable ? (colorcontrol.value >> 24) | (1u << 8) : 0;

  // Color exponent bias. 16-bit fixed-point targets are SNORM here, truncated to -1...1: no extra bias.
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t bias = color_infos[i].color_exp_bias;
    uint32_t bits = 0x3F800000u + (uint32_t(bias) << 23);
    std::memcpy(&c.color_exp_bias[i], &bits, sizeof(bits));
  }
}

void DrawPath::EndFrame() {
  // The stored pipelines (D42): created as their shaders get ready; new records written; the pipeline
  // cache saved, on a thread of its own (D45), once nothing new was compiled for 10 s (at 60 FPS).
  PrepareStored();
  if (storage_) storage_->Flush();
  if (save_failed_.exchange(false)) {
    pipelines_unsaved_ = std::max(pipelines_unsaved_, 1u);  // try again in 600 frames
    frames_since_created_ = 0;
  }
  if (pipelines_unsaved_ && ++frames_since_created_ >= 600 && stored_.empty()) SavePipelineCache();
  if (primitives_) primitives_->EndFrame();
  if (guest_textures_) guest_textures_->BeginFrame(device_->frames_presented() + 1, device_->frames_completed());
}

DrawPath::Stats DrawPath::TakeStats() {
  Stats stats = stats_;
  stats_ = {};
  if (textures_) {
    Textures::Stats textures = textures_->TakeStats();
    stats.textures_resolved = textures.resolved;
    stats.textures_guest = textures.guest;
    stats.textures_other_dimension = textures.other_dimension;
    stats.textures_placeholder = textures.placeholders;
  }
  if (shaders_) {
    ShaderCache::Times times = shaders_->TakeTimes();
    stats.analyses = times.analysis;
    stats.translations = times.translation;
  }
  if (pipelines_) stats.pipelines = pipelines_->TakeTime();
  return stats;
}

size_t DrawPath::shader_count() const { return shaders_ ? shaders_->count() : 0; }
size_t DrawPath::pipeline_count() const { return pipelines_ ? pipelines_->count() : 0; }
uint64_t DrawPath::texture_loads() const { return guest_textures_ ? guest_textures_->stats().loads : 0; }
uint64_t DrawPath::texture_loaded_bytes() const { return guest_textures_ ? guest_textures_->stats().loaded_bytes : 0; }
uint64_t DrawPath::texture_loads_unchanged() const {
  return guest_textures_ ? guest_textures_->stats().unchanged : 0;
}
uint64_t DrawPath::textures_unsupported() const { return guest_textures_ ? guest_textures_->stats().unsupported : 0; }
uint64_t DrawPath::texture_count() const { return guest_textures_ ? guest_textures_->stats().textures : 0; }
uint64_t DrawPath::texture_host_bytes() const { return guest_textures_ ? guest_textures_->stats().host_bytes : 0; }

}  // namespace xenos_native
