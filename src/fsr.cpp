// NativeXRenderer - see fsr.h.

#include "fsr.h"

#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
#include "RefCntAutoPtr.hpp"
#include "RenderDeviceVk.h"
#include "TextureVk.h"

#if NATIVE_X_RENDERER_FSR
#include "ffx_api.h"
#include "ffx_upscale.h"
#include "vk/ffx_api_vk.h"
#endif

namespace native_x_renderer {

using namespace Diligent;

#if NATIVE_X_RENDERER_FSR
namespace {

// The loader's vkGetDeviceProcAddr. FidelityFX's Vulkan backend looks some functions up by their extension's name
// (vkGetBufferMemoryRequirements2KHR) and calls them without checking; where the device did not enable that extension,
// the core function of the same name without the suffix is given instead (Vulkan 1.1 and later have them).
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

// FidelityFX's messages, to the log (the first few: a broken frame would repeat them every frame).
void Message(uint32_t type, const wchar_t* message) {
  static std::atomic<uint32_t> count{0};
  if (count.fetch_add(1) >= 32) return;
  std::string text;
  for (const wchar_t* c = message; c && *c; ++c) text += *c < 128 ? char(*c) : '?';
  if (type == FFX_API_MESSAGE_TYPE_ERROR) {
    REXLOG_ERROR("[native_x_renderer] FSR 3.1: {}", text);
  } else {
    REXLOG_WARN("[native_x_renderer] FSR 3.1: {}", text);
  }
}

uint32_t SurfaceFormat(TEXTURE_FORMAT format) {
  switch (format) {
    case TEX_FORMAT_RGBA32_FLOAT: return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    case TEX_FORMAT_RGBA16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case TEX_FORMAT_RGBA8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case TEX_FORMAT_RGBA8_UNORM_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case TEX_FORMAT_BGRA8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case TEX_FORMAT_BGRA8_UNORM_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case TEX_FORMAT_R11G11B10_FLOAT: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case TEX_FORMAT_RGB10A2_UNORM: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
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

bool IsFloatFormat(TEXTURE_FORMAT format) {
  return format == TEX_FORMAT_RGBA16_FLOAT || format == TEX_FORMAT_RGBA32_FLOAT || format == TEX_FORMAT_R11G11B10_FLOAT;
}

}  // namespace
#endif

Fsr::~Fsr() {
  // The context is not destroyed here: at the process's end the device may be gone already. Its memory goes with
  // the process.
}

void Fsr::RenderSize(uint32_t output_width, uint32_t output_height, int quality, uint32_t& width,
                     uint32_t& height) {
  static constexpr float kRatios[] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
  float ratio = kRatios[std::clamp(quality, 0, 4)];
  width = std::max(1u, uint32_t(float(output_width) / ratio));
  height = std::max(1u, uint32_t(float(output_height) / ratio));
}

uint32_t Fsr::JitterPhases(uint32_t render_width, uint32_t output_width) {
  float ratio = float(output_width) / float(std::max(render_width, 1u));
  return std::max(1u, uint32_t(8.0f * ratio * ratio));
}

bool Fsr::Load(IRenderDevice* device) {
  device_ = device;
#if NATIVE_X_RENDERER_FSR
  if (module_) return true;
  wchar_t path[MAX_PATH] = {};
  DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring folder(path, length);
  folder.resize(folder.find_last_of(L"\\/") + 1);
  HMODULE module = LoadLibraryW((folder + L"amd_fidelityfx_vk.dll").c_str());
  if (!module) {
    REXLOG_WARN("[native_x_renderer] FSR 3.1: amd_fidelityfx_vk.dll is not next to the game's executable");
    return false;
  }
  HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
  if (!vulkan) vulkan = LoadLibraryW(L"vulkan-1.dll");
  g_get_device_proc_addr =
      vulkan ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vulkan, "vkGetDeviceProcAddr")) : nullptr;
  create_context_ = reinterpret_cast<void*>(GetProcAddress(module, "ffxCreateContext"));
  destroy_context_ = reinterpret_cast<void*>(GetProcAddress(module, "ffxDestroyContext"));
  dispatch_ = reinterpret_cast<void*>(GetProcAddress(module, "ffxDispatch"));
  query_ = reinterpret_cast<void*>(GetProcAddress(module, "ffxQuery"));
  if (!create_context_ || !destroy_context_ || !dispatch_ || !query_ || !g_get_device_proc_addr) {
    REXLOG_WARN("[native_x_renderer] FSR 3.1: amd_fidelityfx_vk.dll or the Vulkan loader lacks an entry point");
    FreeLibrary(module);
    return false;
  }
  module_ = module;
  return true;
#else
  REXLOG_INFO("[native_x_renderer] FSR 3.1: not in this build");
  return false;
#endif
}

bool Fsr::CreateContext(uint32_t render_width, uint32_t render_height, uint32_t output_width, uint32_t output_height,
                        uint32_t flags) {
#if NATIVE_X_RENDERER_FSR
  DestroyContext();
  RefCntAutoPtr<IRenderDeviceVk> device(device_, IID_RenderDeviceVk);
  if (!device) return false;
  ffxCreateBackendVKDesc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  backend.vkDevice = device->GetVkDevice();
  backend.vkPhysicalDevice = device->GetVkPhysicalDevice();
  backend.vkDeviceProcAddr = DeviceProcAddr;
  ffxCreateContextDescUpscale desc{};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  desc.header.pNext = &backend.header;
  desc.flags = flags;
  desc.maxRenderSize = {render_width, render_height};
  desc.maxUpscaleSize = {output_width, output_height};
  desc.fpMessage = Message;
  ffxContext context = nullptr;
  ffxReturnCode_t result = reinterpret_cast<PfnFfxCreateContext>(create_context_)(&context, &desc.header, nullptr);
  if (result != FFX_API_RETURN_OK || !context) {
    REXLOG_ERROR("[native_x_renderer] FSR 3.1: the context could not be created (FidelityFX error {})", result);
    return false;
  }
  context_ = context;
  context_size_[0] = render_width;
  context_size_[1] = render_height;
  context_size_[2] = output_width;
  context_size_[3] = output_height;
  context_flags_ = flags;
  last_dispatch_ = {};
  if (!logged_version_) {
    logged_version_ = true;
    ffxQueryGetProviderVersion version{};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    reinterpret_cast<PfnFfxQuery>(query_)(&context_, &version.header);
    REXLOG_INFO("[native_x_renderer] FSR 3.1: AMD's {} upscaler, {}x{} to {}x{}{}{}",
                version.versionName ? version.versionName : "?", render_width, render_height, output_width,
                output_height, (flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) ? ", HDR" : ", LDR",
                (flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) ? ", inverted depth" : "");
  }
  return true;
#else
  (void)render_width;
  (void)render_height;
  (void)output_width;
  (void)output_height;
  (void)flags;
  return false;
#endif
}

void Fsr::DestroyContext() {
#if NATIVE_X_RENDERER_FSR
  if (!context_) return;
  // FSR's own textures may still be in use by frames in flight.
  device_->IdleGPU();
  ffxContext context = context_;
  reinterpret_cast<PfnFfxDestroyContext>(destroy_context_)(&context, nullptr);
  context_ = nullptr;
#endif
}

bool Fsr::Dispatch(IDeviceContext* context, const Inputs& in) {
#if NATIVE_X_RENDERER_FSR
  if (!module_ || !in.color || !in.depth || !in.motion || !in.output) return false;
  const TextureDesc& color = in.color->GetDesc();
  const TextureDesc& output = in.output->GetDesc();
  if (SurfaceFormat(color.Format) == FFX_API_SURFACE_FORMAT_UNKNOWN ||
      SurfaceFormat(output.Format) == FFX_API_SURFACE_FORMAT_UNKNOWN) {
    REXLOG_ERROR("[native_x_renderer] FSR 3.1: a format it does not take (color {}, output {})", int(color.Format),
                 int(output.Format));
    return false;
  }
  // A float scene (before a game's post-processing) is HDR, without an exposure of its own; an 8- or 10-bit one is
  // gamma-encoded LDR.
  bool hdr = IsFloatFormat(color.Format);
  uint32_t flags = hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
                       : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
  if (in.inverted_depth) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
  bool reset = in.reset;
  if (!context_ || context_size_[0] != color.Width || context_size_[1] != color.Height ||
      context_size_[2] != output.Width || context_size_[3] != output.Height || context_flags_ != flags) {
    if (!CreateContext(color.Width, color.Height, output.Width, output.Height, flags)) return false;
    reset = true;
  }

  // The inputs read by compute shaders, the output written: DiligentCore puts them there, FSR is told so and puts
  // them back when it is done.
  StateTransitionDesc barriers[] = {
      {in.color, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE},
      {in.depth, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE},
      {in.motion, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE},
      {in.output, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE},
  };
  context->TransitionResourceStates(uint32_t(std::size(barriers)), barriers);
  RefCntAutoPtr<IDeviceContextVk> context_vk(context, IID_DeviceContextVk);
  if (!context_vk) return false;

  auto now = std::chrono::steady_clock::now();
  float frame_ms = last_dispatch_ == std::chrono::steady_clock::time_point{}
                       ? 16.7f
                       : std::chrono::duration<float, std::milli>(now - last_dispatch_).count();
  last_dispatch_ = now;

  ffxDispatchDescUpscale dispatch{};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  dispatch.commandList = context_vk->GetVkCommandBuffer();
  dispatch.color = Resource(in.color, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
  dispatch.depth = Resource(in.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
  dispatch.motionVectors = Resource(in.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
  dispatch.output = Resource(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, true);
  dispatch.jitterOffset = {in.jitter_x, in.jitter_y};
  dispatch.motionVectorScale = {1.0f, 1.0f};
  dispatch.renderSize = {color.Width, color.Height};
  dispatch.upscaleSize = {output.Width, output.Height};
  dispatch.enableSharpening = false;
  dispatch.sharpness = 0.0f;
  dispatch.frameTimeDelta = std::clamp(frame_ms, 1.0f, 100.0f);
  dispatch.preExposure = 1.0f;
  dispatch.reset = reset;
  dispatch.cameraNear = in.camera_near;
  dispatch.cameraFar = in.camera_far;
  dispatch.cameraFovAngleVertical = in.fov_y;
  dispatch.viewSpaceToMetersFactor = 1.0f;
  dispatch.flags = hdr ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
  ffxContext ffx = context_;
  ffxReturnCode_t result = reinterpret_cast<PfnFfxDispatch>(dispatch_)(&ffx, &dispatch.header);
  if (result != FFX_API_RETURN_OK) {
    REXLOG_ERROR("[native_x_renderer] FSR 3.1: the upscale failed (FidelityFX error {})", result);
    return false;
  }
  return true;
#else
  (void)context;
  (void)in;
  return false;
#endif
}

}  // namespace native_x_renderer
