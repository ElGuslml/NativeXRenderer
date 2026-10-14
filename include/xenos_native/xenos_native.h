// XenosNative - native renderer for Xbox 360 games recompiled with the ReXGlue SDK.
//
// Replaces the drawing done through the game's XDK Direct3D library with host rendering on
// DiligentCore (Vulkan), keeping the SDK's Xenos emulation as the fallback. See xenos_native/README.md
// and xenos_native/docs/.
//
// Phase 0: placeholder. The interface the game side calls (the D3D entry point handlers and the XDK
// profile) is designed in Phase 1 (docs/plan.md, docs/architecture.md "XDK profile").

#pragma once

namespace xenos_native {

// Bumped when the interface the game side uses changes.
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;

// One line for the game's startup log: version and what the library does at this stage.
const char* Describe();

}  // namespace xenos_native
