// NativeXRenderer - the renderer (internal). See renderer.cpp.

#pragma once

#include <filesystem>
#include <native_x_renderer/native_x_renderer.h>

struct PPCContext;

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system
namespace rex::ui {
class Window;
}  // namespace rex::ui
namespace native_x_renderer {
class StateTracker;
struct TrackedCall;
}  // namespace native_x_renderer

namespace native_x_renderer::renderer {

// Reads the native_x_renderer cvar. On: creates the host device on the game's window (detaching the SDK's
// presenter from it) and turns the SDK's host rendering off. Returns true if the renderer is on, so the
// hooks must report the calls.
bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window,
                const std::filesystem::path& cache_root);

// See native_x_renderer::AspectFactor and native_x_renderer::SetGameAspectBox.
double AspectFactor();
void SetGameAspectBox(bool on);
// See native_x_renderer::BeginView and native_x_renderer::EndViewGeometry.
void BeginView(const float* scene_view_projection);
void EndViewGeometry();

// From the hook dispatch (native_x_renderer.cpp), on the render thread. OnEnter: before the original runs;
// OnLeave: an outermost device call finished, and the tracker holds the state after it.
void OnEnter(Call call, PPCContext& ctx, uint8_t* base, bool nested);
void OnLeave(const TrackedCall& tracked, const StateTracker& tracker, PPCContext& ctx, uint8_t* base);
void OnFrameEnd(uint32_t frame);

}  // namespace native_x_renderer::renderer
