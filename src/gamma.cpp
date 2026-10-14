// XenosNative - see gamma.h.

#include "gamma.h"

#include "blit.h"

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace xenos_native {

using namespace Diligent;
namespace xenos = rex::graphics::xenos;

bool DisplayGamma::Initialize(IRenderDevice* device, rex::system::IGraphicsSystem* graphics) {
  device_ = device;
  graphics_ = graphics;
  if (!graphics_ || !graphics_->GetGammaRamp(ramp_)) {
    REXLOG_WARN("[xenos_native] the graphics system does not report the display gamma ramp (SDK patch 0004 "
                "missing): presenting without it");
    graphics_ = nullptr;
    return false;
  }
  TextureDesc desc;
  desc.Name = "XenosNative display gamma ramp";
  desc.Type = RESOURCE_DIM_TEX_2D;
  desc.Width = 256;
  desc.Height = 4;
  desc.Format = TEX_FORMAT_R32_UINT;
  desc.Usage = USAGE_DEFAULT;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  device_->CreateTexture(desc, nullptr, &ramp_texture_);
  if (!ramp_texture_) graphics_ = nullptr;
  return graphics_ != nullptr;
}

void DisplayGamma::Update(IDeviceContext* context) {
  // A copy each frame (3 KB); the texture only when the guest wrote the ramp, and its content differs.
  rex::system::GammaRamp ramp;
  if (!graphics_->GetGammaRamp(ramp)) return;
  if (uploaded_version_ && *uploaded_version_ == ramp.version) return;
  bool changed = !uploaded_version_ || std::memcmp(ramp.table, ramp_.table, sizeof(ramp.table)) != 0 ||
                 std::memcmp(ramp.pwl, ramp_.pwl, sizeof(ramp.pwl)) != 0;
  ramp_ = ramp;
  uploaded_version_ = ramp.version;
  if (!changed) return;

  uint32_t texels[4][256] = {};
  std::memcpy(texels[0], ramp_.table, sizeof(ramp_.table));
  for (uint32_t i = 0; i < 128; ++i) {
    for (uint32_t channel = 0; channel < 3; ++channel) texels[1 + channel][i] = ramp_.pwl[i][channel];
  }
  Box box;
  box.MaxX = 256;
  box.MaxY = 4;
  TextureSubResData data;
  data.pData = texels;
  data.Stride = sizeof(texels[0]);
  context->UpdateTexture(ramp_texture_, 0, 0, box, data, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                         RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // For the log: both ramps' red channel at a few 8-bit levels, out of 1023 (a straight line is 0, 128,
  // 257, 514, 770, 1023).
  std::string table, pwl;
  for (uint32_t level : {0u, 32u, 64u, 128u, 192u, 255u}) {
    table += " " + std::to_string((ramp_.table[level] >> 20) & 0x3FF);
    uint32_t v = (level * 1023 + 127) / 255;
    uint32_t entry = ramp_.pwl[v >> 3][0];
    double value = (double(entry & 0xFFFF) + double((v & 7) * (entry >> 16)) / 8.0) / 65472.0;
    pwl += " " + std::to_string(uint32_t(std::min(value, 1.0) * 1023.0 + 0.5));
  }
  REXLOG_INFO("[xenos_native] display gamma ramp {} (version {}); red at 0, 32, 64, 128, 192, 255 of 255, out of "
              "1023: table{}, piecewise linear{}",
              ramp_.version ? "written by the guest" : "at its default", ramp_.version, table, pwl);
}

ITextureView* DisplayGamma::Apply(IDeviceContext* context, Blitter& blit, ITextureView* front, uint32_t width,
                                  uint32_t height, uint32_t guest_format, uint32_t swizzle) {
  if (!graphics_ || !front || !width || !height) return nullptr;
  Update(context);
  if (!target_ || target_->GetDesc().Width != width || target_->GetDesc().Height != height) {
    target_.Release();
    TextureDesc desc;
    desc.Name = "XenosNative front buffer after the gamma ramp";
    desc.Type = RESOURCE_DIM_TEX_2D;
    desc.Width = width;
    desc.Height = height;
    desc.Format = TEX_FORMAT_RGBA16_FLOAT;  // the ramp's 10 bits (or more, PWL) survive
    desc.Usage = USAGE_DEFAULT;
    desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    device_->CreateTexture(desc, nullptr, &target_);
    if (!target_) return nullptr;
  }
  // The SDK uses the table for 8-bit front buffers and the piecewise linear ramp for 10-bit ones
  // (D3D::InitializePresentationParameters sets up the ramp that way).
  auto format = xenos::TextureFormat(guest_format);
  Blitter::Options options;
  options.swizzle = swizzle;
  options.gamma_pwl =
      format == xenos::TextureFormat::k_2_10_10_10 || format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
  if (logged_format_ != guest_format) {
    logged_format_ = guest_format;
    REXLOG_INFO("[xenos_native] front buffer format {} ({}x{}): presented through the {}", guest_format, width,
                height, options.gamma_pwl ? "piecewise linear gamma ramp" : "256-entry gamma table");
  }
  options.ramp = ramp_texture_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* target = target_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  Blitter::Rect rect{0, 0, width, height};
  blit.Copy(context, front, Blitter::Source::kColorGamma, rect, target, rect, options);
  return target_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
}

}  // namespace xenos_native
