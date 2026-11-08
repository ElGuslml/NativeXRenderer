// NativeXRenderer - see draw.h.

#include "draw.h"

#include "device.h"
#include "glsl.h"
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
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <xxhash.h>

REXCVAR_DEFINE_INT32(native_x_renderer_draw_log, 0, "NativeXRenderer",
                     "Bring-up: log the first N draws NativeXRenderer processes (primitive, indices, shaders)");
REXCVAR_DEFINE_STRING(native_x_renderer_alpha_to_coverage, "auto", "NativeXRenderer",
                      "Alpha to coverage on targets without MSAA (D69, D114): auto (blend on surfaces the game draws "
                      "with MSAA, dither on those it draws at 1x), blend (the color blended by its alpha, depth written "
                      "where the alpha is at least 0.5: close to the game's MSAA) or dither (the shader's 2x2 pattern, "
                      "as on the console without MSAA)");
REXCVAR_DEFINE_BOOL(native_x_renderer_scale_in_constants, true, "NativeXRenderer",
                    "The translated shaders read the render scale they revert from each draw's constants (read at "
                    "startup, D128): one translation, and so one driver pipeline cache, for every window size and "
                    "internal resolution, and a draw at another scale (reduced, a shadow map, a native copy of the "
                    "screen) reads its pixel position right; false: the scale is written into each translation");

namespace native_x_renderer {

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
  bool alpha_to_coverage_blended;  // D69: no dither; a depth-writing draw tests alpha against 0.5
  bool depth_write;
  RenderScale target_scale;  // the scale of the draw's own target: its pixel position reverts it (D128)
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
  std::array<std::pair<uint32_t, uint32_t>, 8> saved_{};
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

// The longest a draw into an offscreen target waits for its translations and pipeline before it is skipped after all
// (D101): a pipeline the driver has never seen takes up to a few hundred milliseconds.
constexpr std::chrono::milliseconds kPipelineWaitLimit{3000};
constexpr double kLongPipelineWaitMs = 50.0;

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

// The motion pixel shader (D48): the two positions the motion vertex shader writes, in clip space, to host
// pixels with the viewport's scale (NDC Y up, pixels Y down), from this frame's position to the last frame's.
RefCntAutoPtr<IShader> CreateMotionPixelShader(IRenderDevice* device) {
  std::string source =
      "#version 450\n"
      "layout(set = 0, binding = 0) uniform XeMotionConstants {\n"
      "  vec4 ndc_to_pixels;  // half the viewport's width, minus half its height\n"
      "};\n"
      "layout(location = " + std::to_string(ShaderCache::kMotionPreviousLocation) + ") in vec4 previous_position;\n"
      "layout(location = " + std::to_string(ShaderCache::kMotionCurrentLocation) + ") in vec4 current_position;\n"
      "layout(location = 0) out vec2 out_motion;\n"
      "void main() {\n"
      "  out_motion = (previous_position.xy / previous_position.w - current_position.xy / current_position.w) *\n"
      "               ndc_to_pixels.xy;\n"
      "}\n";
  RefCntAutoPtr<IShader> shader;
  std::vector<uint32_t> spirv = CompileGlsl(GlslStage::kPixel, source, "NativeXRenderer motion PS");
  if (spirv.empty()) return shader;
  ShaderCreateInfo info;
  info.Desc.Name = "NativeXRenderer motion PS";
  info.Desc.ShaderType = SHADER_TYPE_PIXEL;
  info.Desc.UseCombinedTextureSamplers = true;
  info.ByteCode = spirv.data();
  info.ByteCodeSize = spirv.size() * sizeof(uint32_t);
  device->CreateShader(info, &shader);
  return shader;
}

// Whether every texture a pipeline's variables hold can be sampled in the state it is in (D82): what
// CommitShaderResources would otherwise transition, a texture loaded, copied or drawn into since its last commit.
// The test is DiligentCore's (ShaderResourceCacheVk::TransitionResources): depth textures are read as depth.
bool TextureReadable(ITextureView* view) {
  ITexture* texture = view->GetTexture();
  RESOURCE_STATE state = texture->GetState();
  RESOURCE_STATE required = (texture->GetDesc().BindFlags & BIND_DEPTH_STENCIL) ? RESOURCE_STATE_DEPTH_READ
                                                                                 : RESOURCE_STATE_SHADER_RESOURCE;
  return state == RESOURCE_STATE_UNKNOWN || (state & required) == required;
}

bool TexturesReadable(const Pipeline& pipeline) {
  for (const auto& objects : pipeline.texture_objects) {
    for (IDeviceObject* object : objects) {
      if (object && !TextureReadable(static_cast<ITextureView*>(object))) return false;
    }
  }
  return true;
}

// The same for a binding set's textures (D111).
bool TexturesReadable(const BindingSet& set) {
  for (ITextureView* view : set.views) {
    if (!TextureReadable(view)) return false;
  }
  return true;
}

// A warning logged the first time only (per call site).
#define NATIVE_X_RENDERER_WARN_ONCE(...)   do {                                  static bool logged = false;         if (!logged) {                        logged = true;                      REXLOG_WARN(__VA_ARGS__);         }                                 } while (false)

// A draw whose reprojection is the camera's within this many pixels of the game's frame moved with the camera only
// (D120): the camera's motion is its own, and it needs no motion draw. During the frame the vote's leader stands for
// the camera once this many draws agree on it and they are half of the offers or more.
constexpr double kCameraMotionPx = 0.1;
constexpr uint32_t kLeaderVotes = 8;

}  // namespace

// A main view draw that moved, kept for DrawObjectMotion (D48).
struct DrawPath::MotionDraw {
  Pipeline* pipeline;
  // In motion_data_: the vertex shader's float constants of this frame, those of the last frame, then the
  // system, bool/loop and fetch constants.
  size_t data;
  uint32_t constants_size;
  Viewport viewport;
  Rect scissor;
  IBuffer* index_buffer;  // null: not indexed
  uint64_t index_offset;
  VALUE_TYPE index_type;
  uint32_t vertex_count;
  // Its own reprojection from its perspective matrix (D120), when it has one: left to the camera's motion if that is
  // the camera vote's winner.
  bool has_reprojection;
  Mat4d reprojection;
};

DrawPath::DrawPath() = default;
DrawPath::~DrawPath() {
  if (save_thread_.joinable()) save_thread_.join();
  if (resolved_) resolved_->Watch(nullptr);  // before the guest memory mirror goes
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
  // Resolved textures are invalidated when their guest memory changes, which the mirror's watches report (D70).
  resolved_ = &resolved;
  resolved.Watch(memory_.get());
  primitives_ = std::make_unique<Primitives>(registers, memory, *memory_, render_device, context);
  if (!primitives_->Initialize()) {
    REXLOG_ERROR("[native_x_renderer] could not initialize primitive processing");
    return false;
  }
  shaders_ = std::make_unique<ShaderCache>();
  if (!shaders_->Initialize(render_device, scale_, REXCVAR_GET(native_x_renderer_scale_in_constants))) return false;
  guest_textures_ = std::make_unique<GuestTextures>(registers, *memory_, resolved, render_device, context);
  if (!guest_textures_->Initialize()) return false;
  // Frame numbers are the device's: frame N is recorded after N - 1 presents and complete when the device's
  // frame fence reaches N.
  guest_textures_->BeginFrame(device.frames_presented() + 1, device.frames_completed());
  textures_ = std::make_unique<Textures>();
  if (!textures_->Initialize(render_device, resolved, *guest_textures_)) return false;

  system_constants_buffer_ =
      CreateConstantBuffer(render_device, "NativeXRenderer system constants", sizeof(Translator::SystemConstants));
  bool_loop_constants_buffer_ =
      CreateConstantBuffer(render_device, "NativeXRenderer bool/loop constants", kBoolLoopConstantsSize);
  fetch_constants_buffer_ = CreateConstantBuffer(render_device, "NativeXRenderer fetch constants", kFetchConstantsSize);
  motion_constants_buffer_ = CreateConstantBuffer(render_device, "NativeXRenderer motion constants", 16);
  if (!system_constants_buffer_ || !bool_loop_constants_buffer_ || !fetch_constants_buffer_ ||
      !motion_constants_buffer_) {
    return false;
  }

  PipelineBuffers buffers;
  buffers.shared_memory = memory_->view();
  buffers.system_constants = system_constants_buffer_;
  buffers.bool_loop_constants = bool_loop_constants_buffer_;
  buffers.fetch_constants = fetch_constants_buffer_;
  buffers.motion_constants = motion_constants_buffer_;
  pipelines_ = std::make_unique<PipelineCache>();
  pipelines_->Initialize(render_device, buffers, device.pipeline_library());
  if (!storage.empty()) OpenStorage(storage);
  const std::string& alpha_to_coverage = REXCVAR_GET(native_x_renderer_alpha_to_coverage);
  alpha_to_coverage_blend_.msaa = alpha_to_coverage != "dither";
  alpha_to_coverage_blend_.one = alpha_to_coverage == "blend";
  REXLOG_INFO("[native_x_renderer] alpha to coverage: {}",
              alpha_to_coverage_blend_.one    ? "blended by alpha, depth where alpha >= 0.5 (D69)"
              : alpha_to_coverage_blend_.msaa ? "blended by alpha on surfaces with MSAA in the game (D69), the shader's "
                                                "2x2 dither at 1x as on the console (D114)"
                                              : "the shader's 2x2 dither, as at 1x on the console");
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
  cache_info.Desc.Name = "NativeXRenderer pipeline cache";
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
  // Records from a device with D24_UNORM_S8_UINT depth, on one without it: its draws ask for the fallback (D103).
  const bool depth_unorm24 = DepthUnorm24Supported(render_device);
  for (const auto& record : pipelines) {
    Stored stored{shaders_->Find(xenos::ShaderType::kVertex, record.vertex_hash), record.vertex_modification,
                  record.pixel_hash ? shaders_->Find(xenos::ShaderType::kPixel, record.pixel_hash) : nullptr,
                  record.pixel_modification, record.state};
    if (!stored.vertex_shader || (record.pixel_hash && !stored.pixel_shader)) continue;
    if (!depth_unorm24 && stored.state.dsv_format == TEX_FORMAT_D24_UNORM_S8_UINT) {
      stored.state.dsv_format = TEX_FORMAT_D32_FLOAT_S8X24_UINT;
    }
    // Queue the translations now; the workers start on them right away.
    shaders_->GetHostShader(*stored.vertex_shader, stored.vertex_modification, pending);
    if (stored.pixel_shader) shaders_->GetHostShader(*stored.pixel_shader, stored.pixel_modification, pending);
    stored_.push_back(stored);
  }
  shaders_->SetStorage(storage_.get());
  stored_total_ = uint32_t(stored_.size());
  stored_start_ = std::chrono::steady_clock::now();
  REXLOG_INFO("[native_x_renderer] pipeline storage {}: {} shaders, {} translations and {} pipelines from earlier runs; "
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
    REXLOG_INFO("[native_x_renderer] pipeline storage: the {} stored pipelines created in {:.0f} ms after the first draw "
                "(their compilation continues on DiligentCore's threads); {} pipelines with the resource signature, {} "
                "with their own layout (D131)",
                stored_total_, ms, pipelines_->signature_count(), pipelines_->own_layout_count());
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
      REXLOG_INFO("[native_x_renderer] pipeline cache saved: {} KB, {} new pipelines, in {:.0f} ms on its own thread",
                  blob->GetSize() >> 10, created, ms);
    } else {
      save_failed_ = true;
    }
    saving_ = false;
  });
  pipelines_unsaved_ = 0;
}

rg::SpirvShader* DrawPath::FindShader(xenos::ShaderType type, const StateTracker::Program& program,
                                      bool* new_failure) {
  if (new_failure) *new_failure = false;
  ShaderEntry& entry = shader_entries_[(program.hash + uint32_t(type)) & (shader_entries_.size() - 1)];
  if (entry.shader && entry.hash == program.hash && entry.type == uint32_t(type)) return entry.shader;
  rg::SpirvShader* shader =
      shaders_->Get(type, program.ucode.data(), uint32_t(program.ucode.size()), program.hash, new_failure);
  if (shader) entry = {program.hash, uint32_t(type), shader};
  return shader;
}

IShader* DrawPath::FindHostShader(rg::SpirvShader& shader, uint64_t modification, bool& pending) {
  uint64_t key = reinterpret_cast<uintptr_t>(&shader) ^ (modification * 0x9E3779B97F4A7C15ull);
  HostShaderEntry& entry = host_shader_entries_[(key ^ (key >> 29)) & (host_shader_entries_.size() - 1)];
  if (entry.host && entry.shader == &shader && entry.modification == modification) {
    pending = false;
    return entry.host;
  }
  IShader* host = shaders_->GetHostShader(shader, modification, pending);
  if (host) entry = {&shader, modification, host};
  return host;
}

void DrawPath::AddDrawRanges(const StateTracker::Program& vertex_program, const rg::RegisterFile& regs,
                             std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  if (vertex_program.ucode.empty()) return;
  const uint32_t type = uint32_t(xenos::ShaderType::kVertex);
  const ShaderEntry& entry = shader_entries_[(vertex_program.hash + type) & (shader_entries_.size() - 1)];
  rg::SpirvShader* shader = entry.shader && entry.hash == vertex_program.hash && entry.type == type
                                ? entry.shader
                                : shaders_->Find(xenos::ShaderType::kVertex, vertex_program.hash);
  if (!shader) return;
  // RequestRanges refuses the whole request for one range out of guest memory.
  auto add = [&ranges](uint32_t start, uint32_t length) {
    if (length && start < rg::SharedMemory::kBufferSize && length <= rg::SharedMemory::kBufferSize - start) {
      ranges.emplace_back(start, length);
    }
  };
  // Vertices: as DrawInternal requests them.
  const rg::Shader::ConstantRegisterMap& constants = shader->constant_register_map();
  for (uint32_t i = 0; i < std::size(constants.vertex_fetch_bitmap); ++i) {
    uint32_t bits = constants.vertex_fetch_bitmap[i];
    uint32_t bit;
    while (rex::bit_scan_forward(bits, &bit)) {
      bits &= ~(1u << bit);
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(i * 32 + bit);
      if (fetch.type == xenos::FetchConstantType::kVertex) add(fetch.address << 2, fetch.size << 2);
    }
  }
  // Indices from memory: the range the primitive processor requests for them.
  auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (initiator.source_select == xenos::SourceSelect::kDMA && initiator.num_indices) {
    uint32_t size_log2 = initiator.index_size == xenos::IndexFormat::kInt16 ? 1 : 2;
    uint32_t base = regs[rg::XE_GPU_REG_VGT_DMA_BASE] & ~((UINT32_C(1) << size_log2) - 1);
    add(base, uint32_t(initiator.num_indices) << size_log2);
  }
}

void DrawPath::UploadAhead(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint32_t draws) {
  memory_->BeginWindow();
  if (ranges.empty()) return;
  memory_->RequestRanges(ranges.data(), ranges.size());
  ++stats_.ahead_batches;
  stats_.ahead_draws += draws;
}

void DrawPath::EndAheadWindow() {
  if (memory_) memory_->EndWindow();
}

void DrawPath::Draw(const StateTracker::Program& vertex_program, const StateTracker::Program& pixel_program,
                    std::span<const Copy> copies, const std::array<HostTarget*, 5>& targets, bool tiled) {
  ++stats_.draws;
  ScopedRegisters overrides(*registers_);
  rg::RegisterFile& regs = *registers_;
  // Host targets have one sample (D60): the translated shaders (alpha to coverage) and the interpolation
  // are those of a surface without MSAA.
  overrides.Set(rg::XE_GPU_REG_RB_SURFACE_INFO, regs[rg::XE_GPU_REG_RB_SURFACE_INFO] & ~(3u << 16));
  if (tiled) {
    // The tile's window offset and scissor are in bin-predicated packets, which the tracker skips. The
    // host target holds every tile (D60): no offset, no window scissor (the screen scissor still applies).
    overrides.Set(rg::XE_GPU_REG_PA_SC_WINDOW_OFFSET, 0);
    overrides.Set(rg::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, regs[rg::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] & 0x80000000u);
    overrides.Set(rg::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, 0x2000u | (0x2000u << 16));
  }
  if (DrawInternal(vertex_program, pixel_program, copies, targets)) {
    ++stats_.drawn;
  }
}

bool DrawPath::DrawInternal(const StateTracker::Program& vertex_program, const StateTracker::Program& pixel_program,
                            std::span<const Copy> copies, const std::array<HostTarget*, 5>& targets) {
  const rg::RegisterFile& regs = *registers_;
  IDeviceContext* context = device_->context();
  const uint64_t copies_at_start = memory_->copies();  // D98

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) return false;

  // Shaders.
  // A program the SDK cannot analyze: where it came from, once (bring-up diagnostics).
  auto log_bad_program = [](const char* stage, const StateTracker::Program& program) {
    std::string dwords;
    for (size_t i = 0; i < program.ucode.size() && i < 512; ++i) {
      dwords += fmt::format(" {:08X}", rex::byte_swap(program.ucode[i]));
    }
    REXLOG_ERROR("[native_x_renderer] the {} program came from {} at {:#x} (packet {:#x}) in {}:{}", stage,
                 program.immediate ? "IM_LOAD_IMMEDIATE" : "IM_LOAD", program.address, program.packet,
                 CallName(program.loaded_by), dwords);
  };
  if (vertex_program.ucode.empty()) return false;
  bool new_failure = false;
  rg::SpirvShader* vertex_shader = FindShader(xenos::ShaderType::kVertex, vertex_program, &new_failure);
  if (!vertex_shader) {
    if (new_failure) log_bad_program("vertex", vertex_program);
    ++stats_.failed;
    return false;
  }
  if (vertex_shader->memexport_eM_written()) {
    NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] a vertex shader uses memory export: not supported yet");
    return false;
  }
  bool polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, polygonal)) return false;
  rg::SpirvShader* pixel_shader = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth) {
    if (!pixel_program.ucode.empty()) {
      pixel_shader = FindShader(xenos::ShaderType::kPixel, pixel_program, &new_failure);
      if (!pixel_shader) {
        if (new_failure) log_bad_program("pixel", pixel_program);
        ++stats_.failed;
        return false;
      }
      if (pixel_shader->memexport_eM_written()) {
        NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] a pixel shader uses memory export: not supported yet");
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
    NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] primitive processing failed (primitive type {})",
            uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type));
    ++stats_.failed;
    return false;
  }
  static int32_t logged_draws = 0;
  if (logged_draws < REXCVAR_GET(native_x_renderer_draw_log)) {
    ++logged_draws;
    REXLOG_INFO("[native_x_renderer] draw {}: VGT_DRAW_INITIATOR {:08X} DMA base {:08X} size {:08X}; guest {} x{} -> host {} x{}, "
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
      NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] host primitive type {} not supported",
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
  IShader* host_vertex_shader = FindHostShader(*vertex_shader, vertex_modification.value, translating);
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
    host_pixel_shader = FindHostShader(*pixel_shader, pixel_modification.value, pixel_translating);
    if (!host_pixel_shader && !pixel_translating) {
      ++stats_.failed;
      return false;
    }
    translating |= pixel_translating;
  }
  // D101: a draw into an offscreen target waits for its translations (the workers finish them on their own).
  const auto wait_start = std::chrono::steady_clock::now();
  bool waited = false;
  while (translating && wait_for_pipelines_ && std::chrono::steady_clock::now() - wait_start < kPipelineWaitLimit) {
    waited = true;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    translating = false;
    if (!host_vertex_shader) {
      host_vertex_shader = FindHostShader(*vertex_shader, vertex_modification.value, translating);
      if (!host_vertex_shader && !translating) {
        ++stats_.failed;
        return false;
      }
    }
    if (pixel_shader && !host_pixel_shader) {
      bool pixel_translating = false;
      host_pixel_shader = FindHostShader(*pixel_shader, pixel_modification_value, pixel_translating);
      if (!host_pixel_shader && !pixel_translating) {
        ++stats_.failed;
        return false;
      }
      translating |= pixel_translating;
    }
  }
  if (translating) {
    ++stats_.translating;
    if (steady_pass_) ++stats_.steady_skipped;
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
      NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] color and depth targets with different sample counts");
      ++stats_.failed;
      return false;
    }
    samples = depth->samples;
    state.dsv_format = uint16_t(depth->format);
  }
  // The target the draw's size and scale are taken from: the first color target, else depth.
  const HostTarget* sized = depth;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    if (render_target_views[i]) {
      sized = targets[i];
      break;
    }
  }
  if (depth && (depth->reduced != sized->reduced || depth->shadow != sized->shadow || depth->native != sized->native)) {
    NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] color and depth targets at different scales (one reduced, D49, a "
                                "shadow map, D92, or a native reduced copy of the screen, D127)");
    ++stats_.failed;
    return false;
  }
  // A pixel shader that reads its pixel position reverts the scale of this target (D128, FillSystemConstants): the
  // render scale, the reduced one (D49), the shadow maps' (D92) or 1 for a native reduced copy of the screen (D127).
  // With native_x_renderer_scale_in_constants off, the render scale written into the translation, whatever the target.

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
    // The slope is per host pixel, smaller at a higher scale: the factor grows with the target's scale, as on the
    // Xenos path (the larger axis, for more bias rather than less), so the bias stays what the game asked (D92).
    state.slope_scaled_depth_bias =
        slope * xenos::kPolygonOffsetScaleSubpixelUnit * std::max(sized->scale.x, sized->scale.y);
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
  // Alpha to coverage on host targets with one sample (D69). With the game's MSAA it covers a fraction of
  // each pixel's samples equal to the alpha, and the resolve blends them with what is behind; the shader's
  // dither does it in 2x2 pixels instead, which shows as dots. So the dither is off, a target that does not
  // blend blends by the alpha, and a draw that writes depth writes it where the alpha is at least 0.5
  // (FillSystemConstants): mostly covered pixels still hide what is drawn behind them later. A target that
  // blends already (GRAW fades objects with alpha blending and alpha to coverage together) keeps its blend:
  // somewhat more opaque than the game's MSAA where the alpha is between 0 and 1. On a surface the game draws at 1x,
  // the console dithers too, and its objects are solid there: the dither stays (D114; blending made GH Metallica's
  // hair see-through).
  bool guest_msaa = regs.Get<reg::RB_SURFACE_INFO>().msaa_samples != xenos::MsaaSamples::k1X;
  bool alpha_to_coverage_blended = (guest_msaa ? alpha_to_coverage_blend_.msaa : alpha_to_coverage_blend_.one) &&
                                   pixel_shader && regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable;
  if (alpha_to_coverage_blended) {
    for (uint32_t i = 0; i < render_target_count; ++i) {
      PipelineState::Target& target = state.targets[i];
      if (!target.write_mask || target.blend_enable) continue;
      target.blend_enable = 1;
      target.src = target.src_alpha = uint8_t(BLEND_FACTOR_SRC_ALPHA);
      target.dst = target.dst_alpha = uint8_t(BLEND_FACTOR_INV_SRC_ALPHA);
      target.op = target.op_alpha = uint8_t(BLEND_OPERATION_ADD);
    }
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
    NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] pipeline creation failed (more failures are not logged)");
    ++stats_.failed;
    return false;
  }
  // D101: a draw into an offscreen target waits for its pipeline (DiligentCore's threads compile it).
  while (wait_for_pipelines_ && !pipeline->failed && !pipelines_->Ready(*pipeline) &&
         std::chrono::steady_clock::now() - wait_start < kPipelineWaitLimit) {
    waited = true;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  if (waited) {
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
    ++stats_.waited;
    stats_.waited_ms += ms;
    stats_.max_waited_ms = std::max(stats_.max_waited_ms, ms);
    // A wait the player feels (D126): which pass it was.
    if (ms >= kLongPipelineWaitMs) {
      const HostTarget* target = nullptr;
      for (const HostTarget* candidate : targets) {
        if (candidate && !target) target = candidate;
      }
      if (target) {
        REXLOG_INFO("[native_x_renderer] a draw waited {:.0f} ms for its pipeline (D101): target {}x{}{}, drawn on {} frames", ms,
                    target->guest_width, target->guest_height, target->depth ? " depth" : "", target->drawn_frames);
      }
    }
  }
  if (!pipelines_->Ready(*pipeline)) {
    // Still compiling (D28), or failed (logged): the draw is skipped.
    ++(pipeline->failed ? stats_.failed : stats_.compiling);
    if (steady_pass_ && !pipeline->failed) ++stats_.steady_skipped;
    return false;
  }

  // Viewport and scissor. DiligentCore's Vulkan backend flips Y with a negative viewport height, as
  // Direct3D does, so the translated shaders produce bottom-left-origin NDC (the SDK's D3D12 setting).
  // Both are computed in guest pixels and scaled when they are set (D37): the viewport's NDC transform is
  // relative to it, and Vulkan takes a fractional viewport.
  // Float24 depth keeps the guest's values (full_float24_in_0_to_1 false): the SDK halves it to move EDRAM
  // data through depth and doubles it back in its resolves, but here resolves copy host depth as it is,
  // and shaders compare sampled depth with depth they compute (shadow maps).
  // The viewport stays within the device's maxViewportDimensions once scaled (D134): a draw without clipping gets a
  // viewport of 8192 guest pixels, 22016 host pixels at 3440x1440, past AMD's 16384. The bound is given in guest
  // pixels, and the SDK moves what does not fit into the NDC scale and offset, as its own Vulkan path does.
  const uint32_t viewport_max[2] = {
      std::clamp(uint32_t(float(device_->max_viewport_width()) / std::max(sized->scale.x, 1.0f)), 1u, 16384u),
      std::clamp(uint32_t(float(device_->max_viewport_height()) / std::max(sized->scale.y, 1.0f)), 1u, 16384u)};
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(regs, 1, 1, true, viewport_max[0], viewport_max[1], true, depth_control, false, false,
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
  uint32_t first_vertex_address = 0;  // for the object motion's key (D48)
  for (uint32_t i = 0; i < std::size(vertex_constants.vertex_fetch_bitmap); ++i) {
    uint32_t bits = vertex_constants.vertex_fetch_bitmap[i];
    uint32_t bit;
    while (rex::bit_scan_forward(bits, &bit)) {
      bits &= ~(1u << bit);
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(i * 32 + bit);
      if (fetch.type != xenos::FetchConstantType::kVertex) continue;
      if (!first_vertex_address) first_vertex_address = fetch.address;
      memory_->RequestRange(fetch.address << 2, fetch.size << 2);
    }
  }
  // An inline draw's data, if the game reused its memory since the draw was captured (D71).
  for (const Copy& copy : copies) {
    if (memory_->WriteIfChanged(context, copy.address, copy.data, copy.size)) ++stats_.inline_restored;
  }
  if (primitives_->UploadConverted()) ++stats_.copies;
  // This draw's own data or textures copied, ending the render pass: the queued draws' data copied now too (D98).
  if (upload_ahead_ && memory_->copies() != copies_at_start) upload_ahead_();
  stats_.copies += memory_->TakeCopies();
  stats_.uploaded_bytes += memory_->TakeUploadedBytes();
  stats_.unchanged_bytes += memory_->TakeUnchangedBytes();
  stats_.unwatched_hashed_bytes += memory_->TakeUnwatchedHashedBytes();
  memory_->PrepareForRead(context);

  // Constants.
  DrawContext draw{&regs,
                   &primitives,
                   &viewport_info,
                   polygonal,
                   used_texture_mask,
                   alpha_to_coverage_blended,
                   state.depth_enable && state.depth_write,
                   sized->scale};
  FillSystemConstants(draw);
  {
    MappedBuffer map(context, system_constants_buffer_);
    std::memcpy(map.data(), &system_constants_, sizeof(system_constants_));
  }
  const rg::SpirvShader* stage_shaders[2] = {vertex_shader, pixel_shader};
  // The vertex shader's are also kept for the object motion (D48).
  vertex_constants_.clear();
  vertex_registers_.clear();
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const rg::SpirvShader* shader = stage_shaders[stage];
    if (!shader || !shader->constant_register_map().float_count) continue;
    MappedBuffer map(context, pipeline->float_constants[stage]);
    uint8_t* out = map.data();
    const rg::Shader::ConstantRegisterMap& constants = shader->constant_register_map();
    uint32_t first = stage ? rg::XE_GPU_REG_SHADER_CONSTANT_256_X : rg::XE_GPU_REG_SHADER_CONSTANT_000_X;
    bool keep = !stage && (object_motion_on_ || camera_vote_on_);
    // A constant that holds the size of a resolved texture the shader samples, as the game wrote it for the guest
    // texture: its host size instead (D93).
    uint32_t sized_constant = UINT32_MAX;
    float host_size[4];
    for (const Profile::TextureSizeConstant& entry : texture_size_constants_) {
      if (entry.pixel_shader != (stage == 1) || entry.constant >= 256 || entry.fetch_constant >= 32 ||
          !(constants.float_bitmap[entry.constant >> 6] & (uint64_t(1) << (entry.constant & 63))) ||
          !(shader->GetUsedTextureMaskAfterTranslation() & (1u << entry.fetch_constant))) {
        continue;
      }
      const ResolvedTexture* texture = textures_->Resolved(regs.GetTextureFetch(entry.fetch_constant));
      if (!texture || (texture->width == texture->guest_width && texture->height == texture->guest_height)) continue;
      float game[4];
      std::memcpy(game, &regs[first + (entry.constant << 2)], sizeof(game));
      auto equal = [](float a, float b) { return std::abs(a - b) <= std::abs(b) * 1e-5f; };
      float guest_width = float(texture->guest_width), guest_height = float(texture->guest_height);
      if (!equal(game[0], guest_width) || !equal(game[1], guest_height) || !equal(game[2], 1.0f / guest_width) ||
          !equal(game[3], 1.0f / guest_height)) {
        continue;
      }
      sized_constant = entry.constant;
      host_size[0] = float(texture->width);
      host_size[1] = float(texture->height);
      host_size[2] = 1.0f / host_size[0];
      host_size[3] = 1.0f / host_size[1];
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_INFO("[native_x_renderer] the {} shader's c{} holds the size of the texture at fetch constant {}: its "
                    "host size {}x{} instead of the game's {}x{} (D93)",
                    stage ? "pixel" : "vertex", entry.constant, entry.fetch_constant, texture->width, texture->height,
                    texture->guest_width, texture->guest_height);
      }
      break;
    }
    // A run of consecutive constants at a time (the register file holds them in order): a shader's constants are
    // mostly whole ranges (matrices, bones, lights), and one copy per constant was a tenth of the renderer thread's
    // time in Kameo (D100).
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = constants.float_bitmap[i];
      while (bits) {
        uint32_t start = uint32_t(std::countr_zero(bits));
        uint32_t count = uint32_t(std::countr_zero(~(bits >> start)));
        bits &= count >= 64 ? 0 : ~(((uint64_t(1) << count) - 1) << start);
        uint32_t first_register = (i << 6) + start;
        size_t bytes = size_t(count) * 16;
        std::memcpy(out, &regs[first + (first_register << 2)], bytes);
        if (sized_constant >= first_register && sized_constant < first_register + count) {
          std::memcpy(out + size_t(sized_constant - first_register) * 16, host_size, 16);
        }
        if (keep) {
          // From the register file, not from `out`: the mapped buffer is write-combined memory, slow to read back
          // (with DLAA in a GH Metallica song, a sixth of the renderer thread's time, D120).
          size_t kept = vertex_constants_.size();
          const auto* source = reinterpret_cast<const uint8_t*>(&regs[first + (first_register << 2)]);
          vertex_constants_.insert(vertex_constants_.end(), source, source + bytes);
          if (sized_constant >= first_register && sized_constant < first_register + count) {
            std::memcpy(vertex_constants_.data() + kept + size_t(sized_constant - first_register) * 16, host_size, 16);
          }
          for (uint32_t k = 0; k < count; ++k) vertex_registers_.push_back(uint8_t(first_register + k));
        }
        out += bytes;
      }
    }
  }
  // The bool/loop constants: uploaded when they differ from the last upload of this frame (D81); they rarely
  // change. The fetch constants change with nearly every draw (its textures): compared, they cost more than the
  // upload they would save.
  uint64_t frame = device_->frames_presented();
  const auto* bool_loop = reinterpret_cast<const uint8_t*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031]);
  if (uploaded_frame_ != frame || std::memcmp(uploaded_bool_loop_.data(), bool_loop, kBoolLoopConstantsSize)) {
    MappedBuffer map(context, bool_loop_constants_buffer_);
    std::memcpy(map.data(), bool_loop, kBoolLoopConstantsSize);
    std::memcpy(uploaded_bool_loop_.data(), bool_loop, kBoolLoopConstantsSize);
    uploaded_frame_ = frame;
  }
  {
    MappedBuffer map(context, fetch_constants_buffer_);
    std::memcpy(map.data(), &regs[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
  }

  // Textures and samplers. Drawn at the reduced scale (D49), the samplers take the mip bias for it: log2 of
  // the reduced scale over the render scale (NVIDIA's DLSS guide), for the detail the upscaler brings back.
  float lod_bias = sized->reduced ? std::log2(sized->scale.x / scale_.x) : 0.0f;
  // Each variable set only when its object changes (D82): in Kameo's first level 2 % of them do. With binding sets
  // (D111) the objects are listed instead, per stage the textures then the samplers, and the set for them found.
  bool bindings_changed = false;
  const bool binding_sets = pipeline->binding_sets_on;
  if (binding_sets) binding_objects_.clear();
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const rg::SpirvShader* shader = stage_shaders[stage];
    if (!shader) continue;
    const auto& texture_bindings = shader->GetTextureBindingsAfterTranslation();
    for (size_t i = 0; i < texture_bindings.size() && i < pipeline->textures[stage].size(); ++i) {
      if (binding_sets) {
        IDeviceObject* view = nullptr;
        if (pipeline->textures[stage][i]) {
          const auto& binding = texture_bindings[i];
          view = textures_->View(binding.fetch_constant, regs.GetTextureFetch(binding.fetch_constant),
                                 binding.dimension, binding.is_signed);
        }
        binding_objects_.push_back(view);
        continue;
      }
      if (IShaderResourceVariable* variable = pipeline->textures[stage][i]) {
        const auto& binding = texture_bindings[i];
        IDeviceObject* view = textures_->View(binding.fetch_constant, regs.GetTextureFetch(binding.fetch_constant),
                                              binding.dimension, binding.is_signed);
        if (pipeline->texture_objects[stage][i] != view) {
          variable->Set(view);
          pipeline->texture_objects[stage][i] = view;
          ++stats_.bindings_set;
          bindings_changed = true;
        }
        pipeline->textures_set = true;
      }
    }
    const auto& sampler_bindings = shader->GetSamplerBindingsAfterTranslation();
    for (size_t i = 0; i < sampler_bindings.size() && i < pipeline->samplers[stage].size(); ++i) {
      if (binding_sets) {
        binding_objects_.push_back(
            pipeline->samplers[stage][i]
                ? textures_->Sampler(regs.GetTextureFetch(sampler_bindings[i].fetch_constant), sampler_bindings[i],
                                     lod_bias)
                : nullptr);
        continue;
      }
      if (IShaderResourceVariable* variable = pipeline->samplers[stage][i]) {
        IDeviceObject* sampler =
            textures_->Sampler(regs.GetTextureFetch(sampler_bindings[i].fetch_constant), sampler_bindings[i], lod_bias);
        if (pipeline->sampler_objects[stage][i] != sampler) {
          variable->Set(sampler);
          pipeline->sampler_objects[stage][i] = sampler;
          ++stats_.bindings_set;
          bindings_changed = true;
        }
      }
    }
  }

  // D111: the pipeline's binding set for these objects, created on first use.
  BindingSet* set = nullptr;
  if (binding_sets) {
    bool created = false;
    set = pipelines_->FindSet(*pipeline, binding_objects_, device_->frames_presented(), &created);
    if (!set) {
      ++stats_.failed;
      return false;
    }
    if (created) ++stats_.binding_sets_created;
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
    viewport_valid_ = false;  // DiligentCore sets the viewport to the targets' size (D81)
    committed_ = nullptr;     // and the shader resources are committed again, transitioning what was a target (D82)
  }
  context->SetPipelineState(pipeline->pso);
  // The shader resources are committed when something changed since the last draw committed them (D82): another
  // pipeline, a texture or sampler, the render targets, or a texture no longer readable (loaded, copied or drawn
  // into since). DiligentCore allocates and writes a descriptor set for the textures and samplers on every
  // commit; in Kameo's first level three draws in four have the last draw's pipeline and resources.
  // The commit transitions only when a texture needs it (D131): the guest memory is made readable before the draw
  // (GuestMemory::PrepareForRead) and the constant buffers are dynamic, so with every texture readable DiligentCore's
  // walk over the binding's resources, every slot of the resource signature, finds nothing to do.
  if (set) {
    // D111: the set's binding, its variables written when it was created; committed when it is not the last
    // committed one, or one of its textures is no longer readable.
    const bool readable = TexturesReadable(*set);
    if (committed_ != set || !readable) {
      context->CommitShaderResources(
          set->srb, readable ? RESOURCE_STATE_TRANSITION_MODE_NONE : RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      committed_ = set;
      ++stats_.committed;
    }
  } else {
    const bool readable = TexturesReadable(*pipeline);
    if (bindings_changed || committed_ != pipeline || !readable) {
      context->CommitShaderResources(
          pipeline->srb, readable ? RESOURCE_STATE_TRANSITION_MODE_NONE : RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      committed_ = pipeline;
      ++stats_.committed;
    }
  }
  pipeline->last_drawn = device_->frames_presented();
  // Guest pixels to host pixels: the target's scale per axis (the render scale, D37, D38; the reduced one,
  // D49); in a 2D layer kept at the game's aspect ratio (D43), the smaller of the two on both axes, and an
  // offset that centers the target's guest area in its host size.
  const RenderScale& target_scale = sized->scale;
  float scale_x = target_scale.x, scale_y = target_scale.y, offset_x = 0.0f, offset_y = 0.0f;
  if (aspect_box_ && target_scale.x != target_scale.y) {
    float uniform = std::min(target_scale.x, target_scale.y);
    offset_x = float(sized->guest_width) * (target_scale.x - uniform) * 0.5f;
    offset_y = float(sized->guest_height) * (target_scale.y - uniform) * 0.5f;
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
  // The temporal antialiasing's jitter (D47): the main 3D view's image is shifted by a sub-pixel offset,
  // different every frame. The scissor stays. A view found from the draws (D119) starts at its first depth-writing
  // draw of that size with a perspective matrix.
  bool output_sized = sized->guest_width == jitter_size_[0] && sized->guest_height == jitter_size_[1];
  if (waiting_for_scene_ && !jitter_on_ && output_sized && state.depth_write && !vertex_registers_.empty()) {
    uint32_t matrix = 0;
    bool transposed = false;
    if (CameraVote::FindMatrix(vertex_constants_.data(), vertex_registers_.data(), uint32_t(vertex_registers_.size()),
                               matrix, transposed)) {
      jitter_on_ = jitter_scene_;
      waiting_for_scene_ = false;
      scene_started_ = true;
      scene_start_matrix_ = CameraVote::MatrixAt(vertex_constants_.data(), matrix, transposed);
    }
  }
  bool jittered = jitter_on_ && output_sized;
  if (jittered) {
    viewport.TopLeftX += jitter_[0];
    viewport.TopLeftY += jitter_[1];
    jittered_z_[0] = viewport_info.z_min;
    jittered_z_[1] = viewport_info.z_max;
  }
  Rect scissor_rect{host_x(scissor.offset[0]), host_y(scissor.offset[1]),
                    host_x(int64_t(scissor.offset[0]) + scissor.extent[0]),
                    host_y(int64_t(scissor.offset[1]) + scissor.extent[1])};
  // Set when they change (D81): DiligentCore records them on every call.
  if (!viewport_valid_ || std::memcmp(&viewport, &viewport_, sizeof(viewport))) {
    context->SetViewports(1, &viewport, 0, 0);
    viewport_ = viewport;
  }
  if (!viewport_valid_ || scissor_rect != scissor_) {
    context->SetScissorRects(1, &scissor_rect, 0, 0);
    scissor_ = scissor_rect;
  }
  viewport_valid_ = true;
  if (state.stencil_enable) context->SetStencilRef(stencil_reference);
  const float blend_factors[4] = {regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_GREEN),
                                  regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_ALPHA)};
  context->SetBlendFactors(blend_factors);

  IBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  VALUE_TYPE index_type = primitives.host_index_format == xenos::IndexFormat::kInt16 ? VT_UINT16 : VT_UINT32;
  switch (primitives.index_buffer_type) {
    case rg::PrimitiveProcessor::ProcessedIndexBufferType::kNone:
      break;
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
  if (index_buffer) {
    context->SetIndexBuffer(index_buffer, index_offset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawIndexedAttribs attribs{primitives.host_draw_vertex_count, index_type, DRAW_FLAG_NONE};
    context->DrawIndexed(attribs);
  } else {
    DrawAttribs attribs{primitives.host_draw_vertex_count, DRAW_FLAG_NONE};
    context->Draw(attribs);
  }

  // Object motion (D48): the main view's draws that write depth, matched with the last frame's; those that
  // moved are kept to draw their motion. A vertex shader that samples textures is left to the camera's
  // motion (the motion pipeline binds none). The camera vote (D118): each matched draw with a perspective matrix in
  // its constants offers this frame's and the last frame's.
  if (jittered && (object_motion_on_ || camera_vote_on_) && state.depth_write && !vertex_registers_.empty()) {
    struct {
      uint64_t vertex_shader;
      uint32_t vertex_address, index_address, vertex_count, index_type;
    } key{vertex_shader->ucode_data_hash(), first_vertex_address, regs[rg::XE_GPU_REG_VGT_DMA_BASE],
          primitives.guest_draw_vertex_count, uint32_t(primitives.index_buffer_type)};
    const uint8_t* matched = nullptr;
    uint32_t count = uint32_t(vertex_registers_.size());
    const uint8_t* previous = object_motion_.Match(XXH3_64bits(&key, sizeof(key)), vertex_constants_.data(),
                                                   vertex_registers_.data(), count, &matched);
    uint32_t matrix = 0;
    bool transposed = false;
    const Mat4d* reprojection = nullptr;
    if (camera_vote_on_ && matched &&
        CameraVote::FindMatrix(vertex_constants_.data(), vertex_registers_.data(), count, matrix, transposed)) {
      reprojection = camera_vote_.Offer(CameraVote::MatrixAt(vertex_constants_.data(), matrix, transposed),
                                        CameraVote::MatrixAt(matched, matrix, transposed));
    }
    if (object_motion_on_ && previous && vertex_shader->GetTextureBindingsAfterTranslation().empty()) {
      // D120: a draw that moved with the camera only (each object's own world-view-projection changes with the camera:
      // GH Metallica's) needs no motion draw: its constants changed only in its matrix (and the view's shared ones, not
      // in bones), and its matrix's reprojection is the camera's. Against the vote's leader now; the others against its
      // winner when drawn.
      if (reprojection &&
          !object_motion_.ChangedOnlyIn(vertex_constants_.data(), previous, matrix, matrix + 4)) {
        reprojection = nullptr;
      }
      Mat4d leader;
      uint32_t votes = 0, offers = 0;
      if (reprojection && camera_vote_.Result(leader, votes, offers) && votes >= kLeaderVotes && votes * 2 >= offers &&
          CameraVote::WithinPx(*reprojection, leader, jitter_size_[0], jitter_size_[1], kCameraMotionPx)) {
        ++stats_.motion_camera;
      } else {
        KeepMotionDraw(*vertex_shader, vertex_modification.value, state, previous, viewport, scissor_rect,
                       index_buffer, index_offset, index_type, primitives.host_draw_vertex_count, reprojection);
      }
    }
  }
  return true;
}

void DrawPath::KeepMotionDraw(rg::SpirvShader& vertex_shader, uint64_t vertex_modification, const PipelineState& state,
                              const uint8_t* previous_constants, const Viewport& viewport, const Rect& scissor,
                              IBuffer* index_buffer, uint64_t index_offset, VALUE_TYPE index_type,
                              uint32_t vertex_count, const Mat4d* reprojection) {
  if (!motion_pixel_shader_) {
    if (motion_pixel_shader_failed_) return;
    motion_pixel_shader_ = CreateMotionPixelShader(device_->device());
    if (!motion_pixel_shader_) {
      motion_pixel_shader_failed_ = true;
      REXLOG_ERROR("[native_x_renderer] object motion: the motion pixel shader could not be created");
      return;
    }
  }
  // Made on a worker thread once the draw's own vertex shader is ready; until then, and for a shader it
  // cannot be made for, the draw keeps the camera's motion.
  bool pending = false;
  IShader* motion_vertex_shader = shaders_->GetMotionShader(vertex_shader, vertex_modification, pending);
  if (!motion_vertex_shader) return;
  // The draw's state, into the motion vectors: depth tested equal to what the draw wrote, not written; no
  // stencil, no blending.
  PipelineState motion = state;
  motion.vertex_shader = motion_vertex_shader;
  motion.pixel_shader = motion_pixel_shader_.RawPtr();
  std::fill(std::begin(motion.rtv_formats), std::end(motion.rtv_formats), uint16_t(0));
  motion.rtv_formats[0] = uint16_t(kMotionVectorFormat);
  motion.render_target_count = 1;
  motion.depth_write = 0;
  motion.depth_func = uint8_t(COMPARISON_FUNC_EQUAL);
  motion.stencil_enable = 0;
  motion.stencil_read_mask = motion.stencil_write_mask = 0;
  std::memset(motion.stencil_front, 0, sizeof(motion.stencil_front));
  std::memset(motion.stencil_back, 0, sizeof(motion.stencil_back));
  std::memset(motion.targets, 0, sizeof(motion.targets));
  motion.targets[0].write_mask = uint8_t(COLOR_MASK_RED | COLOR_MASK_GREEN);
  bool created = false;
  Pipeline* pipeline = pipelines_->Get(motion, vertex_shader, nullptr, &created, true);
  if (created) {
    // In DiligentCore's pipeline cache with the others (D42); not in the stored records.
    ++pipelines_unsaved_;
    frames_since_created_ = 0;
  }
  if (!pipeline || !pipelines_->Ready(*pipeline)) return;
  MotionDraw draw{pipeline, motion_data_.size(), uint32_t(vertex_constants_.size()), viewport, scissor,
                  index_buffer, index_offset, index_type, vertex_count, reprojection != nullptr,
                  reprojection ? *reprojection : Mat4d{}};
  const rg::RegisterFile& regs = *registers_;
  const auto* system = reinterpret_cast<const uint8_t*>(&system_constants_);
  const auto* bool_loop = reinterpret_cast<const uint8_t*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031]);
  const auto* fetch = reinterpret_cast<const uint8_t*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0]);
  motion_data_.insert(motion_data_.end(), vertex_constants_.begin(), vertex_constants_.end());
  motion_data_.insert(motion_data_.end(), previous_constants, previous_constants + vertex_constants_.size());
  motion_data_.insert(motion_data_.end(), system, system + sizeof(system_constants_));
  motion_data_.insert(motion_data_.end(), bool_loop, bool_loop + kBoolLoopConstantsSize);
  motion_data_.insert(motion_data_.end(), fetch, fetch + kFetchConstantsSize);
  motion_draws_.push_back(draw);
}

uint32_t DrawPath::DrawObjectMotion(ITextureView* motion, ITextureView* depth) {
  uint32_t drawn = 0;
  if (motion && depth && !motion_draws_.empty()) {
    IDeviceContext* context = device_->context();
    context->SetRenderTargets(1, &motion, depth, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    TargetsChanged();
    uploaded_frame_ = UINT64_MAX;  // its own bool/loop and fetch constants go into the draws' buffers (D81)
    memory_->PrepareForRead(context);
    for (const MotionDraw& draw : motion_draws_) {
      // D120: it moved with the camera only, the vote's winner.
      if (draw.has_reprojection && camera_winner_valid_ &&
          CameraVote::WithinPx(draw.reprojection, camera_winner_, jitter_size_[0], jitter_size_[1], kCameraMotionPx)) {
        ++stats_.motion_camera;
        continue;
      }
      const uint8_t* data = motion_data_.data() + draw.data;
      Pipeline& pipeline = *draw.pipeline;
      auto upload = [&](IBuffer* buffer, size_t size) {
        MappedBuffer map(context, buffer);
        std::memcpy(map.data(), data, size);
        data += size;
      };
      upload(pipeline.float_constants[0], draw.constants_size);
      upload(pipeline.previous_float_constants, draw.constants_size);
      upload(system_constants_buffer_, sizeof(system_constants_));
      upload(bool_loop_constants_buffer_, kBoolLoopConstantsSize);
      upload(fetch_constants_buffer_, kFetchConstantsSize);
      {
        const float ndc_to_pixels[4] = {0.5f * draw.viewport.Width, -0.5f * draw.viewport.Height, 0.0f, 0.0f};
        MappedBuffer map(context, motion_constants_buffer_);
        std::memcpy(map.data(), ndc_to_pixels, sizeof(ndc_to_pixels));
      }
      context->SetPipelineState(pipeline.pso);
      context->CommitShaderResources(pipeline.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      context->SetViewports(1, &draw.viewport, 0, 0);
      context->SetScissorRects(1, &draw.scissor, 0, 0);
      if (draw.index_buffer) {
        context->SetIndexBuffer(draw.index_buffer, draw.index_offset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        DrawIndexedAttribs attribs{draw.vertex_count, draw.index_type, DRAW_FLAG_NONE};
        context->DrawIndexed(attribs);
      } else {
        DrawAttribs attribs{draw.vertex_count, DRAW_FLAG_NONE};
        context->Draw(attribs);
      }
      ++drawn;
    }
  }
  stats_.motion_drawn += drawn;
  motion_draws_.clear();
  motion_data_.clear();
  return drawn;
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
  float alpha_reference = regs.Get<float>(rg::XE_GPU_REG_RB_ALPHA_REF);
  if (draw.alpha_to_coverage_blended && draw.depth_write) {
    // D69: depth where the alpha is at least 0.5, or where the game's own test is stricter.
    if (alpha_test == xenos::CompareFunction::kAlways) {
      alpha_test = xenos::CompareFunction::kGreaterEqual;
      alpha_reference = 0.5f;
    } else if (alpha_test == xenos::CompareFunction::kGreater || alpha_test == xenos::CompareFunction::kGreaterEqual) {
      alpha_reference = std::max(alpha_reference, 0.5f);
    }
  }
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
  // Resolve results are at the render scale: the shaders revert it in texel offsets and weights (D37). Not the
  // native ones, at scale 1 (D127).
  c.textures_resolution_scaled = 0;
  if (scale_.scaled()) {
    uint32_t used = draw.used_texture_mask;
    while (rex::bit_scan_forward(used, &index)) {
      used &= ~(1u << index);
      if (textures_->IsResolutionScaled(index, regs.GetTextureFetch(index))) {
        c.textures_resolution_scaled |= 1u << index;
      }
    }
  }
  // The scales the shaders revert, constants of the draw rather than of the translation (D128): the textures marked
  // above are at the render scale; the pixel position is the draw's own target's. The reciprocals as the
  // translator computed them when the scale was a constant, so a draw at the render scale computes what it did.
  c.texture_resolution_scale[0] = scale_.x;
  c.texture_resolution_scale[1] = scale_.y;
  c.texture_resolution_scale[2] = 1.0f / scale_.x;
  c.texture_resolution_scale[3] = 1.0f / scale_.y;
  c.pixel_position_scale_reciprocal[0] = 1.0f / draw.target_scale.x;
  c.pixel_position_scale_reciprocal[1] = 1.0f / draw.target_scale.y;

  c.alpha_test_reference = alpha_reference;
  c.alpha_to_mask = colorcontrol.alpha_to_mask_enable && !draw.alpha_to_coverage_blended
                        ? (colorcontrol.value >> 24) | (1u << 8)
                        : 0;

  // Color exponent bias. 16-bit fixed-point targets are SNORM here, truncated to -1...1: no extra bias.
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t bias = color_infos[i].color_exp_bias;
    uint32_t bits = 0x3F800000u + (uint32_t(bias) << 23);
    std::memcpy(&c.color_exp_bias[i], &bits, sizeof(bits));
  }
}

uint32_t DrawPath::TakePackedRead() { return textures_->TakePackedRead(); }

void DrawPath::EndFrame() {
  // The stored pipelines (D42): created as their shaders get ready; new records written; the pipeline
  // cache saved, on a thread of its own (D45), once nothing new was compiled for 10 s (at 60 FPS), and at
  // most every 5 minutes (D52): new areas bring pipelines every few seconds, and each save writes tens of
  // MB. What is not saved yet is in the records, compiled again at the next start.
  PrepareStored();
  if (storage_) storage_->Flush();
  if (save_failed_.exchange(false)) {
    pipelines_unsaved_ = std::max(pipelines_unsaved_, 1u);  // try again in 600 frames
    frames_since_created_ = 0;
    frames_since_saved_ = UINT32_MAX / 2;
  }
  ++frames_since_saved_;
  if (pipelines_unsaved_ && ++frames_since_created_ >= 600 && frames_since_saved_ >= 18000 && stored_.empty()) {
    SavePipelineCache();
    frames_since_saved_ = 0;
  }
  if (primitives_) primitives_->EndFrame();
  if (memory_) memory_->EndFrame();
  if (guest_textures_) guest_textures_->BeginFrame(device_->frames_presented() + 1, device_->frames_completed());
  if (textures_) textures_->BeginFrame();
  // Object motion (D48): what DLAA did not draw (no main view this frame) is dropped.
  object_motion_.EndFrame();
  motion_draws_.clear();
  motion_data_.clear();
  // Every 600 frames, the pipelines not drawn in the last 600 let go of their textures (D55), now that the
  // motion draws, which commit the bindings of the frame's draws again, are done.
  uint64_t frame = device_->frames_presented();
  // D111: a game making new texture views all the time would make new binding sets as fast, each holding its
  // textures: past 16384 sets, those not used in the last 8 frames are dropped at once (the frames in flight
  // release their descriptor sets in DiligentCore).
  if (pipelines_ && pipelines_->binding_set_count() > 16384) {
    uint32_t dropped = pipelines_->ReleaseSets(frame, 8);
    committed_ = nullptr;
    if (dropped && !many_sets_logged_) {
      many_sets_logged_ = true;
      REXLOG_WARN("[native_x_renderer] frame {}: over 16384 binding sets kept; {} not used in 8 frames dropped (D111, "
                  "logged once)",
                  frame, dropped);
    }
  }
  if (pipelines_ && frame % 600 == 0) {
    PipelineCache::Released released = pipelines_->ReleaseTextures(frame, 600);
    if (released.sets) committed_ = nullptr;  // the last committed set may be gone (D111)
    if (released.pipelines || released.sets) {
      REXLOG_INFO("[native_x_renderer] frame {}: {} pipelines not drawn in 600 frames let go of their textures, {} "
                  "binding sets not used in 600 frames dropped (D111); {} textures only they held freed ({} MB)",
                  frame, released.pipelines, released.sets, released.textures, released.bytes >> 20);
    }
  }
}

DrawPath::Stats DrawPath::TakeStats() {
  Stats stats = stats_;
  stats_ = {};
  if (memory_) {
    stats.unwatched_pages = memory_->unwatched_pages();
    stats.unwatched_copy_bytes = memory_->copy_committed_bytes();
  }
  if (pipelines_) {
    stats.bindings_set += pipelines_->TakeSetVariables();
    stats.binding_sets_kept = pipelines_->binding_set_count();
  }
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
  ObjectMotion::Stats motion = object_motion_.TakeStats();
  stats.motion_matched = motion.matched;
  stats.motion_moved = motion.moved;
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

}  // namespace native_x_renderer
