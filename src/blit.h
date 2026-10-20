// NativeXRenderer - copies between host textures with small utility pipelines (internal).
//
// Resolves copy a host render target into a host texture, with what the guest's copy does on the way:
// MSAA samples averaged or one sample taken, red and blue swapped, an exponent bias, depth into a color
// texture. From a target at the reduced scale (D49) they also scale: color filtered, depth nearest. The
// upscaled view's depth target is brought to the render scale by a copy into a depth target. Presenting
// maps the front buffer through the guest's gamma ramp (gamma.h), then scales it into the swap chain. All
// of them are one triangle that covers the destination rectangle, with a pixel shader that reads the
// source.

#pragma once

#include <cstdint>
#include <map>
#include <tuple>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "TextureView.h"

namespace native_x_renderer {

struct BlitRect {
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
};

struct BlitOptions {
  uint32_t samples = 1;    // of the source
  int32_t sample = -1;     // MSAA: the sample to take, -1 = the average
  bool swap_red_blue = false;
  float scale = 1.0f;      // the guest's exponent bias, as a factor
  // The source is a 2_10_10_10_FLOAT render target, kept in a wider host format: its values are brought to
  // what the guest's format holds before the copy, RGB within the 7e3 range (0 to 31.875) and alpha within
  // 0 to 1 at its 2 bits, as the EDRAM stores them.
  bool float_7e3 = false;
  // kColorScaled, kColorGamma: a guest texture swizzle (3 bits per component,
  // xenos::XE_GPU_TEXTURE_SWIZZLE_*), the default being RGBA.
  uint32_t swizzle = 0 | 1 << 3 | 2 << 6 | 3 << 9;
  // kColorGamma: the ramp texture (gamma.h), and which of its ramps to apply.
  Diligent::ITextureView* ramp = nullptr;
  bool gamma_pwl = false;
  // kDepthStencilPacked: the stencil view of the depth target, and whether its depth is float24 (20e4,
  // D24FS8) rather than unorm24.
  Diligent::ITextureView* stencil = nullptr;
  bool depth_float24 = false;
};

class Blitter {
 public:
  using Rect = BlitRect;
  using Options = BlitOptions;

  enum class Source : uint8_t {
    kColor,         // sampler2D, read texel by texel
    kColorMsaa,     // sampler2DMS: average the samples, or take one
    kDepth,         // a depth texture, written as the red channel
    kDepthMsaa,
    kColorScaled,   // sampler2D, filtered: presenting, and resolves from a target at another scale
    kColorGamma,    // sampler2D, read texel by texel through the guest's gamma ramp
    kDepthScaled,   // a depth texture to another size, nearest, written as the red channel
    kDepthToDepth,  // a depth texture to another size, nearest, into a depth target (its view): D49
    // A depth texture and its stencil, texel by texel, as the 32-bit words a resolve into a k_24_8 texture
    // writes to memory, in an RGBA8 target: R the stencil, G, B, A the depth's 24 bits, low first (D64).
    kDepthStencilPacked,
    // No source: a rectangle of a color target, or of a depth-stencil target, filled with a value (Fill*; D80).
    kFillColor,
    kFillDepthStencil,
  };

  bool Initialize(Diligent::IRenderDevice* device);

  // Copies `from` (texels of the source) to `to` (texels of the target, same size unless the source is
  // kColorScaled, kDepthScaled or kDepthToDepth).
  void Copy(Diligent::IDeviceContext* context, Diligent::ITextureView* source, Source kind, const Rect& from,
            Diligent::ITextureView* target, const Rect& to, const Options& options = {});

  // A clear of a rectangle (texels of the target), which DiligentCore's clears do not take (D80): a color target's
  // render target view filled with `color`; a depth-stencil view's depth and/or stencil with `depth`, `stencil`.
  void FillColor(Diligent::IDeviceContext* context, Diligent::ITextureView* target, const Rect& to,
                 const float color[4]);
  void FillDepthStencil(Diligent::IDeviceContext* context, Diligent::ITextureView* target, const Rect& to,
                        bool write_depth, float depth, bool write_stencil, uint8_t stencil);

 private:
  struct Pipeline {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  // variant: kFillDepthStencil's writes, bit 0 the depth, bit 1 the stencil.
  Pipeline* GetPipeline(Source kind, Diligent::TEXTURE_FORMAT target_format, uint8_t variant = 0);
  void Draw(Diligent::IDeviceContext* context, Pipeline* pipeline, Diligent::ITextureView* target, bool depth_target,
            const Rect& to);

  Diligent::IRenderDevice* device_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::IShader> vertex_shader_;
  std::map<Source, Diligent::RefCntAutoPtr<Diligent::IShader>> pixel_shaders_;
  std::map<std::tuple<Source, Diligent::TEXTURE_FORMAT, uint8_t>, Pipeline> pipelines_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
};

}  // namespace native_x_renderer
