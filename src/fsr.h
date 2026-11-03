// NativeXRenderer - AMD FidelityFX Super Resolution 3.1 on DiligentCore's Vulkan device (internal, D116).
//
// The temporal upscaler beside DLSS (upscaler.h), for the GPUs DLSS does not run on: AMD, Intel, NVIDIA GTX. AMD's
// FidelityFX API: its headers (thirdparty/ffx_api, MIT) and its signed amd_fidelityfx_vk.dll from the FidelityFX SDK
// 1.1.4, fetched when configuring and copied next to the game's executable, loaded at run time. FSR records its compute
// passes into DiligentCore's command buffer; the inputs are put in the states it is told they are in, and it puts them
// back in those states when it is done (the FidelityFX Vulkan backend's UnregisterResourcesVK).

#pragma once

#include <chrono>
#include <cstdint>

#include "DeviceContext.h"
#include "RenderDevice.h"
#include "Texture.h"

namespace native_x_renderer {

class Fsr {
 public:
  ~Fsr();

  // Loads AMD's library from the executable's folder. False (logged) when it is missing or not in this build.
  bool Load(Diligent::IRenderDevice* device);

  // The input size FSR uses for an output and a quality mode (0 native AA, 1 quality, 2 balanced, 3 performance, 4
  // ultra performance: AMD's per-axis ratios 1, 1.5, 1.7, 2 and 3).
  static void RenderSize(uint32_t output_width, uint32_t output_height, int quality, uint32_t& width,
                         uint32_t& height);
  // AMD's jitter phase count for that pair of widths: 8 × the ratio squared.
  static uint32_t JitterPhases(uint32_t render_width, uint32_t output_width);

  struct Inputs {
    Diligent::ITexture* color = nullptr;   // the input size
    Diligent::ITexture* depth = nullptr;   // R32_FLOAT, the input size
    Diligent::ITexture* motion = nullptr;  // RG16F, input pixels, from this frame's position to the last's
    Diligent::ITexture* output = nullptr;  // the output size, an unordered access texture
    float jitter_x = 0.0f, jitter_y = 0.0f;  // input pixels, the picture's shift (x right, y down)
    bool reset = false;
    bool inverted_depth = false;  // 1 at the near plane, 0 at the far one
    float camera_near = 0.1f, camera_far = 1000.0f, fov_y = 1.0f;
  };
  // Records the upscale into `context`'s command buffer (no render pass open). FSR's context is made on the first call
  // and again when the sizes, the color's range or the depth's direction change. False when it failed (logged).
  bool Dispatch(Diligent::IDeviceContext* context, const Inputs& in);

 private:
  bool CreateContext(uint32_t render_width, uint32_t render_height, uint32_t output_width, uint32_t output_height,
                     uint32_t flags);
  void DestroyContext();

  Diligent::IRenderDevice* device_ = nullptr;
  void* module_ = nullptr;  // amd_fidelityfx_vk.dll (never unloaded)
  void* create_context_ = nullptr;
  void* destroy_context_ = nullptr;
  void* dispatch_ = nullptr;
  void* query_ = nullptr;
  void* context_ = nullptr;  // ffxContext
  uint32_t context_size_[4] = {};
  uint32_t context_flags_ = 0;
  std::chrono::steady_clock::time_point last_dispatch_{};
  bool logged_version_ = false;
};

}  // namespace native_x_renderer
