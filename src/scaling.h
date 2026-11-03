// NativeXRenderer - the image scaled into the window by a spatial upscaler, and sharpened (internal, D115).
//
// Presenting scales the front buffer, after the guest's gamma ramp (D36), into the window; the internal resolution
// can be lower than the window's (D54). The Blitter's bilinear stretch does it by default. Chosen at startup
// (native_x_renderer_scaling, native_x_renderer_sharpness), these do better:
//   - AMD FidelityFX Super Resolution 1 (thirdparty/fsr1, MIT): EASU, the edge-adaptive upscale, in passes of at
//     most 2x per axis, then RCAS, its sharpening; RCAS alone at the same size;
//   - NVIDIA Image Scaling 1.0.3 (thirdparty/nis, MIT): NVScaler, a compute shader that upscales and sharpens, in
//     passes of at most 2x per axis (its range), sharpening in the last; NVSharpen at the same size;
//   - with the bilinear stretch, AMD FidelityFX CAS (thirdparty/cas, MIT) as sharpening: it scales too, up to 4x
//     the pixels, beyond which the image is stretched first.
// They read the image as the game composed it, HUD included, in the display's gamma, which is what they are made for.
// An image larger than the window (a smaller window) is still stretched: the upscalers do not shrink.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string_view>
#include <tuple>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

#include "blit.h"

namespace native_x_renderer {

class Scaling {
 public:
  enum class Filter { kBilinear, kFsr, kNis };
  // "bilinear", "fsr" or "nis"; anything else is bilinear (logged).
  static Filter ParseFilter(std::string_view name);
  static const char* FilterName(Filter filter);

  // `sharpness` 0 to 1, or negative for the filter's default: none with the bilinear stretch, RCAS at 0.2 stops
  // with FSR (AMD's), 0.5 with NIS (NVIDIA's). False if its shaders could not be made (the stretch then, logged).
  bool Initialize(Diligent::IRenderDevice* device, Filter filter, float sharpness);
  // Whether Apply does anything: a filter other than the stretch, or the stretch with sharpening.
  bool active() const { return active_; }

  // Scales `source` (its top-left `width` × `height`, slice 0) into `to` of `target`, filtered and sharpened. False
  // when it does not apply (a smaller rectangle, the same size without sharpening, a pass that could not be made):
  // the caller stretches the image itself.
  bool Apply(Diligent::IDeviceContext* context, Blitter& blit, Diligent::ITextureView* source, uint32_t width,
             uint32_t height, Diligent::ITextureView* target, const BlitRect& to);

  // Applications since the last call, for the log.
  uint32_t TakeApplied() {
    uint32_t applied = applied_;
    applied_ = 0;
    return applied;
  }

 private:
  enum class Pass : uint8_t { kEasu, kRcas, kCas, kNisScaler, kNisSharpen };
  struct Pipeline {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  struct Size {
    uint32_t width = 0, height = 0;
    bool operator==(const Size&) const = default;
  };

  // The pass's shader, compiled on first use (at Initialize for the filter's passes).
  Diligent::IShader* Shader(Pass pass);
  Pipeline* GetPipeline(Pass pass, Diligent::TEXTURE_FORMAT target_format);
  // Intermediate image `slot` (RGBA16F: render target, shader resource, storage), made again at another size.
  Diligent::ITexture* Intermediate(uint32_t slot, Size size);
  // A pixel pass of `source` (its top-left `in`) into `to` of `target`; `param` is the pass's sharpness.
  void DrawPass(Diligent::IDeviceContext* context, Pass pass, Diligent::ITextureView* source, Size in,
                Diligent::ITextureView* target, const BlitRect& to, float param);
  // NVScaler from `in` to `out`, or NVSharpen at `in` (`out` the same), into `target` (a storage image).
  void DispatchNis(Diligent::IDeviceContext* context, Diligent::ITextureView* source, Size in, Size out,
                   Diligent::ITexture* target, float sharpness);
  // The sizes of the passes from `in` to `out`, at most 2x per axis each; empty when an axis shrinks.
  static std::vector<Size> Steps(Size in, Size out);

  Diligent::IRenderDevice* device_ = nullptr;
  Filter filter_ = Filter::kBilinear;
  bool active_ = false;
  float rcas_stops_ = -1.0f;      // FSR: RCAS's sharpness in stops (0 the sharpest), negative: no RCAS
  float nis_sharpness_ = 0.5f;    // NIS: 0 to 1
  float cas_sharpness_ = -1.0f;   // the stretch: CAS's sharpness 0 to 1, negative: none
  uint32_t nis_block_width_ = 32, nis_block_height_[2] = {24, 32}, nis_threads_ = 128;  // [scaler, sharpen]
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_, nis_constants_;
  Diligent::RefCntAutoPtr<Diligent::ITexture> nis_coef_scaler_, nis_coef_usm_;
  Diligent::RefCntAutoPtr<Diligent::IShader> vertex_shader_;
  std::array<Diligent::RefCntAutoPtr<Diligent::IShader>, 5> shaders_;  // per Pass
  std::map<std::tuple<Pass, Diligent::TEXTURE_FORMAT>, Pipeline> pipelines_;
  std::array<Diligent::RefCntAutoPtr<Diligent::ITexture>, 3> intermediates_;  // a chain of passes takes them in turn
  uint32_t applied_ = 0;
  // The sizes last logged: a line when they change.
  bool logged_ = false;
  Size logged_in_, logged_out_;
};

}  // namespace native_x_renderer
