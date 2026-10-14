// XenosNative - the renderer.
//
// With --xenos_native=true (read once, at startup), XenosNative draws the game instead of the SDK's
// Xenos emulation:
//   - the SDK's command processor keeps executing the game's command buffer for synchronization only
//     (xenos_native/sdk_patches/0002: fences, interrupts, swaps, queries with fake sample counts), and
//     draws nothing on the host;
//   - the SDK's presenter is detached from the game's window, and XenosNative presents there through
//     its own DiligentCore (Vulkan) swap chain. The SDK's ImGui overlays are not shown on this path yet.
//
// Phase 2 builds this up step by step (docs/plan.md). Current step: the device and the swap chain,
// presenting a test pattern at every Present.

#include "renderer.h"

#include "device.h"
#include "internal.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/window.h>

#include <cmath>
#include <memory>

REXCVAR_DEFINE_BOOL(xenos_native, false, "XenosNative",
                    "Draw the game with XenosNative instead of the SDK's Xenos emulation (read at startup; "
                    "Phase 2, in development)");

namespace xenos_native::renderer {
namespace {

using namespace internal;

const Profile* g_profile = nullptr;
std::unique_ptr<Device> g_device;
uint32_t g_frames_presented = 0;

}  // namespace

bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window) {
  g_profile = &profile;
  if (!REXCVAR_GET(xenos_native)) {
    return false;
  }
  void* native_window = window ? window->GetNativeWindowHandle() : nullptr;
  if (!graphics || !native_window) {
    REXLOG_ERROR("[xenos_native] renderer: needs the graphics system and the game's window; off");
    return false;
  }

  // The window can hold one swap chain: take it from the SDK's presenter.
  window->SetPresenter(nullptr);
  auto device = std::make_unique<Device>();
  if (!device->Create(native_window)) {
    REXLOG_ERROR("[xenos_native] renderer: no host device; the Xenos path draws");
    return false;
  }
  if (!graphics->SetHostRendering(false)) {
    REXLOG_ERROR("[xenos_native] renderer: the graphics system cannot turn host rendering off "
                 "(SDK patch missing?)");
    return false;
  }
  g_device = std::move(device);
  REXLOG_INFO("[xenos_native] renderer on: the SDK runs the command buffer for synchronization only");
  return true;
}

void OnEnter(Call call, PPCContext& ctx, uint8_t* base, bool nested) {
  (void)call, (void)ctx, (void)base, (void)nested;
}

void OnLeave(Call call, PPCContext& ctx, uint8_t* base, bool nested) {
  (void)call, (void)ctx, (void)base, (void)nested;
}

void OnFrameEnd(uint32_t frame) {
  if (!g_device || !g_device->UpdateSize()) return;
  auto* context = g_device->context();
  auto* rtv = g_device->swap_chain()->GetCurrentBackBufferRTV();
  context->SetRenderTargets(1, &rtv, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  // Test pattern: a color that cycles with the game's frames, so pacing and stalls are visible.
  float t = float(frame % 120) / 120.0f;
  const float color[4] = {0.1f, 0.15f + 0.35f * (0.5f + 0.5f * std::sin(6.2831853f * t)), 0.3f, 1.0f};
  context->ClearRenderTarget(rtv, color, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  g_device->Present();
  if (++g_frames_presented == 1) {
    REXLOG_INFO("[xenos_native] renderer: first frame presented (game frame {})", frame);
  }
}

}  // namespace xenos_native::renderer
