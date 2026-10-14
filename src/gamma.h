// XenosNative - the guest's display gamma ramp, applied when presenting (internal, D36).
//
// The GPU's display controller maps every front buffer pixel through a gamma ramp the game sets (the
// DC_LUT registers): a 256-entry table for 8-bit front buffers, a piecewise linear ramp for 10-bit ones.
// The Xenos path's presenters apply it (the SDK's apply_gamma shaders), and so does XenosNative: the front
// buffer goes through the ramp at its own size into a 16-bit float texture, which Present then scales into
// the window. The command processor tracks the ramp from the register writes; XenosNative copies it every
// frame (IGraphicsSystem::GetGammaRamp, sdk_patches/0004) and updates its ramp texture when it changed.

#pragma once

#include <rex/system/interfaces/graphics.h>

#include <cstdint>
#include <optional>

#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"

namespace xenos_native {

class Blitter;

class DisplayGamma {
 public:
  // False if the graphics system cannot report the ramp (SDK patch 0004 missing): Present then shows the
  // front buffer as it is.
  bool Initialize(Diligent::IRenderDevice* device, rex::system::IGraphicsSystem* graphics);

  // The front buffer (`width` × `height`, read through the guest texture `swizzle`, of the guest texture
  // format `guest_format`) through the current ramp, as a texture of the same size. nullptr if off.
  Diligent::ITextureView* Apply(Diligent::IDeviceContext* context, Blitter& blit, Diligent::ITextureView* front,
                                uint32_t width, uint32_t height, uint32_t guest_format, uint32_t swizzle);

 private:
  void Update(Diligent::IDeviceContext* context);

  Diligent::IRenderDevice* device_ = nullptr;
  rex::system::IGraphicsSystem* graphics_ = nullptr;
  rex::system::GammaRamp ramp_{};
  std::optional<uint64_t> uploaded_version_;
  std::optional<uint32_t> logged_format_;
  // Row 0: the 256-entry table; rows 1-3: the piecewise linear ramp's red, green and blue entries.
  Diligent::RefCntAutoPtr<Diligent::ITexture> ramp_texture_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> target_;
};

}  // namespace xenos_native
