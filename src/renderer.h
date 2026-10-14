// XenosNative - the renderer (internal). See renderer.cpp.

#pragma once

#include <xenos_native/xenos_native.h>

struct PPCContext;

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system
namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace xenos_native::renderer {

// Reads the xenos_native cvar. On: creates the host device on the game's window (detaching the SDK's
// presenter from it) and turns the SDK's host rendering off. Returns true if the renderer is on, so the
// hooks must report the calls.
bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window);

// From the hook dispatch (xenos_native.cpp), on the render thread.
void OnEnter(Call call, PPCContext& ctx, uint8_t* base, bool nested);
void OnLeave(Call call, PPCContext& ctx, uint8_t* base, bool nested);
void OnFrameEnd(uint32_t frame);

}  // namespace xenos_native::renderer
