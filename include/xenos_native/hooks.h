// XenosNative - hooks over a game's XDK Direct3D entry points.
//
// The game's hook file (compiled into the executable, never into this library: the generated
// functions are weak, and a strong definition inside an archive member is not guaranteed to be pulled
// in) has one line per entry point:
//
//   #include <xenos_native/hooks.h>
//   XENOS_NATIVE_HOOK(824AC3C0, DrawIndexedVertices)
//   XENOS_NATIVE_HOOK(822B5D58, Present)
//
// Every hook runs the original function. When no XenosNative feature is on, a hook is one predictable
// branch plus the call to the original; when the trace is on, the library sees the call before and
// after the original runs.

#pragma once

#include <rex/hook.h>

#include <xenos_native/xenos_native.h>

namespace xenos_native::detail {

// True while a feature needs the hooks (the trace). Set once by Initialize(), before the guest starts.
extern bool g_hooks_active;

void Enter(Call call, PPCContext& ctx, uint8_t* base);
void Leave(Call call, PPCContext& ctx, uint8_t* base);

}  // namespace xenos_native::detail

#define XENOS_NATIVE_HOOK(address, call)                                        \
  REX_EXTERN(__imp__sub_##address);                                             \
  REX_HOOK_RAW(sub_##address) {                                                 \
    if (!::xenos_native::detail::g_hooks_active) [[likely]] {                   \
      __imp__sub_##address(ctx, base);                                          \
      return;                                                                   \
    }                                                                           \
    ::xenos_native::detail::Enter(::xenos_native::Call::k##call, ctx, base);    \
    __imp__sub_##address(ctx, base);                                            \
    ::xenos_native::detail::Leave(::xenos_native::Call::k##call, ctx, base);    \
  }
