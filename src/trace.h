// NativeXRenderer - trace mode (internal). See trace.cpp.

#pragma once

#include <native_x_renderer/native_x_renderer.h>

struct PPCContext;

namespace native_x_renderer::trace {

// Reads the trace cvars and registers the "trace a frame" key. Returns true if the trace is on, so
// the hooks must report the calls.
bool Initialize(const Profile& profile);

// From the hook dispatch (native_x_renderer.cpp). nested: inside another hooked call on this thread;
// other_thread: not the thread that presents.
void OnEnter(Call call, PPCContext& ctx, uint8_t* base, bool nested, bool other_thread);
void OnLeave(Call call, PPCContext& ctx, uint8_t* base);
void OnFrameEnd(uint32_t frame);

}  // namespace native_x_renderer::trace
