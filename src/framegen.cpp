// NativeXRenderer - see framegen.h.

#include "framegen.h"

#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>

#if NATIVE_X_RENDERER_FSR
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
// DiligentCore's Vulkan interfaces need the Vulkan types first.
#include <vulkan/vulkan.h>

#include "DeviceContextVk.h"
#include "RenderDeviceVk.h"
#include "TextureVk.h"

#if NATIVE_X_RENDERER_FSR
#include "ffx_api.h"
#include "ffx_framegeneration.h"
#include "vk/ffx_api_vk.h"
#endif

namespace native_x_renderer {

using namespace Diligent;

#if NATIVE_X_RENDERER_FSR
namespace {

// The loader's vkGetDeviceProcAddr, with the core function where the device did not enable the extension FidelityFX
// asks a function by (as fsr.cpp).
PFN_vkGetDeviceProcAddr g_get_device_proc_addr = nullptr;

PFN_vkVoidFunction VKAPI_CALL DeviceProcAddr(VkDevice device, const char* name) {
  PFN_vkVoidFunction function = g_get_device_proc_addr(device, name);
  if (function) return function;
  std::string core(name);
  for (const char* suffix : {"KHR", "EXT"}) {
    size_t length = std::char_traits<char>::length(suffix);
    if (core.size() > length && core.compare(core.size() - length, length, suffix) == 0) {
      core.resize(core.size() - length);
      return g_get_device_proc_addr(device, core.c_str());
    }
  }
  return nullptr;
}

// FidelityFX's messages, to the log (the first few).
void Message(uint32_t type, const wchar_t* message) {
  static std::atomic<uint32_t> count{0};
  if (count.fetch_add(1) >= 32) return;
  std::string text;
  for (const wchar_t* c = message; c && *c; ++c) text += *c < 128 ? char(*c) : '?';
  if (type == FFX_API_MESSAGE_TYPE_ERROR) {
    REXLOG_ERROR("[native_x_renderer] frame generation: {}", text);
  } else {
    REXLOG_WARN("[native_x_renderer] frame generation: {}", text);
  }
}

uint32_t SurfaceFormat(TEXTURE_FORMAT format) {
  switch (format) {
    case TEX_FORMAT_RGBA8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case TEX_FORMAT_BGRA8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case TEX_FORMAT_RG16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case TEX_FORMAT_R32_FLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
  }
}

FfxApiResource Resource(ITexture* texture, uint32_t state, bool unordered_access) {
  const TextureDesc& desc = texture->GetDesc();
  FfxApiResourceDescription description{};
  description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
  description.format = SurfaceFormat(desc.Format);
  description.width = desc.Width;
  description.height = desc.Height;
  description.depth = 1;
  description.mipCount = 1;
  description.flags = FFX_API_RESOURCE_FLAGS_NONE;
  description.usage = unordered_access ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
  RefCntAutoPtr<ITextureVk> texture_vk(texture, IID_TextureVk);
  VkImage image = texture_vk ? texture_vk->GetVkImage() : VK_NULL_HANDLE;
  return ffxApiGetResourceVK(reinterpret_cast<void*>(image), description, state);
}

}  // namespace
#endif

bool FrameGeneration::Load(IRenderDevice* device) {
  device_ = device;
#if NATIVE_X_RENDERER_FSR
  if (module_) return true;
  wchar_t path[MAX_PATH] = {};
  DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring folder(path, length);
  folder.resize(folder.find_last_of(L"\\/") + 1);
  HMODULE module = LoadLibraryW((folder + L"amd_fidelityfx_vk.dll").c_str());
  if (!module) {
    REXLOG_WARN("[native_x_renderer] frame generation: amd_fidelityfx_vk.dll is not next to the game's executable");
    return false;
  }
  HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
  if (!vulkan) vulkan = LoadLibraryW(L"vulkan-1.dll");
  g_get_device_proc_addr =
      vulkan ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vulkan, "vkGetDeviceProcAddr")) : nullptr;
  create_context_ = reinterpret_cast<void*>(GetProcAddress(module, "ffxCreateContext"));
  destroy_context_ = reinterpret_cast<void*>(GetProcAddress(module, "ffxDestroyContext"));
  configure_ = reinterpret_cast<void*>(GetProcAddress(module, "ffxConfigure"));
  dispatch_ = reinterpret_cast<void*>(GetProcAddress(module, "ffxDispatch"));
  query_ = reinterpret_cast<void*>(GetProcAddress(module, "ffxQuery"));
  if (!create_context_ || !destroy_context_ || !configure_ || !dispatch_ || !query_ || !g_get_device_proc_addr) {
    REXLOG_WARN("[native_x_renderer] frame generation: amd_fidelityfx_vk.dll or the Vulkan loader lacks an entry "
                "point");
    FreeLibrary(module);
    return false;
  }
  module_ = module;
  return true;
#else
  REXLOG_INFO("[native_x_renderer] frame generation: not in this build");
  return false;
#endif
}

bool FrameGeneration::CreateContext(uint32_t display_width, uint32_t display_height, uint32_t render_width,
                                    uint32_t render_height) {
#if NATIVE_X_RENDERER_FSR
  DestroyContext();
  RefCntAutoPtr<IRenderDeviceVk> device(device_, IID_RenderDeviceVk);
  if (!device) return false;
  ffxCreateBackendVKDesc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  backend.vkDevice = device->GetVkDevice();
  backend.vkPhysicalDevice = device->GetVkPhysicalDevice();
  backend.vkDeviceProcAddr = DeviceProcAddr;
  // The view's depth is the upscaler's copy, 0 at the near plane; its motion vectors are at its size, without the
  // jitter; the picture is the composed frame, gamma-encoded 8-bit.
  ffxCreateContextDescFrameGeneration desc{};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  desc.header.pNext = &backend.header;
  desc.flags = 0;
  desc.displaySize = {display_width, display_height};
  desc.maxRenderSize = {render_width, render_height};
  desc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
  ffxContext context = nullptr;
  ffxReturnCode_t result = reinterpret_cast<PfnFfxCreateContext>(create_context_)(&context, &desc.header, nullptr);
  if (result != FFX_API_RETURN_OK || !context) {
    REXLOG_ERROR("[native_x_renderer] frame generation: the context could not be created (FidelityFX error {})",
                 result);
    return false;
  }
  context_ = context;
  context_size_[0] = display_width;
  context_size_[1] = display_height;
  context_size_[2] = render_width;
  context_size_[3] = render_height;
  ffxConfigureDescGlobalDebug1 debug{};
  debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
  debug.fpMessage = Message;
  debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
  reinterpret_cast<PfnFfxConfigure>(configure_)(&context, &debug.header);
  if (!logged_version_) {
    logged_version_ = true;
    ffxQueryGetProviderVersion version{};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    reinterpret_cast<PfnFfxQuery>(query_)(&context, &version.header);
    REXLOG_INFO("[native_x_renderer] frame generation: AMD's {} frame interpolation, the view {}x{}, the picture {}x{} "
                "(D123)",
                version.versionName ? version.versionName : "?", render_width, render_height, display_width,
                display_height);
  }
  return true;
#else
  (void)display_width;
  (void)display_height;
  (void)render_width;
  (void)render_height;
  return false;
#endif
}

void FrameGeneration::DestroyContext() {
#if NATIVE_X_RENDERER_FSR
  if (!context_) return;
  // Its textures may still be in use by frames in flight.
  device_->IdleGPU();
  ffxContext context = context_;
  reinterpret_cast<PfnFfxDestroyContext>(destroy_context_)(&context, nullptr);
  context_ = nullptr;
#endif
}

void FrameGeneration::Prepare(IDeviceContext* context, const View& view, uint32_t display_width,
                              uint32_t display_height) {
  prepared_ = false;
#if NATIVE_X_RENDERER_FSR
  if (!module_ || !view.depth || !view.motion) return;
  const TextureDesc& depth = view.depth->GetDesc();
  bool reset = view.reset;
  if (!context_ || context_size_[0] != display_width || context_size_[1] != display_height ||
      context_size_[2] != depth.Width || context_size_[3] != depth.Height) {
    if (!CreateContext(display_width, display_height, depth.Width, depth.Height)) return;
    reset = true;
    last_generated_ = false;
  }
  StateTransitionDesc barriers[] = {
      {view.depth, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE},
      {view.motion, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE},
  };
  context->TransitionResourceStates(uint32_t(std::size(barriers)), barriers);
  RefCntAutoPtr<IDeviceContextVk> context_vk(context, IID_DeviceContextVk);
  if (!context_vk) return;

  static std::chrono::steady_clock::time_point last;
  auto now = std::chrono::steady_clock::now();
  float frame_ms = last == std::chrono::steady_clock::time_point{}
                       ? 16.7f
                       : std::chrono::duration<float, std::milli>(now - last).count();
  last = now;

  ffxDispatchDescFrameGenerationPrepare prepare{};
  prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
  prepare.frameID = frame_id_;
  prepare.flags = 0;
  prepare.commandList = context_vk->GetVkCommandBuffer();
  prepare.renderSize = {depth.Width, depth.Height};
  prepare.jitterOffset = {view.jitter_x, view.jitter_y};
  prepare.motionVectorScale = {1.0f, 1.0f};
  prepare.frameTimeDelta = std::clamp(frame_ms, 1.0f, 100.0f);
  prepare.cameraNear = view.camera_near;
  prepare.cameraFar = view.camera_far;
  prepare.cameraFovAngleVertical = view.fov_y;
  prepare.viewSpaceToMetersFactor = 1.0f;
  prepare.depth = Resource(view.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
  prepare.motionVectors = Resource(view.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
  ffxContext ffx = context_;
  ffxReturnCode_t result = reinterpret_cast<PfnFfxDispatch>(dispatch_)(&ffx, &prepare.header);
  // FidelityFX recorded its own compute pipelines and bindings (as the upscaler's, D122).
  context->InvalidateState();
  if (result != FFX_API_RETURN_OK) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_ERROR("[native_x_renderer] frame generation: the prepare pass failed (FidelityFX error {})", result);
    }
    return;
  }
  prepared_ = true;
  reset_ = reset;
#else
  (void)context;
  (void)view;
  (void)display_width;
  (void)display_height;
#endif
}

bool FrameGeneration::Generate(IDeviceContext* context, ITexture* current, ITexture* hudless,
                               const int32_t area[4]) {
  bool prepared = prepared_;
  prepared_ = false;
#if NATIVE_X_RENDERER_FSR
  const TextureDesc* desc = current ? &current->GetDesc() : nullptr;
  if (!context_ || !prepared || !desc || desc->Width != context_size_[0] || desc->Height != context_size_[1]) {
    last_generated_ = false;
    ++stats_.skipped;
    return false;
  }
  if (!output_ || output_->GetDesc().Width != desc->Width || output_->GetDesc().Height != desc->Height) {
    output_.Release();
    TextureDesc texture;
    texture.Name = "NativeXRenderer generated frame";
    texture.Type = RESOURCE_DIM_TEX_2D;
    texture.Width = desc->Width;
    texture.Height = desc->Height;
    texture.Format = TEX_FORMAT_RGBA8_UNORM;
    texture.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    device_->CreateTexture(texture, nullptr, &output_);
    output_srv_ = output_ ? output_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    if (!output_) {
      last_generated_ = false;
      return false;
    }
  }
  FfxApiRect2D rect{area[0], area[1], area[2], area[3]};
  ffxConfigureDescFrameGeneration configure{};
  configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  configure.swapChain = nullptr;
  configure.frameGenerationEnabled = true;
  configure.allowAsyncWorkloads = false;
  configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
  configure.generationRect = rect;
  configure.frameID = frame_id_;
  if (hudless) configure.HUDLessColor = Resource(hudless, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
  ffxContext ffx = context_;
  reinterpret_cast<PfnFfxConfigure>(configure_)(&ffx, &configure.header);

  StateTransitionDesc barriers[] = {
      {current, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE},
      {output_, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE},
      {hudless, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE},
  };
  context->TransitionResourceStates(hudless ? 3u : 2u, barriers);
  RefCntAutoPtr<IDeviceContextVk> context_vk(context, IID_DeviceContextVk);
  if (!context_vk) return false;
  // Without the last frame's picture (FidelityFX keeps it from the last dispatch), or a view starting again: the
  // dispatch only keeps this one.
  bool reset = reset_ || !last_generated_;
  ffxDispatchDescFrameGeneration dispatch{};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
  dispatch.commandList = context_vk->GetVkCommandBuffer();
  dispatch.presentColor = Resource(current, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
  dispatch.outputs[0] = Resource(output_, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, true);
  dispatch.numGeneratedFrames = 1;
  dispatch.reset = reset;
  dispatch.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
  dispatch.minMaxLuminance[0] = 0.0f;
  dispatch.minMaxLuminance[1] = 1.0f;
  dispatch.generationRect = rect;
  dispatch.frameID = frame_id_;
  ffxReturnCode_t result = reinterpret_cast<PfnFfxDispatch>(dispatch_)(&ffx, &dispatch.header);
  context->InvalidateState();
  if (result != FFX_API_RETURN_OK) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_ERROR("[native_x_renderer] frame generation: the dispatch failed (FidelityFX error {})", result);
    }
    last_generated_ = false;
    ++stats_.skipped;
    return false;
  }
  last_generated_ = true;
  if (reset) {
    ++stats_.skipped;
    return false;
  }
  ++stats_.generated;
  return true;
#else
  (void)context;
  (void)current;
  (void)hudless;
  (void)area;
  (void)prepared;
  return false;
#endif
}

void FrameGeneration::EndFrame() {
  ++frame_id_;
  prepared_ = false;
}

}  // namespace native_x_renderer
