// XenosNative - trace mode (internal). See trace.cpp.

#pragma once

#include <xenos_native/xenos_native.h>

struct PPCContext;

namespace xenos_native::trace {

// Reads the trace cvars and registers the "trace a frame" key. Returns true if the trace is on, so
// the hooks must report the calls.
bool Initialize(const Profile& profile);

// From the hook dispatch (xenos_native.cpp). nested: inside another hooked call on this thread;
// other_thread: not the thread that presents.
void OnEnter(Call call, PPCContext& ctx, uint8_t* base, bool nested, bool other_thread);
void OnLeave(Call call, PPCContext& ctx, uint8_t* base);
void OnFrameEnd(uint32_t frame);

}  // namespace xenos_native::trace
