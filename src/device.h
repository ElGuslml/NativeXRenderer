// XenosNative - the host device: DiligentCore (Vulkan), its immediate context, and a swap chain on the
// game's window (internal).

#pragma once

#include <cstdint>

#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "SwapChain.h"

namespace xenos_native {

class Device {
 public:
  // Creates the Vulkan device and a swap chain on the window (HWND on Windows). Logs and returns false
  // on failure.
  bool Create(void* native_window);

  Diligent::IRenderDevice* device() const { return device_; }
  Diligent::IDeviceContext* context() const { return context_; }
  Diligent::ISwapChain* swap_chain() const { return swap_chain_; }

  // Follows the window's client size. Returns false while the window has no area (minimized): skip
  // the frame then.
  bool UpdateSize();

  // Presents without waiting for vertical sync: the game paces itself (its present interval and the
  // SDK's vblank), and a second wait here would halve the frame rate.
  void Present();

 private:
  void* window_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
  Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
  Diligent::RefCntAutoPtr<Diligent::ISwapChain> swap_chain_;
};

}  // namespace xenos_native
