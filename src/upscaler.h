// XenosNative - temporal antialiasing and upscaling through NVIDIA DLSS (internal, Phase 5, D46).
//
// DiligentCore's SuperResolution module drives NVIDIA's NGX SDK on the Vulkan device (decision D46: its DLSS
// provider, the NGX loader and the DLSS runtime from DiligentGraphics/DLSS-Headers, the runtime copied next
// to the game's executable). DLSS needs, besides the color image: the depth buffer, per-pixel motion
// vectors and a sub-pixel jitter of the 3D views from frame to frame; those are built in the next steps
// of the plan (docs/plan.md, Phase 5). This first step finds DLSS on the device and reports what it
// offers: whether it runs (an RTX GPU with a recent driver) and the input sizes of its quality modes for
// the output.
//
// Without DLSS in the build (a platform DiligentCore does not support it on), Initialize says so and
// returns false.

#pragma once

#include <cstdint>

#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"

namespace Diligent {
struct ISuperResolutionFactory;
}

namespace xenos_native {

class Upscaler {
 public:
  Upscaler();
  ~Upscaler();

  // Finds DLSS on the device and logs its quality modes' input sizes for an output of this size. False if
  // DLSS is not available (logged).
  bool Initialize(Diligent::IRenderDevice* device, uint32_t output_width, uint32_t output_height);
  bool available() const { return available_; }

 private:
  Diligent::RefCntAutoPtr<Diligent::ISuperResolutionFactory> factory_;
  bool available_ = false;
};

}  // namespace xenos_native
