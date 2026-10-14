// XenosNative - the renderer (internal). See renderer.cpp.

#pragma once

#include <filesystem>
#include <xenos_native/xenos_native.h>

struct PPCContext;

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system
namespace rex::ui {
class Window;
}  // namespace rex::ui
namespace xenos_native {
class StateTracker;
struct TrackedCall;
}  // namespace xenos_native

namespace xenos_native::renderer {

// Reads the xenos_native cvar. On: creates the host device on the game's window (detaching the SDK's
// presenter from it) and turns the SDK's host rendering off. Returns true if the renderer is on, so the
// hooks must report the calls.
bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window,
                const std::filesystem::path& cache_root);

// See xenos_native::AspectFactor and xenos_native::SetGameAspectBox.
double AspectFactor();
void SetGameAspectBox(bool on);

// From the hook dispatch (xenos_native.cpp), on the render thread. OnEnter: before the original runs;
// OnLeave: an outermost device call finished, and the tracker holds the state after it.
void OnEnter(Call call, PPCContext& ctx, uint8_t* base, bool nested);
void OnLeave(const TrackedCall& tracked, const StateTracker& tracker, PPCContext& ctx, uint8_t* base);
void OnFrameEnd(uint32_t frame);

}  // namespace xenos_native::renderer
