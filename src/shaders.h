// XenosNative - guest shaders, translated to SPIR-V by the SDK and loaded as DiligentCore shaders (internal).
//
// A guest shader is its microcode, as the last IM_LOAD before a draw gave it to the GPU (state.h). It is
// identified by a hash of the microcode, analyzed once (the SDK's Shader::AnalyzeUcode), and translated
// once per modification: the bits the draw state adds to the microcode (host vertex shader type,
// interpolators, user clip planes, depth mode...), which the SDK's SpirvShaderTranslator turns into
// different SPIR-V. The SPIR-V's resources are bound by name (draw.cpp).
//
// Analysis happens on the render thread, when a draw first uses the microcode: it is quick, and the draw
// needs its results to choose the modification. Translation and the DiligentCore shader are made on
// worker threads, each with its own translator (D34): a draw whose shader is not ready yet is skipped, as
// one whose pipeline is still compiling (D28).

#pragma once

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Shader.h"
#include "internal.h"
#include "scale.h"

namespace xenos_native {

class ShaderCache {
 public:
  ShaderCache();
  ~ShaderCache();

  // `scale`: the render scale the translated shaders revert (D37).
  bool Initialize(Diligent::IRenderDevice* device, const RenderScale& scale);

  // The render thread's translator, for the default modifications (it translates nothing).
  rex::graphics::SpirvShaderTranslator& translator() { return *translator_; }
  const rex::graphics::SpirvShaderTranslator::Features& features() const { return features_; }

  // The guest shader with this microcode (big-endian dwords, as in guest memory), analyzed. Created on
  // first use. nullptr if the SDK could not analyze it (logged once); `new_failure` (optional) is set when
  // that happened in this call.
  rex::graphics::SpirvShader* Get(rex::graphics::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords,
                                  bool* new_failure = nullptr);

  // The host shader of one translation of a guest shader. The first call queues the translation for a
  // worker thread. nullptr with `pending` set until the worker finished; nullptr without it if the
  // translation or the shader creation failed (logged once).
  Diligent::IShader* GetHostShader(rex::graphics::SpirvShader& shader, uint64_t modification, bool& pending);

  size_t count() const { return shaders_.size(); }
  size_t translation_count() const { return host_shaders_.size(); }

  // Analyses (render thread), and translations with their DiligentCore shaders (worker threads), since
  // the last call.
  struct Times {
    internal::CreationTime analysis, translation;
  };
  Times TakeTimes();

 private:
  enum : uint32_t { kPending, kReady, kFailed };
  struct HostShader {
    std::atomic<uint32_t> state{kPending};
    Diligent::RefCntAutoPtr<Diligent::IShader> shader;  // written by the worker before state
  };
  struct Job {
    rex::graphics::Shader::Translation* translation;
    HostShader* host;
  };

  void Worker();
  void Translate(rex::graphics::SpirvShaderTranslator& translator, const Job& job);

  Diligent::IRenderDevice* device_ = nullptr;
  rex::graphics::SpirvShaderTranslator::Features features_;
  RenderScale scale_;
  std::unique_ptr<rex::graphics::SpirvShaderTranslator> translator_;
  rex::string::StringBuffer disassembly_buffer_;
  // nullptr for a shader the analysis failed on. Render thread only; a worker reads only the analysis of
  // the shader its translation belongs to, which no longer changes.
  std::map<std::pair<uint64_t, uint32_t>, std::unique_ptr<rex::graphics::SpirvShader>> shaders_;
  // By translation. Render thread only; a worker writes the HostShader of its job.
  std::unordered_map<const rex::graphics::Shader::Translation*, std::unique_ptr<HostShader>> host_shaders_;

  // The workers. The destructor takes no lock (at process exit a worker may have been terminated holding
  // one): workers wake up on their own to see `shutdown_`.
  std::vector<std::thread> workers_;
  std::mutex jobs_mutex_;
  std::condition_variable jobs_cv_;
  std::deque<Job> jobs_;
  std::atomic<bool> shutdown_{false};

  std::mutex times_mutex_;
  Times times_;
};

}  // namespace xenos_native
