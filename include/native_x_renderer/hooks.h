// NativeXRenderer - hooks over a game's XDK Direct3D entry points.
//
// The game's hook file (compiled into the executable, never into this library: the generated
// functions are weak, and a strong definition inside an archive member is not guaranteed to be pulled
// in) has one line per entry point:
//
//   #include <native_x_renderer/hooks.h>
//   NATIVE_X_RENDERER_HOOK(824AC3C0, DrawIndexedVertices)
//   NATIVE_X_RENDERER_HOOK(822B5D58, Present)
//
// Every hook runs the original function. When no NativeXRenderer feature is on, a hook is one predictable
// branch plus the call to the original; when the trace is on, the library sees the call before and
// after the original runs.

#pragma once

#include <rex/hook.h>

#include <native_x_renderer/native_x_renderer.h>

namespace native_x_renderer::detail {

// True while a feature needs the hooks (the trace). Set once by Initialize(), before the guest starts.
extern bool g_hooks_active;

void Enter(Call call, PPCContext& ctx, uint8_t* base);
void Leave(Call call, PPCContext& ctx, uint8_t* base);

}  // namespace native_x_renderer::detail

#define NATIVE_X_RENDERER_HOOK(address, call)                                        \
  REX_EXTERN(__imp__sub_##address);                                             \
  REX_HOOK_RAW(sub_##address) {                                                 \
    if (!::native_x_renderer::detail::g_hooks_active) [[likely]] {                   \
      __imp__sub_##address(ctx, base);                                          \
      return;                                                                   \
    }                                                                           \
    ::native_x_renderer::detail::Enter(::native_x_renderer::Call::k##call, ctx, base);    \
    __imp__sub_##address(ctx, base);                                            \
    ::native_x_renderer::detail::Leave(::native_x_renderer::Call::k##call, ctx, base);    \
  }
