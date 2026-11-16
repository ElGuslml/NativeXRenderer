// NativeXRenderer - the host device: DiligentCore (Vulkan), its immediate context, and a swap chain on the
// game's window (internal).

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <cstdint>
#include <string>

#include "record_queue.h"
#include "DeviceContext.h"
#include "Fence.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "SwapChain.h"

namespace native_x_renderer {

// Whether the Vulkan device has sample rate shading enabled (DiligentCore enables it where the GPU has it,
// diligent_patches/0012): translated shaders then write a sample mask, which alpha to coverage needs (D138).
bool SampleRateShadingEnabled(Diligent::IRenderDevice* device);

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
  // Whether the game pipelines are linked from parts (graphics pipeline libraries, D132).
  bool pipeline_library() const { return pipeline_library_; }
  // The device's VkPhysicalDeviceLimits::maxViewportDimensions, host pixels (D134): 16384 on AMD, 32768 on NVIDIA.
  uint32_t max_viewport_width() const { return max_viewport_[0]; }
  uint32_t max_viewport_height() const { return max_viewport_[1]; }
  // The parts' counts since the start (D132): made, reused, pipelines linked, replaced by their whole pipeline, made
  // whole for want of parts.
  std::array<uint32_t, 5> PipelineLibraryStats() const;
  // The context, with every operation the record queue holds run first (the callers not using the queue yet).
  Diligent::IDeviceContext* context() const {
    if (queue_) queue_->Flush();
    return context_;
  }
  // The record stage's queue over the context (record_queue.h): the library's context calls go through it.
  RecordQueue& queue() const { return *queue_; }
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
  // `wait` false: without the wait (D154: the record thread presents, the renderer's thread waits with
  // WaitForExpectedFrames, counting the Presents it has prepared).
  double Present(bool wait = true);
  double WaitForExpectedFrames();

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
  uint64_t frames_presented() const { return frame_.load(std::memory_order_acquire); }
  // The frames presented once every Present prepared so far has run (D154: Present may run on the record thread, after
  // the renderer's thread has gone on): what the renderer's thread counts frames with (the texture cache's
  // submissions, the binding sets' and pipelines' last use). ExpectPresent: one more Present prepared.
  uint64_t frames_expected() const { return expected_.load(std::memory_order_relaxed); }
  void ExpectPresent() { expected_.fetch_add(1, std::memory_order_relaxed); }
  // Whether the window's client area is the swap chain's and not empty: Present needs no resize (UpdateSize) then.
  bool SizeUnchanged() const;
  uint64_t frames_completed() const { return frame_fence_->GetCompletedValue(); }

 private:
  // Waits until the GPU has finished the frame kMaxFramesInFlight frames back (Present's wait).
  double WaitForFramesInFlight();

  void* window_ = nullptr;
  bool dynamic_rendering_ = false;
  bool pipeline_library_ = false;
  uint32_t max_viewport_[2] = {16384, 16384};
  Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
  Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
  std::unique_ptr<RecordQueue> queue_;
  Diligent::RefCntAutoPtr<Diligent::ISwapChain> swap_chain_;
  Diligent::RefCntAutoPtr<Diligent::IFence> frame_fence_;
  std::atomic<uint64_t> frame_{0}, expected_{0};
  // native_x_renderer_present_mode as the swap chain was last given it.
  void ApplyPresentMode();
  std::string present_mode_;
  bool present_mode_applied_ = false;
  bool force_vsync_ = false;
};

}  // namespace native_x_renderer
