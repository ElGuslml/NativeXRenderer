// NativeXRenderer - the host device: DiligentCore (Vulkan), its immediate context, and a swap chain on the
// game's window (internal).

#pragma once

#include <cstdint>
#include <string>

#include "DeviceContext.h"
#include "Fence.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "SwapChain.h"

namespace native_x_renderer {

class Device {
 public:
  // Creates the Vulkan device and a swap chain on the window (HWND on Windows). Logs and returns false
  // on failure. `half_precision`: the device's 16-bit float shaders enabled where the GPU has them, for AMD's FSR
  // 3.1, which picks its 16-bit shaders by what the GPU supports (D116). Without a window, no swap chain: the
  // pipeline preparation tool (D129), with `compile_threads` for DiligentCore's pipeline compilation (0: the game's).
  bool Create(void* native_window, bool half_precision = false, uint32_t compile_threads = 0);

  Diligent::IRenderDevice* device() const { return device_; }
  // "<vendor> <device> <driver version> <pipeline cache UUID>" (hexadecimal) of the Vulkan device: what Vulkan's
  // pipeline caches are valid for (the preparation's stamp, D129). Empty if it cannot be read.
  std::string DriverIdentity() const;
  // Whether the device draws with dynamic rendering (D130).
  bool dynamic_rendering() const { return dynamic_rendering_; }
  Diligent::IDeviceContext* context() const { return context_; }
  Diligent::ISwapChain* swap_chain() const { return swap_chain_; }

  // Follows the window's client size. Returns false while the window has no area (minimized): skip
  // the frame then.
  bool UpdateSize();

  // Presents without waiting for vertical sync: the game paces itself (its present interval and the
  // SDK's vblank), and a second wait here would halve the frame rate. Then waits until the GPU has
  // finished the frame kMaxFramesInFlight frames back: without vertical sync DiligentCore's swap chain
  // (mailbox) never blocks, and a GPU slower than the game would fall further behind every frame, until
  // DiligentCore's dynamic heap fills up. Returns the time waited, in milliseconds.
  // The present modes are native_x_renderer_present_mode's (D106, D107: V-Sync is FIFO, which waits for the
  // display; the SDK's vblank_follows_display keeps the game from running ahead of it).
  double Present();

  // Ends a frame that is not shown, while the window has no area (minimized: Alt+Tab from exclusive fullscreen): its
  // work submitted and the frame finished as Present does, so DiligentCore releases the frame's dynamic memory and
  // stale resources, then the same wait for the GPU (D125). Returns the time waited, in milliseconds.
  double FinishHiddenFrame();

  static constexpr uint64_t kMaxFramesInFlight = 2;
  // V-Sync whatever native_x_renderer_present_mode says (frame generation, D123: FIFO shows the generated frame and
  // the game's a refresh apart; without it the game's would take the generated one's place).
  void ForceVsync(bool on) {
    force_vsync_ = on;
    present_mode_applied_ = false;
  }

  // For the log: the frames presented and completed (frame fence), and DiligentCore's command queue fence
  // (next and completed values), which its dynamic heap and queries wait on.
  struct FenceState {
    uint64_t frames_presented, frames_completed, queue_next, queue_completed;
  };
  FenceState GetFenceState();
  // Frames presented so far, and the last one the GPU finished.
  uint64_t frames_presented() const { return frame_; }
  uint64_t frames_completed() const { return frame_fence_->GetCompletedValue(); }

 private:
  // Waits until the GPU has finished the frame kMaxFramesInFlight frames back (Present's wait).
  double WaitForFramesInFlight();

  void* window_ = nullptr;
  bool dynamic_rendering_ = false;
  Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
  Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
  Diligent::RefCntAutoPtr<Diligent::ISwapChain> swap_chain_;
  Diligent::RefCntAutoPtr<Diligent::IFence> frame_fence_;
  uint64_t frame_ = 0;
  // native_x_renderer_present_mode as the swap chain was last given it.
  void ApplyPresentMode();
  std::string present_mode_;
  bool present_mode_applied_ = false;
  bool force_vsync_ = false;
};

}  // namespace native_x_renderer
