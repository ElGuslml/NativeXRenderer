// NativeXRenderer - see glsl.h.

#include "glsl.h"

#include <rex/logging.h>

#include <SPIRV/GlslangToSpv.h>
#include <StandAlone/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include <mutex>
#include <string>

namespace native_x_renderer {

std::vector<uint32_t> CompileGlsl(GlslStage stage, std::string_view source, const char* name) {
  static std::once_flag initialized;
  std::call_once(initialized, [] { glslang::InitializeProcess(); });

  EShLanguage language = stage == GlslStage::kVertex ? EShLangVertex : EShLangFragment;
  glslang::TShader shader(language);
  std::string text(source);
  const char* strings[] = {text.c_str()};
  shader.setStrings(strings, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
  auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&glslang::DefaultTBuiltInResource, 450, false, messages)) {
    REXLOG_ERROR("[native_x_renderer] GLSL {}: {}", name, shader.getInfoLog());
    return {};
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    REXLOG_ERROR("[native_x_renderer] GLSL {} (link): {}", name, program.getInfoLog());
    return {};
  }
  std::vector<uint32_t> spirv;
  glslang::GlslangToSpv(*program.getIntermediate(language), spirv);
  return spirv;
}

}  // namespace native_x_renderer
