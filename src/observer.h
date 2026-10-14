// XenosNative - observer (internal). See observer.cpp.

#pragma once

#include <xenos_native/xenos_native.h>

struct PPCContext;

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system

namespace xenos_native::observer {

// Reads the observer cvars and registers its key. Returns true if the observer is on, so the hooks
// must report the calls. graphics: where the draw observer is registered during a verification.
bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics);

// From the hook dispatch (xenos_native.cpp).
void OnEnter(Call call, PPCContext& ctx, uint8_t* base, bool nested);
void OnLeave(Call call, PPCContext& ctx, uint8_t* base, bool nested);
void OnFrameEnd(uint32_t frame);

}  // namespace xenos_native::observer
