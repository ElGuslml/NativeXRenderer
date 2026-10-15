// XenosNative - temporal antialiasing through NVIDIA DLSS (internal, Phase 5, D46, D47).
//
// DiligentCore's SuperResolution module drives NVIDIA's NGX SDK on the Vulkan device (D46: its DLSS provider,
// the NGX loader and the DLSS runtime from DiligentGraphics/DLSS-Headers, the runtime copied next to the
// game's executable). DLAA is DLSS at the render resolution, used as an antialiasing mode (D47):
//   - the jitter: each frame the game's main 3D view is drawn shifted by a sub-pixel offset (a Halton 2, 3
//     sequence of 64), through the host viewport of its draws (draw.cpp);
//   - the motion vectors: from the view's depth and its view-projection matrices this frame and the last
//     (the camera's motion), in a pixel pass; then the motion of the objects that moved on their own is drawn
//     over it (D48, motion.h);
//   - DLSS then takes the view's color (before the game's post-processing), depth, motion vectors and
//     jitter, and its result replaces the color, so the post-processing works on the antialiased image.
//
// Without DLSS in the build, or on a GPU without it, Initialize says so and returns false.

#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"

namespace Diligent {
struct ISuperResolutionFactory;
struct ISuperResolution;
}  // namespace Diligent

namespace xenos_native {

class Upscaler {
 public:
  Upscaler();
  ~Upscaler();

  // Finds DLSS on the device and sets up the motion vector pass. Logs the quality modes' input sizes for
  // an output of this size. False if DLSS is not available (logged).
  bool Initialize(Diligent::IRenderDevice* device, uint32_t output_width, uint32_t output_height);
  bool available() const { return available_; }

  // The jitter of this frame's main 3D view, in host pixels (-0.5 to 0.5). NextFrame moves to the next.
  float jitter_x() const { return jitter_[0]; }
  float jitter_y() const { return jitter_[1]; }
  void NextFrame();

  // DLAA on the main 3D view: `color` (rendered with this frame's jitter) is replaced by DLSS's result.
  // `depth` is the view's depth target, `view_projection` its matrix (16 floats, rows, as the game gives
  // them to its shaders, without jitter), and z_min, z_max the depth its viewport writes at the near and
  // far planes (reversed when z_min > z_max). `object_motion` draws the motion of the objects that moved on
  // their own into the render target view it gets, after the camera's motion (D48). False if it could not
  // run (logged once).
  bool Apply(Diligent::IDeviceContext* context, Diligent::ITexture* color, Diligent::ITexture* depth,
             Diligent::ITextureView* depth_srv, const std::array<float, 16>& view_projection, float z_min,
             float z_max, const std::function<void(Diligent::ITextureView*)>& object_motion);

  // Applications since the last call, for the log.
  uint32_t TakeApplied() {
    uint32_t applied = applied_;
    applied_ = 0;
    return applied;
  }

 private:
  bool EnsureResources(Diligent::ITexture* color, Diligent::ITexture* depth);

  Diligent::IRenderDevice* device_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::ISuperResolutionFactory> factory_;
  Diligent::RefCntAutoPtr<Diligent::ISuperResolution> dlss_;
  bool available_ = false;
  bool failed_ = false;
  std::array<uint8_t, 16> variant_{};  // DLSS's variant id (an INTERFACE_ID)

  // The motion vector pass: a full-screen triangle into motion_ (RG16F, host pixels).
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> motion_pso_;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> motion_srb_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> motion_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> output_;  // DLSS's result, copied over the color

  uint32_t frame_ = 0;
  float jitter_[2] = {};
  std::array<float, 16> last_view_projection_{};
  uint32_t last_applied_frame_ = UINT32_MAX;  // the frame DLAA last ran in: history is kept across one frame
  uint32_t applied_ = 0;
};

}  // namespace xenos_native
