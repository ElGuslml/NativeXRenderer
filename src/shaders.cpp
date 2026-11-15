// NativeXRenderer - see shaders.h.

#include "shaders.h"

#include "device.h"
#include "pipelines.h"
#include "spirv_patch.h"
#include "storage.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#include <xxhash.h>

REXCVAR_DEFINE_BOOL(native_x_renderer_specialize, true, "NativeXRenderer",
                    "Pixel shaders specialized to what each draw sets per texture and per state (the textures' signs "
                    "and swizzles, the textures at the render scale, the flags), compiled behind the generic ones and "
                    "drawn with once ready (read at startup, D146). False: the generic shaders only, as before");

namespace native_x_renderer {

using namespace Diligent;
namespace rg = rex::graphics;

namespace {

// The SDK's analysis throws on some invalid microcode and dereferences null on other (a texture fetch
// whose filter has no name): the access violation becomes a failure here, like the exceptions (which
// the filter lets through to the caller's catch).
bool AnalyzeProtected(rg::Shader& shader, rex::string::StringBuffer& disassembly) {
#ifdef _WIN32
  __try {
    shader.AnalyzeUcode(disassembly);
    return true;
  } __except (_exception_code() == 0xC0000005u ? 1 /* EXCEPTION_EXECUTE_HANDLER */
                                                : 0 /* EXCEPTION_CONTINUE_SEARCH */) {
    return false;
  }
#else
  shader.AnalyzeUcode(disassembly);
  return true;
#endif
}

}  // namespace

ShaderCache::ShaderCache() = default;

ShaderCache::~ShaderCache() {
  shutdown_.store(true, std::memory_order_release);
  jobs_cv_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
}

bool ShaderCache::Initialize(IRenderDevice* device, const RenderScale& scale, bool scale_in_constants,
                             unsigned worker_count) {
  device_ = device;
  scale_ = scale;
  scale_in_constants_ = scale_in_constants;
  // What the device created by device.cpp has. DiligentCore enables neither clip and cull distances nor
  // sample rate shading, and NativeXRenderer sets texture swizzles in the shader (system constants) rather
  // than in image views. Memory export (vertex and pixel stores) is not used by POTCAWE; off for now.
  features_ = rg::SpirvShaderTranslator::Features(false);
  const auto& info = device->GetDeviceInfo();
  if (info.APIVersion >= Version{1, 2}) {
    features_.spirv_version = spv::Spv_1_5;
  } else if (info.APIVersion >= Version{1, 1}) {
    features_.spirv_version = spv::Spv_1_3;
  }
  features_.max_storage_buffer_range = UINT32_MAX;  // one binding for the whole 512 MB
  features_.full_draw_index_uint32 = true;
  features_.image_view_format_swizzle = false;
  // Alpha to coverage on targets with one sample: the translator applies its 2x2 dither through the shader's sample
  // mask, which it writes only with sample rate shading (D138).
  features_.sample_rate_shading = SampleRateShadingEnabled(device);
  REXLOG_INFO("[native_x_renderer] sample rate shading {}: alpha to coverage on 1x targets {}",
              features_.sample_rate_shading ? "on" : "off",
              features_.sample_rate_shading ? "dithers in 2x2 as on the console (D138)" : "is off");
  translator_ = std::make_unique<rg::SpirvShaderTranslator>(features_, true, true, false);
  ConfigureScale(*translator_);
  // D146: needs the textures named by their place, so that a specialized pipeline shares its generic one's resource
  // signature and binding sets.
  specialize_ = REXCVAR_GET(native_x_renderer_specialize) && ResourceSignatureOn();
  REXLOG_INFO("[native_x_renderer] pixel shaders specialized to each draw's texture signs, swizzles and flags (D146): {}",
              specialize_ ? "on" : "off");
  // Two workers: a level load translates about a hundred shaders at up to 2.7 ms each, and the game's
  // threads and DiligentCore's four pipeline compilation threads (D28) need the cores at the same time.
  if (!worker_count) worker_count = std::thread::hardware_concurrency() >= 4 ? 2 : 1;
  for (unsigned i = 0; i < worker_count; ++i) workers_.emplace_back([this] { Worker(); });
  return true;
}

void ShaderCache::ConfigureScale(rg::SpirvShaderTranslator& translator) const {
  if (scale_in_constants_) {
    // Read from the system constants (the SDK's recomp change 73, D128): the translation is the same at every scale.
    translator.SetDynamicResolutionScale(true);
  } else {
    // Written into the translation, where the shaders read pixel positions and resolved textures (change 30).
    translator.SetFractionalResolutionScale(scale_.x, scale_.y);
  }
}

ShaderCache::Times ShaderCache::TakeTimes() {
  std::lock_guard<std::mutex> lock(times_mutex_);
  Times times = times_;
  times_ = {};
  return times;
}

rg::SpirvShader* ShaderCache::Get(rg::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords,
                                  bool* new_failure) {
  return Get(type, ucode, dwords, XXH3_64bits(ucode, size_t(dwords) * sizeof(uint32_t)), new_failure);
}

rg::SpirvShader* ShaderCache::Get(rg::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords, uint64_t hash,
                                  bool* new_failure) {
  if (new_failure) *new_failure = false;
  auto key = std::make_pair(hash, uint32_t(type));
  auto it = shaders_.find(key);
  if (it != shaders_.end()) return it->second.get();
  auto start = std::chrono::steady_clock::now();
  auto shader = std::make_unique<rg::SpirvShader>(type, hash, ucode, dwords);
  std::string error;
  try {
    if (!AnalyzeProtected(*shader, disassembly_buffer_)) error = "access violation";
  } catch (const std::exception& e) {
    // The SDK's disassembly throws on what it cannot name (an unknown vertex format, for one).
    error = e.what();
  }
  if (!error.empty()) {
    REXLOG_ERROR("[native_x_renderer] {} shader {:016X} ({} dwords): analysis failed: {}",
                 type == rg::xenos::ShaderType::kVertex ? "vertex" : "pixel", hash, dwords, error);
    disassembly_buffer_.Reset();
    shader.reset();
    if (new_failure) *new_failure = true;
  }
  {
    std::lock_guard<std::mutex> lock(times_mutex_);
    times_.analysis.Add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  }
  if (shader && storage_) storage_->AddShader(uint8_t(type), hash, ucode, dwords);
  return shaders_.emplace(key, std::move(shader)).first->second.get();
}

rg::SpirvShader* ShaderCache::Find(rg::xenos::ShaderType type, uint64_t hash) const {
  auto it = shaders_.find(std::make_pair(hash, uint32_t(type)));
  return it != shaders_.end() ? it->second.get() : nullptr;
}

IShader* ShaderCache::GetHostShader(rg::SpirvShader& shader, uint64_t modification, bool& pending) {
  rg::Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
  auto it = host_shaders_.find(translation);
  if (it == host_shaders_.end()) {
    // New: to a worker. The translation's shader is analyzed and stays as it is; the queue's lock makes
    // the analysis visible to the worker.
    it = host_shaders_.emplace(translation, std::make_unique<HostShader>()).first;
    if (storage_) storage_->AddTranslation(uint8_t(shader.type()), shader.ucode_data_hash(), modification);
    Queue({translation, it->second.get(), JobKind::kTranslate});
  }
  uint32_t state = it->second->state.load(std::memory_order_acquire);
  pending = state == kPending;
  return state == kReady ? it->second->shader.RawPtr() : nullptr;
}

void ShaderCache::Queue(Job job) {
  {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    jobs_.push_back(std::move(job));
  }
  jobs_cv_.notify_one();
}

const std::vector<std::pair<uint32_t, uint32_t>>& ShaderCache::SpecializedRanges() {
  using Constants = rg::SpirvShaderTranslator::SystemConstants;
  static const std::vector<std::pair<uint32_t, uint32_t>> ranges = {
      {uint32_t(offsetof(Constants, flags)), uint32_t(offsetof(Constants, flags) + sizeof(Constants::flags))},
      {uint32_t(offsetof(Constants, textures_resolution_scaled)),
       uint32_t(offsetof(Constants, textures_resolution_scaled) + sizeof(Constants::textures_resolution_scaled))},
      {uint32_t(offsetof(Constants, texture_swizzled_signs)),
       uint32_t(offsetof(Constants, texture_swizzled_signs) + sizeof(Constants::texture_swizzled_signs))},
      {uint32_t(offsetof(Constants, texture_swizzles)),
       uint32_t(offsetof(Constants, texture_swizzles) + sizeof(Constants::texture_swizzles))},
  };
  return ranges;
}

const std::vector<uint32_t>* ShaderCache::SpecializableWords(rg::SpirvShader& shader, uint64_t modification) {
  auto it = host_shaders_.find(shader.GetOrCreateTranslation(modification));
  if (it == host_shaders_.end() || it->second->state.load(std::memory_order_acquire) != kReady) return nullptr;
  return &it->second->specializable;
}

IShader* ShaderCache::GetSpecializedShader(rg::SpirvShader& shader, uint64_t modification, uint64_t key,
                                           const uint32_t* values, bool& pending) {
  pending = false;
  rg::Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
  auto base = host_shaders_.find(translation);
  if (base == host_shaders_.end() || base->second->state.load(std::memory_order_acquire) != kReady ||
      base->second->specializable.empty()) {
    return nullptr;
  }
  auto& variants = specialized_shaders_[translation];
  auto it = variants.find(key);
  if (it == variants.end()) {
    it = variants.emplace(key, std::make_unique<HostShader>()).first;
    const std::vector<uint32_t>& words = base->second->specializable;
    Job job{translation, it->second.get(), JobKind::kSpecialize};
    job.base = base->second.get();
    job.values.assign(values, values + words.size());
    job.key = key;
    Queue(std::move(job));
    ++specialized_count_;
  }
  uint32_t state = it->second->state.load(std::memory_order_acquire);
  pending = state == kPending;
  return state == kReady ? it->second->shader.RawPtr() : nullptr;
}

IShader* ShaderCache::GetMotionShader(rg::SpirvShader& shader, uint64_t modification, bool& pending) {
  pending = false;
  rg::Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
  auto host = host_shaders_.find(translation);
  if (host == host_shaders_.end() || host->second->state.load(std::memory_order_acquire) != kReady) return nullptr;
  auto it = motion_shaders_.find(translation);
  if (it == motion_shaders_.end()) {
    // The translation is finished (its host shader is ready): the worker only reads its SPIR-V.
    it = motion_shaders_.emplace(translation, std::make_unique<HostShader>()).first;
    Queue({translation, it->second.get(), JobKind::kMotion});
  }
  uint32_t state = it->second->state.load(std::memory_order_acquire);
  pending = state == kPending;
  return state == kReady ? it->second->shader.RawPtr() : nullptr;
}

void ShaderCache::Worker() {
  rg::SpirvShaderTranslator translator(features_, true, true, false);
  ConfigureScale(translator);
  while (!shutdown_.load(std::memory_order_acquire)) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(jobs_mutex_);
      // Woken by new jobs, and every 50 ms to see a shutdown (the destructor does not lock).
      jobs_cv_.wait_for(lock, std::chrono::milliseconds(50),
                        [this] { return !jobs_.empty() || shutdown_.load(std::memory_order_acquire); });
      if (jobs_.empty()) continue;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    auto start = std::chrono::steady_clock::now();
    switch (job.kind) {
      case JobKind::kMotion: MakeMotionShader(job); break;
      case JobKind::kSpecialize: MakeSpecializedShader(job); break;
      default: Translate(translator, job); break;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::lock_guard<std::mutex> lock(times_mutex_);
    times_.translation.Add(ms);
  }
}

// On a worker thread: the translation's SPIR-V, then its DiligentCore shader (DiligentCore creates device
// objects from any thread).
void ShaderCache::Translate(rg::SpirvShaderTranslator& translator, const Job& job) {
  rg::Shader::Translation* translation = job.translation;
  const rg::Shader& shader = translation->shader();
  uint64_t modification = translation->modification();
  bool vertex = shader.type() == rg::xenos::ShaderType::kVertex;
  auto finish = [&](uint32_t state) { job.host->state.store(state, std::memory_order_release); };
  bool translated = false;
  try {
    translated = translator.TranslateAnalyzedShader(*translation);
  } catch (const std::exception& e) {
    REXLOG_ERROR("[native_x_renderer] translation threw: {}", e.what());
  }
  if (!translated) {
    REXLOG_ERROR("[native_x_renderer] {} shader {:016X} (modification {:016X}): translation failed",
                 vertex ? "vertex" : "pixel", shader.ucode_data_hash(), modification);
    return finish(kFailed);
  }
  if (!translation->is_valid()) return finish(kFailed);
  std::vector<uint32_t> spirv = HostModule(*translation);
  // D146: what a draw could fix in this pixel shader.
  if (specialize_ && !vertex) {
    job.host->specializable =
        spirv::FindUniformWords(spirv.data(), spirv.size(), "xe_uniform_system_constants", SpecializedRanges());
  }

  std::string name = std::string(vertex ? "VS " : "PS ") + std::to_string(shader.ucode_data_hash()) + " mod " +
                     std::to_string(modification);
  ShaderCreateInfo create_info;
  create_info.Desc.Name = name.c_str();
  create_info.Desc.ShaderType = vertex ? SHADER_TYPE_VERTEX : SHADER_TYPE_PIXEL;
  create_info.Desc.UseCombinedTextureSamplers = false;
  create_info.EntryPoint = "main";
  create_info.ByteCode = spirv.data();
  create_info.ByteCodeSize = spirv.size() * 4;
  device_->CreateShader(create_info, &job.host->shader);
  if (!job.host->shader) {
    REXLOG_ERROR("[native_x_renderer] {}: DiligentCore did not accept the SPIR-V ({} bytes)", name, spirv.size() * 4);
    return finish(kFailed);
  }
  finish(kReady);
}

std::vector<uint32_t> ShaderCache::HostModule(const rg::Shader::Translation& translation) const {
  // The translator writes no OpSource, which DiligentCore requires (D18).
  const std::vector<uint8_t>& binary = translation.translated_binary();
  std::vector<uint32_t> spirv =
      spirv::PrepareForDiligent(reinterpret_cast<const uint32_t*>(binary.data()), binary.size() / 4);
  // D131: the textures and samplers named by their place, for the game pipelines' one resource signature (a shader
  // with more than it has keeps the translator's names, and its pipelines their own layout).
  if (ResourceSignatureOn()) {
    const uint32_t stage = translation.shader().type() == rg::xenos::ShaderType::kVertex ? 0 : 1;
    spirv::NamePositionally(spirv, kTexturesSet[stage], kSignaturePrefix[stage], kSignatureTextures[stage],
                            kSignatureSamplers[stage]);
  }
  return spirv;
}

// On a worker thread: a finished pixel translation's SPIR-V with its specializable words made constants (D146).
void ShaderCache::MakeSpecializedShader(const Job& job) {
  const rg::Shader::Translation* translation = job.translation;
  auto finish = [&](uint32_t state) { job.host->state.store(state, std::memory_order_release); };
  std::vector<uint32_t> module = HostModule(*translation);
  std::vector<uint32_t> spirv =
      spirv::SpecializeUniformWords(module.data(), module.size(), "xe_uniform_system_constants", SpecializedRanges(),
                                    job.base->specializable, job.values.data());
  std::string name = "PS " + std::to_string(translation->shader().ucode_data_hash()) + " mod " +
                     std::to_string(translation->modification()) + " specialized " + std::to_string(job.key);
  if (spirv.empty()) {
    REXLOG_WARN("[native_x_renderer] {}: could not be specialized (D146)", name);
    return finish(kFailed);
  }
  ShaderCreateInfo create_info;
  create_info.Desc.Name = name.c_str();
  create_info.Desc.ShaderType = SHADER_TYPE_PIXEL;
  create_info.Desc.UseCombinedTextureSamplers = false;
  create_info.EntryPoint = "main";
  create_info.ByteCode = spirv.data();
  create_info.ByteCodeSize = spirv.size() * 4;
  device_->CreateShader(create_info, &job.host->shader);
  if (!job.host->shader) {
    REXLOG_ERROR("[native_x_renderer] {}: DiligentCore did not accept the SPIR-V ({} bytes)", name, spirv.size() * 4);
    return finish(kFailed);
  }
  finish(kReady);
}

// On a worker thread: the motion vertex shader from a finished translation's SPIR-V (D48).
void ShaderCache::MakeMotionShader(const Job& job) {
  const rg::Shader::Translation* translation = job.translation;
  auto finish = [&](uint32_t state) { job.host->state.store(state, std::memory_order_release); };
  const std::vector<uint8_t>& binary = translation->translated_binary();
  std::vector<uint32_t> motion =
      spirv::MakeMotionVertexShader(reinterpret_cast<const uint32_t*>(binary.data()), binary.size() / 4,
                                    kMotionPreviousLocation, kMotionCurrentLocation);
  if (motion.empty()) {
    // Not an error: the shader has no float constants, for one. Its draws get the camera's motion.
    REXLOG_DEBUG("[native_x_renderer] VS {:016X} (modification {:016X}): no motion vertex shader",
                 translation->shader().ucode_data_hash(), translation->modification());
    return finish(kFailed);
  }
  std::vector<uint32_t> spirv = spirv::PrepareForDiligent(motion.data(), motion.size());
  std::string name = "Motion VS " + std::to_string(translation->shader().ucode_data_hash()) + " mod " +
                     std::to_string(translation->modification());
  ShaderCreateInfo create_info;
  create_info.Desc.Name = name.c_str();
  create_info.Desc.ShaderType = SHADER_TYPE_VERTEX;
  create_info.Desc.UseCombinedTextureSamplers = false;
  create_info.EntryPoint = "main";
  create_info.ByteCode = spirv.data();
  create_info.ByteCodeSize = spirv.size() * 4;
  device_->CreateShader(create_info, &job.host->shader);
  if (!job.host->shader) {
    REXLOG_ERROR("[native_x_renderer] {}: DiligentCore did not accept the SPIR-V ({} bytes)", name, spirv.size() * 4);
    return finish(kFailed);
  }
  finish(kReady);
}

}  // namespace native_x_renderer
