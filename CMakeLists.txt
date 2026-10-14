# XenosNative - native renderer for Xbox 360 games recompiled with the ReXGlue SDK (see README.md).
#
# Phase 1: the trace (src/trace.cpp) and the observer (src/observer.cpp, with the SDK's draw observer
# from sdk_patches/). Phase 2: DiligentCore (Vulkan) and the renderer (docs/plan.md). This folder must
# not contain game-specific code or addresses: it is extracted into its own repository once a second
# game uses it (docs/decisions.md, D3).
#
# A game's hook definitions (XENOS_NATIVE_HOOK, include/xenos_native/hooks.h) are compiled into the
# game's executable, never into this static library: the generated functions are weak, and a strong
# definition inside an archive member is not guaranteed to be pulled in when the weak one is already
# defined.
#
# Include this folder after the ReXGlue SDK (generated/rexglue.cmake): DiligentCore reuses the SDK's
# Vulkan-Headers, SPIRV-Headers and xxHash targets instead of defining its own.

# ---------------------------------------------------------------------------------------------------
# DiligentCore: the Vulkan backend only.
#
# The integration problems Hell's Gate documented (docs/credits.md), and what is done about them:
# - glslang and SPIRV-Tools: the SDK builds glslang (its targets would collide), and XenosNative gives
#   Diligent SPIR-V from the SDK's translator, never GLSL or HLSL source: DILIGENT_NO_GLSLANG and
#   DILIGENT_NO_HLSL, which also skip SPIRV-Tools.
# - AVX2: Diligent adds -mavx2 to its release builds through a normal variable that a cache value
#   cannot override; the project builds for x86-64-v2, so the flag is removed from
#   Diligent-BuildSettings after the subdirectory is added.
# - VSync and BC1: handled where the swap chain and the texture formats are created (src/).
set(XENOS_NATIVE_DILIGENT_DIR "${CMAKE_SOURCE_DIR}/thirdparty/diligent-core" CACHE PATH
    "DiligentCore source tree (a submodule of the game's repository)")
foreach(_option
    DILIGENT_NO_DIRECT3D11 DILIGENT_NO_DIRECT3D12 DILIGENT_NO_OPENGL DILIGENT_NO_METAL DILIGENT_NO_WEBGPU
    DILIGENT_NO_ARCHIVER DILIGENT_NO_SUPER_RESOLUTION DILIGENT_NO_GLSLANG DILIGENT_NO_HLSL)
  set(${_option} ON CACHE BOOL "Set by xenos_native" FORCE)
endforeach()
set(DILIGENT_BUILD_TESTS OFF CACHE BOOL "Set by xenos_native" FORCE)
set(DILIGENT_INSTALL_CORE OFF CACHE BOOL "Set by xenos_native" FORCE)
set(DILIGENT_NO_FORMAT_VALIDATION ON CACHE BOOL "Set by xenos_native" FORCE)
add_subdirectory("${XENOS_NATIVE_DILIGENT_DIR}" "${CMAKE_BINARY_DIR}/diligent-core" EXCLUDE_FROM_ALL)

get_target_property(_diligent_options Diligent-BuildSettings INTERFACE_COMPILE_OPTIONS)
if(_diligent_options)
  list(TRANSFORM _diligent_options REPLACE "-mavx2" "")
  list(TRANSFORM _diligent_options REPLACE "/arch:AVX2" "")
  set_property(TARGET Diligent-BuildSettings PROPERTY INTERFACE_COMPILE_OPTIONS "${_diligent_options}")
endif()

# ---------------------------------------------------------------------------------------------------
# The library.

add_library(xenos_native STATIC
    src/xenos_native.cpp
    src/trace.cpp
    src/observer.cpp
    src/renderer.cpp
    src/device.cpp
)
add_library(xenos_native::xenos_native ALIAS xenos_native)

target_include_directories(xenos_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(xenos_native PUBLIC cxx_std_23)
# Cvars, logging, keybinds, and the PPC context the hooks receive.
target_link_libraries(xenos_native PUBLIC rex::runtime)
target_link_libraries(xenos_native PRIVATE
    Diligent-GraphicsEngineVk-static
    Diligent-GraphicsEngineVkInterface
    Diligent-GraphicsEngineInterface
    Diligent-PlatformInterface
    Diligent-Common
)
rexglue_apply_target_settings(xenos_native)
