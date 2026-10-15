// NativeXRenderer - antialiasing of its own: SMAA or FXAA on the finished scene, before the HUD (internal, D41).
//
// NativeXRenderer does not bring over a game's MSAA (D31): it antialiases the finished image instead. The
// image must be taken after the game's post-processing and before its HUD is composited, or the HUD's text
// gets blurred. Where that is depends on the game, so the game's profile names the resolve that copies the
// finished scene (Profile::scene_resolve_caller); its result is antialiased in place, right after the copy,
// and everything that later samples it (the HUD composite) sees the antialiased scene.
//
// Two methods, chosen at startup (native_x_renderer_antialiasing):
//   - SMAA 1x, the reference implementation (thirdparty/smaa, MIT), preset High: luma edge detection,
//     blending weights from the precomputed area and search textures, neighborhood blending. Three passes.
//   - FXAA: the SDK's FXAA (its Vulkan presenter's, the normal quality), as a pixel shader with the luma
//     computed in place. One pass.
// Both work on the gamma-encoded image as the game wrote it, which is what they are designed for.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

namespace native_x_renderer {

class Antialiasing {
 public:
  enum class Mode { kOff, kFxaa, kSmaa };
  // "off", "fxaa" or "smaa"; anything else is SMAA (logged).
  static Mode ParseMode(std::string_view name);
  static const char* ModeName(Mode mode);

  // Compiles the method's shaders and creates its lookup textures. False if that failed (the image is then
  // not antialiased, logged).
  bool Initialize(Diligent::IRenderDevice* device, Mode mode);
  Mode mode() const { return mode_; }

  // Antialiases slice 0 of `texture` in place, through `target`, a render target view of that slice.
  void Apply(Diligent::IDeviceContext* context, Diligent::ITexture* texture, Diligent::ITextureView* target);

  // Applications since the last call, for the log.
  uint32_t TakeApplied() {
    uint32_t applied = applied_;
    applied_ = 0;
    return applied;
  }

 private:
  struct Pass {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  struct Input {
    const char* name;
    bool linear;  // the sampler's filter
  };
  // A full-screen pass: vertex and pixel shader (GLSL), the textures it samples, its target's format.
  bool CreatePass(Pass& pass, const char* name, const std::string& vertex, const std::string& pixel,
                  std::initializer_list<Input> inputs, Diligent::TEXTURE_FORMAT target_format);
  // The passes that write the output, per output format (created on first use).
  Pass* OutputPass(Diligent::TEXTURE_FORMAT format);
  bool EnsureTextures(uint32_t width, uint32_t height, Diligent::TEXTURE_FORMAT format);
  void Draw(Diligent::IDeviceContext* context, Pass& pass, Diligent::ITextureView* target, bool clear);

  Diligent::IRenderDevice* device_ = nullptr;
  Mode mode_ = Mode::kOff;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Pass smaa_edges_, smaa_weights_;
  std::map<Diligent::TEXTURE_FORMAT, Pass> output_passes_;  // SMAA neighborhood blending, or FXAA
  Diligent::RefCntAutoPtr<Diligent::ITexture> area_, search_;
  // At the image's size: a copy of the input, and SMAA's edges and blending weights.
  Diligent::RefCntAutoPtr<Diligent::ITexture> color_, edges_, weights_;
  uint32_t applied_ = 0;
};

}  // namespace native_x_renderer
