// NativeXRenderer - temporal antialiasing through NVIDIA DLSS or AMD FSR 3.1 (internal, Phase 5, D46, D47, D116).
//
// DiligentCore's SuperResolution module drives NVIDIA's NGX SDK on the Vulkan device (D46: its DLSS provider,
// the NGX loader and the DLSS runtime from DiligentGraphics/DLSS-Headers, the runtime copied next to the
// game's executable). DLAA is DLSS at the render resolution, used as an antialiasing mode (D47); the DLSS
// modes draw the main 3D view at a lower resolution, NGX's for the mode, and DLSS brings it to the render
// resolution (D49, targets.h):
//   - the jitter: each frame the game's main 3D view is drawn shifted by a sub-pixel offset (a Halton 2, 3
//     sequence of 64), through the host viewport of its draws (draw.cpp);
//   - the motion vectors: from the view's depth and its view-projection matrices this frame and the last
//     (the camera's motion), in a pixel pass; then the motion of the objects that moved on their own is drawn
//     over it (D48, motion.h);
//   - DLSS then takes the view's color (before the game's post-processing), depth, motion vectors and
//     jitter, and its result replaces the color (at the render resolution), so the post-processing works on
//     the antialiased image.
//
// AMD FidelityFX Super Resolution 3.1 (fsr.h, D116) is the other provider, for the GPUs DLSS does not run on: the same
// jitter, motion vectors and place in the frame, its native AA in place of DLAA and its quality modes (AMD's ratios) in
// place of NGX's. It also gets the view's depth as R32_FLOAT (a copy) and the camera's near and far planes and
// vertical field of view, found in the view-projection matrix.
//
// Without the provider in the build, or on a GPU without it, Initialize says so and returns false.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "camera_vote.h"

namespace Diligent {
struct ISuperResolutionFactory;
struct ISuperResolution;
}  // namespace Diligent

namespace native_x_renderer {

class Fsr;

class Upscaler {
 public:
  Upscaler();
  ~Upscaler();

  // DLAA, or DLSS from the input resolution NGX recommends for one of its quality modes (D49); with FSR 3.1, its
  // native AA and its quality modes (D116).
  enum class Mode { kDlaa, kQuality, kBalanced, kPerformance, kUltraPerformance };
  enum class Provider { kDlss, kFsr };
  static const char* ModeName(Provider provider, Mode mode);
  const char* Name() const { return ModeName(provider_, mode_); }

  // Finds the provider on the device and sets up the motion vector pass. Logs the quality modes' input sizes for
  // an output of this size. False if it is not available (logged).
  bool Initialize(Diligent::IRenderDevice* device, uint32_t output_width, uint32_t output_height, Mode mode,
                  Provider provider = Provider::kDlss);
  bool available() const { return available_; }
  Mode mode() const { return mode_; }
  Provider provider() const { return provider_; }
  // The resolution the main 3D view is drawn at: the output's for DLAA.
  uint32_t input_width() const { return input_size_[0]; }
  uint32_t input_height() const { return input_size_[1]; }
  bool upscales() const { return input_size_[0] != output_size_[0] || input_size_[1] != output_size_[1]; }

  // The jitter of this frame's main 3D view, in host pixels (-0.5 to 0.5). NextFrame moves to the next.
  float jitter_x() const { return jitter_[0]; }
  float jitter_y() const { return jitter_[1]; }
  void NextFrame();

  // DLSS on the main 3D view: `color` (rendered with this frame's jitter, at the input resolution) is
  // antialiased, and upscaled, into `output` (at the output resolution; `color` itself for DLAA).
  // `depth` is the view's depth target, `view_projection` its matrix (16 floats, rows, as the game gives
  // them to its shaders, without jitter), and z_min, z_max the depth its viewport writes at the near and
  // far planes (reversed when z_min > z_max). `object_motion` draws the motion of the objects that moved on
  // their own into the render target view it gets, after the camera's motion (D48). False if it could not
  // run (logged once).
  // `camera_reprojection`, when given: the camera's reprojection found by the camera vote (D118, column form: last
  // clip = r x this clip), used in place of the one view_projection gives; where both are there, the difference is
  // measured (last_vote_difference_px).
  bool Apply(Diligent::IDeviceContext* context, Diligent::ITexture* color, Diligent::ITexture* depth,
             Diligent::ITextureView* depth_srv, Diligent::ITexture* output,
             const std::array<float, 16>& view_projection, float z_min, float z_max,
             const std::function<void(Diligent::ITextureView*)>& object_motion,
             const Mat4d* camera_reprojection = nullptr);
  // The last Apply's largest difference, in output pixels, between the voted reprojection and the view matrix's, over
  // points across the screen and the depth range; negative when they were not both there.
  double last_vote_difference_px() const { return last_vote_difference_px_; }

  // Applications since the last call, for the log.
  uint32_t TakeApplied() {
    uint32_t applied = applied_;
    applied_ = 0;
    return applied;
  }

 private:
  bool EnsureResources(Diligent::ITexture* color, Diligent::ITexture* depth, Diligent::ITexture* output);
  bool InitializeDlss(uint32_t output_width, uint32_t output_height);
  bool InitializeFsr(uint32_t output_width, uint32_t output_height);
  // FSR 3.1: the view's depth copied as R32_FLOAT, then the upscale into output_ (D116).
  bool ApplyFsr(Diligent::IDeviceContext* context, Diligent::ITexture* color, Diligent::ITextureView* depth_srv,
                const std::array<float, 16>& view_projection, bool reset);

  Diligent::IRenderDevice* device_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::ISuperResolutionFactory> factory_;
  Diligent::RefCntAutoPtr<Diligent::ISuperResolution> dlss_;
  bool available_ = false;
  bool failed_ = false;
  std::array<uint8_t, 16> variant_{};  // DLSS's variant id (an INTERFACE_ID)
  Mode mode_ = Mode::kDlaa;
  Provider provider_ = Provider::kDlss;
  std::unique_ptr<Fsr> fsr_;
  // FSR 3.1: the depth copy pass, into depth_copy_ (R32_FLOAT at the input size).
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> depth_copy_pso_;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> depth_copy_srb_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> depth_copy_;
  uint8_t optimization_type_ = 0;  // DiligentCore's SUPER_RESOLUTION_OPTIMIZATION_TYPE for the mode
  uint32_t input_size_[2] = {}, output_size_[2] = {};

  // The motion vector pass: a full-screen triangle into motion_ (RG16F, host pixels).
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> motion_pso_;
  Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> motion_srb_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> motion_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> output_;  // DLSS's result, copied to the output

  uint32_t frame_ = 0;
  // The jitter's phases: at least the 8 per output pixel per input pixel NVIDIA recommends, 64 at least.
  uint32_t jitter_phases_ = 64;
  float jitter_[2] = {};
  std::array<float, 16> last_view_projection_{};
  uint32_t last_applied_frame_ = UINT32_MAX;  // the frame DLAA last ran in: history is kept across one frame
  uint32_t applied_ = 0;
  double last_vote_difference_px_ = -1.0;
};

}  // namespace native_x_renderer
