// NativeXRenderer - AMD FidelityFX frame generation (FSR 3.1's frame interpolation) on DiligentCore's Vulkan device
// (internal, D123).
//
// A frame between two of the game's, from the last frame presented and this one, the 3D view's depth and motion
// vectors (the temporal upscaler's, D47-D49) and FidelityFX's optical flow for the rest of the picture. FidelityFX's
// API is used without its frame interpolation swap chain (FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY): the
// frame is generated into a texture of ours, and the renderer presents it and then the game's frame through
// DiligentCore's swap chain, with its own present modes (renderer.cpp, ExecutePresent). AMD's amd_fidelityfx_vk.dll
// is the one FSR 3.1 loads (fsr.h); the passes are recorded into DiligentCore's command buffer.

#pragma once

#include <cstdint>

#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"

namespace native_x_renderer {

class FrameGeneration {
 public:
  // Loads AMD's library from the executable's folder. False (logged) when it is missing or not in this build.
  bool Load(Diligent::IRenderDevice* device);

  // This frame's 3D view, after the upscale.
  struct View {
    Diligent::ITexture* depth = nullptr;   // R32_FLOAT at the view's size, 0 at the near plane
    Diligent::ITexture* motion = nullptr;  // RG16F at the view's size, its pixels, from this frame's position to the last's
    float jitter_x = 0.0f, jitter_y = 0.0f;  // the view's pixels, the picture's shift
    float camera_near = 0.1f, camera_far = 1000.0f, fov_y = 1.0f;
    bool reset = false;  // the view's history starts again (a cut, or no view in the last frame)
  };
  // FidelityFX's prepare pass on the view, into `context`'s command buffer (no render pass open), for a picture of
  // `display_width` x `display_height` (the window's).
  void Prepare(Diligent::IDeviceContext* context, const View& view, uint32_t display_width, uint32_t display_height);

  // At Present, the frame composed into `current` (the window's size, RGBA8_UNORM): the frame between the last one
  // given here and it, into output(). `hudless`, when given: the same picture without the HUD (FidelityFX keeps the
  // HUD where the two differ instead of moving it with the scene). `area` is where the 3D view is in the picture (x, y,
  // width, height). False when there is none to show: this frame's view was not prepared, the last frame was not
  // given here, or the view was reset; `current` is still kept as the next frame's previous one when the view was
  // prepared.
  bool Generate(Diligent::IDeviceContext* context, Diligent::ITexture* current, Diligent::ITexture* hudless,
                const int32_t area[4]);
  Diligent::ITextureView* output_srv() const { return output_srv_; }
  // After the frame's presents.
  void EndFrame();

  // Frames generated and not, since the last call (for the log).
  struct Stats {
    uint32_t generated = 0, skipped = 0;
  };
  Stats TakeStats() {
    Stats stats = stats_;
    stats_ = {};
    return stats;
  }

 private:
  bool CreateContext(uint32_t display_width, uint32_t display_height, uint32_t render_width, uint32_t render_height);
  void DestroyContext();

  Diligent::IRenderDevice* device_ = nullptr;
  void* module_ = nullptr;  // amd_fidelityfx_vk.dll (never unloaded)
  void* create_context_ = nullptr;
  void* destroy_context_ = nullptr;
  void* configure_ = nullptr;
  void* dispatch_ = nullptr;
  void* query_ = nullptr;
  void* context_ = nullptr;  // ffxContext
  uint32_t context_size_[4] = {};  // display, then the view's
  bool logged_version_ = false;

  uint64_t frame_id_ = 1;
  bool prepared_ = false;          // this frame's view went through the prepare pass
  bool reset_ = false;             // and its history starts again
  bool last_generated_ = false;    // the last frame's picture was given to Generate (FidelityFX kept it)
  Diligent::RefCntAutoPtr<Diligent::ITexture> output_;
  Diligent::ITextureView* output_srv_ = nullptr;
  Stats stats_;
};

}  // namespace native_x_renderer
