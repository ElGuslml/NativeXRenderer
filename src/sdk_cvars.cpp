// XenosNative - the SDK graphics cvars the in-process SDK code reads (decision D15).
//
// XenosNative compiles part of the SDK's graphics code (the shader translator, the register, draw and
// texture utilities) into the executable. Those sources read cvars that the GPU plugin defines and
// registers (compiled into rexgpu-xenos). Compiling their definitions again would register them twice,
// and the registry ignores the second registration: the executable's copy would win, and the settings
// (potcawe.toml, the command line, src/startup.cpp's defaults) would stop reaching the plugin, which still
// runs the Xenos path. So:
//   - cvars defined in sources XenosNative does not compile (flags.cpp) get a forwarding accessor here;
//   - sources XenosNative compiles that define cvars (util/draw.cpp, pipeline/texture/cache.cpp) are
//     compiled through wrappers (sdk_draw.cpp, sdk_texture_cache.cpp) that turn the definitions into
//     forwarding accessors (sdk_cvars.h).
// Both forward to the value registered by the plugin; nothing is registered from the executable.

#include "sdk_cvars.h"

std::string& FLAGS_dump_shaders_storage_() {
  return xenos_native::sdk::Forward<std::string>("dump_shaders", std::string());
}
bool& FLAGS_gpu_allow_invalid_fetch_constants_storage_() {
  return xenos_native::sdk::Forward<bool>("gpu_allow_invalid_fetch_constants", false);
}
