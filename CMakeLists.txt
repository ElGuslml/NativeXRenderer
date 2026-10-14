# XenosNative - native renderer for Xbox 360 games recompiled with the ReXGlue SDK (see README.md).
#
# Phase 0: the library target only, with no behavior yet. DiligentCore and the SDK graphics sources are
# added in Phase 2 (docs/plan.md). This folder must not contain game-specific code or addresses: it is
# extracted into its own repository once a second game uses it (docs/decisions.md, D3).
#
# A game's hook definitions (REX_HOOK_RAW over the D3D entry points) are compiled into the game's
# executable, never into this static library: the generated functions are weak, and a strong
# definition inside an archive member is not guaranteed to be pulled in when the weak one is already
# defined.

add_library(xenos_native STATIC
    src/xenos_native.cpp
)
add_library(xenos_native::xenos_native ALIAS xenos_native)

target_include_directories(xenos_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(xenos_native PUBLIC cxx_std_23)
