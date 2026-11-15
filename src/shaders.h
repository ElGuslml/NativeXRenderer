// NativeXRenderer - guest shaders, translated to SPIR-V by the SDK and loaded as DiligentCore shaders (internal).
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

namespace native_x_renderer {

class PipelineStorage;

class ShaderCache {
 public:
  ShaderCache();
  ~ShaderCache();

  // `scale_in_constants`: the translations read the scale they revert from the system constants (D128), so one
  // translation serves every render scale and each draw gives its own (DrawPath::FillSystemConstants); otherwise
  // `scale`, the render scale (D37), is written into each translation. `worker_count`: the translating threads
  // (0: the game's two; the pipeline preparation tool, D129, takes more).
  bool Initialize(Diligent::IRenderDevice* device, const RenderScale& scale, bool scale_in_constants,
                  unsigned worker_count = 0);

  // The render thread's translator, for the default modifications (it translates nothing).
  rex::graphics::SpirvShaderTranslator& translator() { return *translator_; }
  const rex::graphics::SpirvShaderTranslator::Features& features() const { return features_; }

  // The guest shader with this microcode (big-endian dwords, as in guest memory), analyzed. Created on
  // first use. nullptr if the SDK could not analyze it (logged once); `new_failure` (optional) is set when
  // that happened in this call.
  rex::graphics::SpirvShader* Get(rex::graphics::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords,
                                  bool* new_failure = nullptr);
  // The same, with the microcode's XXH3 already known (the state tracker hashes a program once per load; the
  // draws then skip it, D68).
  rex::graphics::SpirvShader* Get(rex::graphics::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords,
                                  uint64_t hash, bool* new_failure);

  // The host shader of one translation of a guest shader. The first call queues the translation for a
  // worker thread. nullptr with `pending` set until the worker finished; nullptr without it if the
  // translation or the shader creation failed (logged once).
  Diligent::IShader* GetHostShader(rex::graphics::SpirvShader& shader, uint64_t modification, bool& pending);
  // The motion vertex shader of a translation whose host shader is ready (spirv::MakeMotionVertexShader,
  // D48), made on a worker thread the same way. Its motion outputs are at these locations.
  static constexpr uint32_t kMotionPreviousLocation = 30, kMotionCurrentLocation = 31;
  Diligent::IShader* GetMotionShader(rex::graphics::SpirvShader& shader, uint64_t modification, bool& pending);

  // D146: a pixel shader translation specialized to what a draw decides in the system constants per texture and per
  // state: the flags, the textures at the render scale, the textures' signs and swizzles. The translations read them
  // at run time and decode every texture component both ways (unsigned and signed views); fixed, the driver drops
  // the paths not taken. Off with native_x_renderer_specialize (read at Initialize).
  bool specialize() const { return specialize_; }
  // The byte ranges of the system constants a specialization fixes.
  static const std::vector<std::pair<uint32_t, uint32_t>>& SpecializedRanges();
  // The dword indices in the system constants of the words in those ranges that a translation reads; nullptr while
  // its host shader is not ready (or it failed). Empty: nothing to specialize.
  const std::vector<uint32_t>* SpecializableWords(rex::graphics::SpirvShader& shader, uint64_t modification);
  // The host shader of that translation with those words fixed to `values` (one per index, in their order), `key`
  // a hash of them; made on a worker like a translation. nullptr with `pending` set until it is ready; nullptr
  // without it if it could not be made.
  Diligent::IShader* GetSpecializedShader(rex::graphics::SpirvShader& shader, uint64_t modification, uint64_t key,
                                          const uint32_t* values, bool& pending);
  // Specialized host shaders made so far.
  size_t specialized_count() const { return specialized_count_; }

  // An analyzed shader already in the cache, nullptr if not (or if its analysis failed).
  rex::graphics::SpirvShader* Find(rex::graphics::xenos::ShaderType type, uint64_t hash) const;
  // New shaders are recorded there (D42).
  void SetStorage(PipelineStorage* storage) { storage_ = storage; }

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
    // D146: a pixel translation's specializable words (SpecializableWords), written by the worker before state.
    std::vector<uint32_t> specializable;
  };
  enum class JobKind : uint8_t { kTranslate, kMotion, kSpecialize };
  struct Job {
    rex::graphics::Shader::Translation* translation;
    HostShader* host;
    JobKind kind;
    // kSpecialize: the translation's host shader (ready), and the values of its specializable words.
    const HostShader* base = nullptr;
    std::vector<uint32_t> values;
    uint64_t key = 0;
  };

  void Worker();
  void Translate(rex::graphics::SpirvShaderTranslator& translator, const Job& job);
  void MakeMotionShader(const Job& job);
  void MakeSpecializedShader(const Job& job);
  // The translation's SPIR-V as its host shader takes it: an OpSource (D18), the textures named by their place (D131).
  std::vector<uint32_t> HostModule(const rex::graphics::Shader::Translation& translation) const;
  void Queue(Job job);
  // The scale the translations revert, as Initialize was told (D128).
  void ConfigureScale(rex::graphics::SpirvShaderTranslator& translator) const;

  Diligent::IRenderDevice* device_ = nullptr;
  rex::graphics::SpirvShaderTranslator::Features features_;
  RenderScale scale_;
  bool scale_in_constants_ = true;
  PipelineStorage* storage_ = nullptr;
  std::unique_ptr<rex::graphics::SpirvShaderTranslator> translator_;
  rex::string::StringBuffer disassembly_buffer_;
  // nullptr for a shader the analysis failed on. Render thread only; a worker reads only the analysis of
  // the shader its translation belongs to, which no longer changes.
  std::map<std::pair<uint64_t, uint32_t>, std::unique_ptr<rex::graphics::SpirvShader>> shaders_;
  // By translation. Render thread only; a worker writes the HostShader of its job.
  std::unordered_map<const rex::graphics::Shader::Translation*, std::unique_ptr<HostShader>> host_shaders_;
  std::unordered_map<const rex::graphics::Shader::Translation*, std::unique_ptr<HostShader>> motion_shaders_;
  // D146: by translation, then by the hash of the specialized words' values.
  std::unordered_map<const rex::graphics::Shader::Translation*,
                     std::unordered_map<uint64_t, std::unique_ptr<HostShader>>>
      specialized_shaders_;
  bool specialize_ = false;
  size_t specialized_count_ = 0;

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

}  // namespace native_x_renderer
