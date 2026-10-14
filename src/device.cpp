// XenosNative - see device.h.

#include "device.h"

#include <rex/logging.h>

#include "EngineFactoryVk.h"
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

namespace xenos_native {

using namespace Diligent;

bool Device::Create(void* native_window) {
  window_ = native_window;
  IEngineFactoryVk* factory = GetEngineFactoryVk();
  if (!factory) {
    REXLOG_ERROR("[xenos_native] no DiligentCore Vulkan factory");
    return false;
  }

  EngineVkCreateInfo create_info;
  create_info.EnableValidation = false;
  IDeviceContext* contexts[1] = {};
  factory->CreateDeviceAndContextsVk(create_info, &device_, contexts);
  context_.Attach(contexts[0]);
  if (!device_ || !context_) {
    REXLOG_ERROR("[xenos_native] could not create the Vulkan device");
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
    REXLOG_ERROR("[xenos_native] could not create the swap chain on the game window");
    return false;
  }

  const auto& adapter = device_->GetAdapterInfo();
  const auto& desc = swap_chain_->GetDesc();
  REXLOG_INFO("[xenos_native] Vulkan device on {}; swap chain {}x{}", adapter.Description, desc.Width,
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

void Device::Present() { swap_chain_->Present(0); }

}  // namespace xenos_native
