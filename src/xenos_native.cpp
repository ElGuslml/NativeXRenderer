// XenosNative - see include/xenos_native/xenos_native.h.
//
// This file owns the hook dispatch: the game's hooks (hooks.h) call detail::Enter/Leave around the
// original function while a feature is on, and the features (trace.cpp, observer.cpp) see every call
// with whether it is nested in another hooked call, and the end of every frame.

#include <xenos_native/hooks.h>
#include <xenos_native/xenos_native.h>

#include <rex/logging.h>

#include <atomic>
#include <string_view>

#include "internal.h"
#include "observer.h"
#include "trace.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xenos_native {
namespace internal {

namespace {
constexpr CallInfo kCalls[] = {
#define XENOS_NATIVE_CALL(id, arguments, flags) \
  {#id, arguments, flags, std::string_view(arguments).starts_with("dev")},
#include <xenos_native/calls.inc>
#undef XENOS_NATIVE_CALL
};
static_assert(std::size(kCalls) == kCallCount);
}  // namespace

const CallInfo& Info(Call call) { return kCalls[size_t(call)]; }

uint32_t CurrentThreadId() {
#ifdef _WIN32
  return GetCurrentThreadId();
#else
  return 0;
#endif
}

}  // namespace internal

namespace {

bool g_trace = false;
bool g_observer = false;
std::atomic<uint32_t> g_frame{0};
std::atomic<uint32_t> g_render_thread{0};  // the thread that presents
thread_local int t_depth = 0;

}  // namespace

const char* Describe() {
  return "XenosNative 0.2 (Phase 1: D3D trace and observer; the Xenos path draws)";
}

const char* CallName(Call call) {
  return size_t(call) < kCallCount ? internal::Info(call).name : "?";
}

void Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics) {
  REXLOG_INFO("[xenos_native] profile: {} (XDK {})", profile.game, profile.xdk_version);
  g_trace = trace::Initialize(profile);
  g_observer = observer::Initialize(profile, graphics);
  detail::g_hooks_active = g_trace || g_observer;
}

namespace detail {

bool g_hooks_active = false;

void Enter(Call call, PPCContext& ctx, uint8_t* base) {
  bool nested = t_depth++ > 0;
  if (g_trace) {
    uint32_t render = g_render_thread.load(std::memory_order_relaxed);
    trace::OnEnter(call, ctx, base, nested, render && render != internal::CurrentThreadId());
  }
  if (g_observer) observer::OnEnter(call, ctx, base, nested);
}

void Leave(Call call, PPCContext& ctx, uint8_t* base) {
  bool nested = --t_depth > 0;
  if (g_trace) trace::OnLeave(call, ctx, base);
  if (g_observer) observer::OnLeave(call, ctx, base, nested);
  if ((internal::Info(call).flags & internal::kFrameEnd) && !nested) {
    g_render_thread.store(internal::CurrentThreadId(), std::memory_order_relaxed);
    uint32_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_trace) trace::OnFrameEnd(frame);
    if (g_observer) observer::OnFrameEnd(frame);
  }
}

}  // namespace detail

}  // namespace xenos_native
