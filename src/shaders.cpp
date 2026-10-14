// XenosNative - see shaders.h.

#include "shaders.h"

#include "spirv_patch.h"
#include "storage.h"

#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <vector>

#include <xxhash.h>

namespace xenos_native {

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

bool ShaderCache::Initialize(IRenderDevice* device, const RenderScale& scale) {
  device_ = device;
  scale_ = scale;
  // What the device created by device.cpp has. DiligentCore enables neither clip and cull distances nor
  // sample rate shading, and XenosNative sets texture swizzles in the shader (system constants) rather
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
  translator_ = std::make_unique<rg::SpirvShaderTranslator>(features_, true, true, false);
  translator_->SetFractionalResolutionScale(scale_.x, scale_.y);
  // Two workers: a level load translates about a hundred shaders at up to 2.7 ms each, and the game's
  // threads and DiligentCore's four pipeline compilation threads (D28) need the cores at the same time.
  unsigned worker_count = std::thread::hardware_concurrency() >= 4 ? 2 : 1;
  for (unsigned i = 0; i < worker_count; ++i) workers_.emplace_back([this] { Worker(); });
  return true;
}

ShaderCache::Times ShaderCache::TakeTimes() {
  std::lock_guard<std::mutex> lock(times_mutex_);
  Times times = times_;
  times_ = {};
  return times;
}

rg::SpirvShader* ShaderCache::Get(rg::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords,
                                  bool* new_failure) {
  if (new_failure) *new_failure = false;
  uint64_t hash = XXH3_64bits(ucode, size_t(dwords) * sizeof(uint32_t));
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
    REXLOG_ERROR("[xenos_native] {} shader {:016X} ({} dwords): analysis failed: {}",
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
    {
      std::lock_guard<std::mutex> lock(jobs_mutex_);
      jobs_.push_back({translation, it->second.get()});
    }
    jobs_cv_.notify_one();
  }
  uint32_t state = it->second->state.load(std::memory_order_acquire);
  pending = state == kPending;
  return state == kReady ? it->second->shader.RawPtr() : nullptr;
}

void ShaderCache::Worker() {
  rg::SpirvShaderTranslator translator(features_, true, true, false);
  // The scale the shaders revert where they read pixel positions and resolved textures (sdk_patches/0005).
  translator.SetFractionalResolutionScale(scale_.x, scale_.y);
  while (!shutdown_.load(std::memory_order_acquire)) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(jobs_mutex_);
      // Woken by new jobs, and every 50 ms to see a shutdown (the destructor does not lock).
      jobs_cv_.wait_for(lock, std::chrono::milliseconds(50),
                        [this] { return !jobs_.empty() || shutdown_.load(std::memory_order_acquire); });
      if (jobs_.empty()) continue;
      job = jobs_.front();
      jobs_.pop_front();
    }
    auto start = std::chrono::steady_clock::now();
    Translate(translator, job);
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
    REXLOG_ERROR("[xenos_native] translation threw: {}", e.what());
  }
  if (!translated) {
    REXLOG_ERROR("[xenos_native] {} shader {:016X} (modification {:016X}): translation failed",
                 vertex ? "vertex" : "pixel", shader.ucode_data_hash(), modification);
    return finish(kFailed);
  }
  if (!translation->is_valid()) return finish(kFailed);
  // The translator writes no OpSource, which DiligentCore requires (D18).
  const std::vector<uint8_t>& binary = translation->translated_binary();
  std::vector<uint32_t> spirv =
      spirv::PrepareForDiligent(reinterpret_cast<const uint32_t*>(binary.data()), binary.size() / 4);

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
    REXLOG_ERROR("[xenos_native] {}: DiligentCore did not accept the SPIR-V ({} bytes)", name, spirv.size() * 4);
    return finish(kFailed);
  }
  finish(kReady);
}

}  // namespace xenos_native
