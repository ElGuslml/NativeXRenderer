// NativeXRenderer - observer (internal). See observer.cpp.

#pragma once

#include <native_x_renderer/native_x_renderer.h>

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system

namespace native_x_renderer {
class StateTracker;
struct TrackedCall;
}  // namespace native_x_renderer

namespace native_x_renderer::observer {

// Reads the observer cvars and registers its key. Returns true if the observer is on: the hooks must
// report the calls, and the state tracker must run. graphics: where the draw observer is registered
// during a verification.
bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics);

// From the hook dispatch (native_x_renderer.cpp): an outermost device call finished, and the tracker holds the
// state after it.
void OnLeave(const TrackedCall& tracked, const StateTracker& tracker);
void OnFrameEnd(uint32_t frame);

}  // namespace native_x_renderer::observer
