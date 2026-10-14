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
# XenosNative's changes to DiligentCore (diligent_patches/, decision D22), applied before it is added.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_diligent_patches.cmake")
add_subdirectory("${XENOS_NATIVE_DILIGENT_DIR}" "${CMAKE_BINARY_DIR}/diligent-core" EXCLUDE_FROM_ALL)

get_target_property(_diligent_options Diligent-BuildSettings INTERFACE_COMPILE_OPTIONS)
if(_diligent_options)
  list(TRANSFORM _diligent_options REPLACE "-mavx2" "")
  list(TRANSFORM _diligent_options REPLACE "/arch:AVX2" "")
  set_property(TARGET Diligent-BuildSettings PROPERTY INTERFACE_COMPILE_OPTIONS "${_diligent_options}")
endif()

# ---------------------------------------------------------------------------------------------------
# The SDK's graphics code XenosNative uses in process (decision D15): the shader translator to SPIR-V,
# the register, texture and draw utilities. The SDK compiles it only into its GPU plugin, with hidden
# symbols. It is compiled again here, except the cvar definitions, which stay the plugin's
# (src/sdk_cvars.cpp explains why and how).
set(_xn_sdk_graphics "${REXSDK_DIR}/src/graphics")
add_library(xenos_native_sdk STATIC
    ${_xn_sdk_graphics}/register_file.cpp
    ${_xn_sdk_graphics}/registers.cpp
    ${_xn_sdk_graphics}/xenos.cpp
    ${_xn_sdk_graphics}/sampler_info.cpp
    ${_xn_sdk_graphics}/format/ucode.cpp
    ${_xn_sdk_graphics}/pipeline/shader/shader.cpp
    ${_xn_sdk_graphics}/pipeline/shader/translator.cpp
    ${_xn_sdk_graphics}/pipeline/shader/translator_disasm.cpp
    ${_xn_sdk_graphics}/pipeline/shader/spirv.cpp
    ${_xn_sdk_graphics}/pipeline/shader/spirv_builder.cpp
    ${_xn_sdk_graphics}/pipeline/shader/spirv_translator.cpp
    ${_xn_sdk_graphics}/pipeline/shader/spirv_translator_alu.cpp
    ${_xn_sdk_graphics}/pipeline/shader/spirv_translator_fetch.cpp
    ${_xn_sdk_graphics}/pipeline/shader/spirv_translator_memexport.cpp
    ${_xn_sdk_graphics}/pipeline/shader/spirv_translator_rb.cpp
    ${_xn_sdk_graphics}/pipeline/texture/info.cpp
    ${_xn_sdk_graphics}/pipeline/texture/info_formats.cpp
    ${_xn_sdk_graphics}/pipeline/texture/util.cpp
    ${_xn_sdk_graphics}/pipeline/texture/conversion.cpp
    ${_xn_sdk_graphics}/pipeline/texture/extent.cpp
    ${_xn_sdk_graphics}/shared_memory.cpp
    src/sdk_cvars.cpp
    src/sdk_draw.cpp
    src/sdk_primitive_processor.cpp
    src/sdk_texture_cache.cpp
    # glslang's default resource limits, for XenosNative's utility shaders (src/glsl.cpp, decision D16).
    ${REXSDK_DIR}/thirdparty/glslang/StandAlone/ResourceLimits.cpp
)
target_compile_definitions(xenos_native_sdk PRIVATE
    XENOS_NATIVE_SDK_DRAW_CPP="${_xn_sdk_graphics}/util/draw.cpp"
    XENOS_NATIVE_SDK_PRIMITIVE_PROCESSOR_CPP="${_xn_sdk_graphics}/primitive_processor.cpp"
    XENOS_NATIVE_SDK_TEXTURE_CACHE_CPP="${_xn_sdk_graphics}/pipeline/texture/cache.cpp")
target_compile_features(xenos_native_sdk PUBLIC cxx_std_23)
target_compile_options(xenos_native_sdk PRIVATE -Wno-unused-variable)
# The same third-party pieces the plugin compiles these files with.
target_include_directories(xenos_native_sdk PRIVATE "${REXSDK_DIR}")
# The SDK's graphics headers include renderdoc_app.h (through rex/ui) and glslang's.
target_include_directories(xenos_native_sdk PUBLIC "${REXSDK_DIR}/thirdparty/renderdoc" "${REXSDK_DIR}/thirdparty/glslang")
target_link_libraries(xenos_native_sdk PUBLIC rex::runtime Vulkan::Headers glslang glslang::SPIRV
    PRIVATE xxHash::xxhash)
rexglue_apply_target_settings(xenos_native_sdk)

# ---------------------------------------------------------------------------------------------------
# The library.

add_library(xenos_native STATIC
    src/xenos_native.cpp
    src/trace.cpp
    src/observer.cpp
    src/state.cpp
    src/renderer.cpp
    src/device.cpp
    src/targets.cpp
    src/blit.cpp
    src/aa.cpp
    src/glsl.cpp
    src/guest_memory.cpp
    src/primitives.cpp
    src/shaders.cpp
    src/spirv_patch.cpp
    src/pipelines.cpp
    src/textures.cpp
    src/gamma.cpp
    src/guest_textures.cpp
    src/queries.cpp
    src/draw.cpp
)
add_library(xenos_native::xenos_native ALIAS xenos_native)

target_include_directories(xenos_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# The SDK's precompiled SPIR-V shaders (texture loading), included by guest_textures.cpp.
target_include_directories(xenos_native PRIVATE "${REXSDK_DIR}/src/graphics/shaders")
# SMAA (thirdparty/smaa, MIT): its lookup textures are headers, and its shader source is compiled at startup
# (src/aa.cpp), so it is embedded as a string, regenerated when the file changes.
set(_xn_smaa_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/smaa/SMAA.hlsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_xn_smaa_hlsl}")
file(READ "${_xn_smaa_hlsl}" _xn_smaa_source)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/smaa_source.h"
     "// Generated by xenos_native/CMakeLists.txt from thirdparty/smaa/SMAA.hlsl.\n#pragma once\n"
     "constexpr const char kSmaaSource[] = R\"SMAA(${_xn_smaa_source})SMAA\";\n")
target_include_directories(xenos_native PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/smaa"
                                                "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(xenos_native PUBLIC cxx_std_23)
# Cvars, logging, keybinds, and the PPC context the hooks receive.
target_link_libraries(xenos_native PUBLIC rex::runtime)
target_link_libraries(xenos_native PRIVATE
    xenos_native_sdk
    Diligent-GraphicsEngineVk-static
    Diligent-GraphicsEngineVkInterface
    Diligent-GraphicsEngineInterface
    Diligent-GraphicsAccessories
    Diligent-PlatformInterface
    Diligent-Common
    xxHash::xxhash
)
rexglue_apply_target_settings(xenos_native)
