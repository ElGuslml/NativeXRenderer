// NativeXRenderer - see device.h.

#include "device.h"

#include <rex/logging.h>

#include <chrono>

#include "EngineFactoryVk.h"
#include "CommandQueue.h"
#include "Fence.h"
#include "NativeWindow.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace native_x_renderer {

using namespace Diligent;

namespace {

// DiligentCore's messages, in the game's log.
void DILIGENT_CALL_TYPE OnDiligentMessage(DEBUG_MESSAGE_SEVERITY severity, const Char* message, const Char* function,
                                          const Char* file, int line) {
  (void)file, (void)line;
  const char* text = message ? message : "";
  const char* where = function ? function : "";
  switch (severity) {
    case DEBUG_MESSAGE_SEVERITY_INFO:
      REXLOG_INFO("[native_x_renderer] DiligentCore: {}", text);
      break;
    case DEBUG_MESSAGE_SEVERITY_WARNING:
      REXLOG_WARN("[native_x_renderer] DiligentCore: {} ({})", text, where);
      break;
    default:
      REXLOG_ERROR("[native_x_renderer] DiligentCore: {} ({})", text, where);
      break;
  }
}

}  // namespace

bool Device::Create(void* native_window) {
  window_ = native_window;
  IEngineFactoryVk* factory = GetEngineFactoryVk();
  if (!factory) {
    REXLOG_ERROR("[native_x_renderer] no DiligentCore Vulkan factory");
    return false;
  }
  factory->SetMessageCallback(OnDiligentMessage);

  EngineVkCreateInfo create_info;
  create_info.EnableValidation = false;
  // What guest draws need: independent blending per render target, depth clamp (PA_CL_CLIP_CNTL
  // clip_disable), wireframe (polygon modes), BC textures. Optional ones are used when present.
  create_info.Features.IndependentBlend = DEVICE_FEATURE_STATE_ENABLED;
  create_info.Features.DepthClamp = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.DepthBiasClamp = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.WireframeFill = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.TextureCompressionBC = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.OcclusionQueries = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.Features.DurationQueries = DEVICE_FEATURE_STATE_OPTIONAL;  // GPU time per frame, for the log
  // Pipelines compile in DiligentCore's thread pool (D28). A few threads: new pipelines come in bursts
  // (a level, a new effect), and the game's own threads need the CPU.
  create_info.Features.AsyncShaderCompilation = DEVICE_FEATURE_STATE_OPTIONAL;
  create_info.NumAsyncShaderCompilationThreads = 4;
  // Every draw maps its constant buffers from the dynamic heap (system, bool/loop and fetch constants:
  // 1.4 KB, plus the float constants its shaders use), with several frames in flight.
  create_info.DynamicHeapSize = 256 << 20;
  create_info.DynamicHeapPageSize = 1 << 20;
  IDeviceContext* contexts[1] = {};
  factory->CreateDeviceAndContextsVk(create_info, &device_, contexts);
  context_.Attach(contexts[0]);
  if (!device_ || !context_) {
    REXLOG_ERROR("[native_x_renderer] could not create the Vulkan device");
    return false;
  }

  FenceDesc fence_desc;
  fence_desc.Name = "NativeXRenderer frames";
  fence_desc.Type = FENCE_TYPE_CPU_WAIT_ONLY;
  device_->CreateFence(fence_desc, &frame_fence_);
  if (!frame_fence_) {
    REXLOG_ERROR("[native_x_renderer] could not create the frame fence");
    return false;
  }

  SwapChainDesc swap_chain_desc;
  // The guest's front buffer is 8-bit RGBA, already gamma-corrected by the game: no sRGB conversion
  // on the way out.
  swap_chain_desc.ColorBufferFormat = TEX_FORMAT_RGBA8_UNORM;
  swap_chain_desc.DepthBufferFormat = TEX_FORMAT_UNKNOWN;
  swap_chain_desc.BufferCount = 3;
#ifdef _WIN32
  Win32NativeWindow window{static_cast<HWND>(native_window)};
#else
  NativeWindow window{};
#endif
  factory->CreateSwapChainVk(device_, context_, swap_chain_desc, window, &swap_chain_);
  if (!swap_chain_) {
    REXLOG_ERROR("[native_x_renderer] could not create the swap chain on the game window");
    return false;
  }

  const auto& adapter = device_->GetAdapterInfo();
  const auto& desc = swap_chain_->GetDesc();
  REXLOG_INFO("[native_x_renderer] Vulkan device on {}; swap chain {}x{}", adapter.Description, desc.Width,
              desc.Height);
  return true;
}

bool Device::UpdateSize() {
#ifdef _WIN32
  RECT rect{};
  if (!GetClientRect(static_cast<HWND>(window_), &rect)) return false;
  uint32_t width = uint32_t(rect.right - rect.left), height = uint32_t(rect.bottom - rect.top);
#else
  uint32_t width = swap_chain_->GetDesc().Width, height = swap_chain_->GetDesc().Height;
#endif
  if (!width || !height) return false;
  const auto& desc = swap_chain_->GetDesc();
  if (desc.Width != width || desc.Height != height) swap_chain_->Resize(width, height);
  return true;
}

Device::FenceState Device::GetFenceState() {
  FenceState state{frame_, frame_fence_->GetCompletedValue(), 0, 0};
  if (ICommandQueue* queue = context_->LockCommandQueue()) {
    state.queue_next = queue->GetNextFenceValue();
    state.queue_completed = queue->GetCompletedFenceValue();
    context_->UnlockCommandQueue();
  }
  return state;
}

double Device::Present() {
  context_->EnqueueSignal(frame_fence_, ++frame_);
  swap_chain_->Present(0);
  if (frame_ <= kMaxFramesInFlight) return 0.0;
  auto start = std::chrono::steady_clock::now();
  frame_fence_->Wait(frame_ - kMaxFramesInFlight);
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace native_x_renderer
