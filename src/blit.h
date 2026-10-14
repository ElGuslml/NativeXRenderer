// XenosNative - copies between host textures with small utility pipelines (internal).
//
// Resolves copy a host render target into a host texture, with what the guest's copy does on the way:
// MSAA samples averaged or one sample taken, red and blue swapped, an exponent bias, depth into a color
// texture. Presenting scales the front buffer into the swap chain. All of them are one triangle that
// covers the destination rectangle, with a pixel shader that reads the source.

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

namespace xenos_native {

struct BlitRect {
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
};

struct BlitOptions {
  uint32_t samples = 1;    // of the source
  int32_t sample = -1;     // MSAA: the sample to take, -1 = the average
  bool swap_red_blue = false;
  float scale = 1.0f;      // the guest's exponent bias, as a factor
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
    kColorScaled,   // sampler2D, filtered: presenting
  };

  bool Initialize(Diligent::IRenderDevice* device);

  // Copies `from` (texels of the source) to `to` (texels of the target, same size unless the source is
  // kColorScaled).
  void Copy(Diligent::IDeviceContext* context, Diligent::ITextureView* source, Source kind, const Rect& from,
            Diligent::ITextureView* target, const Rect& to, const Options& options = {});

 private:
  struct Pipeline {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  Pipeline* GetPipeline(Source kind, Diligent::TEXTURE_FORMAT target_format);

  Diligent::IRenderDevice* device_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::IShader> vertex_shader_;
  std::map<Source, Diligent::RefCntAutoPtr<Diligent::IShader>> pixel_shaders_;
  std::map<std::tuple<Source, Diligent::TEXTURE_FORMAT>, Pipeline> pipelines_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
};

}  // namespace xenos_native
