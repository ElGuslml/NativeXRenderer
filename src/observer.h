// XenosNative - observer (internal). See observer.cpp.

#pragma once

#include <xenos_native/xenos_native.h>

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system

namespace xenos_native {
class StateTracker;
struct TrackedCall;
}  // namespace xenos_native

namespace xenos_native::observer {

// Reads the observer cvars and registers its key. Returns true if the observer is on: the hooks must
// report the calls, and the state tracker must run. graphics: where the draw observer is registered
// during a verification.
bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics);

// From the hook dispatch (xenos_native.cpp): an outermost device call finished, and the tracker holds the
// state after it.
void OnLeave(const TrackedCall& tracked, const StateTracker& tracker);
void OnFrameEnd(uint32_t frame);

}  // namespace xenos_native::observer
