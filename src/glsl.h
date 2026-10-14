// XenosNative - GLSL to SPIR-V for XenosNative's own utility shaders (internal).
//
// DiligentCore is built without its shader compilers (DILIGENT_NO_GLSLANG, decision D2/D15): the game's
// shaders come as SPIR-V from the SDK's translator. XenosNative's few utility shaders (copies, resolves,
// presenting) are written in GLSL and compiled at startup with the glslang library the SDK already
// builds (decision D16).

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace xenos_native {

enum class GlslStage { kVertex, kPixel };

// Compiles a #version 450 GLSL shader with Vulkan semantics to SPIR-V 1.3. Logs the errors and returns
// an empty vector on failure.
std::vector<uint32_t> CompileGlsl(GlslStage stage, std::string_view source, const char* name);

}  // namespace xenos_native
