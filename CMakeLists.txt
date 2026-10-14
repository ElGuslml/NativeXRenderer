# XenosNative - native renderer for Xbox 360 games recompiled with the ReXGlue SDK (see README.md).
#
# Phase 1: the trace (src/trace.cpp) and the observer (src/observer.cpp, with the SDK's draw observer
# from sdk_patches/). DiligentCore and the SDK graphics sources are added in
# Phase 2 (docs/plan.md). This folder must not contain game-specific code or addresses: it is
# extracted into its own repository once a second game uses it (docs/decisions.md, D3).
#
# A game's hook definitions (XENOS_NATIVE_HOOK, include/xenos_native/hooks.h) are compiled into the
# game's executable, never into this static library: the generated functions are weak, and a strong
# definition inside an archive member is not guaranteed to be pulled in when the weak one is already
# defined.

add_library(xenos_native STATIC
    src/xenos_native.cpp
    src/trace.cpp
    src/observer.cpp
)
add_library(xenos_native::xenos_native ALIAS xenos_native)

target_include_directories(xenos_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(xenos_native PUBLIC cxx_std_23)
# Cvars, logging, keybinds, and the PPC context the hooks receive.
target_link_libraries(xenos_native PUBLIC rex::runtime)
rexglue_apply_target_settings(xenos_native)
