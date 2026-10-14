// XenosNative - guest shaders, translated to SPIR-V by the SDK and loaded as DiligentCore shaders (internal).
//
// A guest shader is its microcode, as the last IM_LOAD before a draw gave it to the GPU (state.h). It is
// identified by a hash of the microcode, analyzed once (the SDK's Shader::AnalyzeUcode), and translated
// once per modification: the bits the draw state adds to the microcode (host vertex shader type,
// interpolators, user clip planes, depth mode...), which the SDK's SpirvShaderTranslator turns into
// different SPIR-V. The SPIR-V's resources are bound by name (draw.cpp).

#pragma once

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>

#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Shader.h"
#include "internal.h"

namespace xenos_native {

class ShaderCache {
 public:
  ShaderCache();
  ~ShaderCache();

  bool Initialize(Diligent::IRenderDevice* device);

  rex::graphics::SpirvShaderTranslator& translator() { return *translator_; }
  const rex::graphics::SpirvShaderTranslator::Features& features() const { return features_; }

  // The guest shader with this microcode (big-endian dwords, as in guest memory), analyzed. Created on
  // first use. nullptr if the SDK could not analyze it (logged once); `new_failure` (optional) is set when
  // that happened in this call.
  rex::graphics::SpirvShader* Get(rex::graphics::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords,
                                  bool* new_failure = nullptr);

  // The host shader of one translation of a guest shader, translated and created on first use. nullptr
  // if the translation or the shader creation failed (logged once).
  Diligent::IShader* GetHostShader(rex::graphics::SpirvShader& shader, uint64_t modification);

  size_t count() const { return shaders_.size(); }
  size_t translation_count() const { return host_shaders_.size(); }

  // Analyses, and translations with their DiligentCore shaders, since the last call.
  struct Times {
    internal::CreationTime analysis, translation;
  };
  Times TakeTimes() {
    Times times = times_;
    times_ = {};
    return times;
  }

 private:
  Diligent::IRenderDevice* device_ = nullptr;
  rex::graphics::SpirvShaderTranslator::Features features_;
  std::unique_ptr<rex::graphics::SpirvShaderTranslator> translator_;
  rex::string::StringBuffer disassembly_buffer_;
  Times times_;
  // nullptr for a shader the analysis failed on.
  std::map<std::pair<uint64_t, uint32_t>, std::unique_ptr<rex::graphics::SpirvShader>> shaders_;
  // nullptr for a translation that failed.
  std::unordered_map<const rex::graphics::Shader::Translation*, Diligent::RefCntAutoPtr<Diligent::IShader>>
      host_shaders_;
};

}  // namespace xenos_native
