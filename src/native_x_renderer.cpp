// NativeXRenderer - see include/native_x_renderer/native_x_renderer.h.
//
// This file owns the hook dispatch: the game's hooks (hooks.h) call detail::Enter/Leave around the
// original function while a feature is on, and the features (trace.cpp, observer.cpp) see every call
// with whether it is nested in another hooked call, and the end of every frame.

#include <native_x_renderer/hooks.h>
#include <native_x_renderer/native_x_renderer.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <memory>
#include <string_view>

#include "internal.h"
#include "observer.h"
#include "renderer.h"
#include "state.h"
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

REXCVAR_DEFINE_BOOL(native_x_renderer_lean_setters, true, "NativeXRenderer",
                    "Follow the D3D state setters lean (D140): with the renderer alone, a setter's packets are read "
                    "only if it wrote some; off, every setter goes through the state tracker's full path");

namespace native_x_renderer {
namespace internal {

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
bool g_renderer = false;
// The setters followed lean (D140): with the renderer alone.
bool g_lean = false;
// The guest GPU state, for the observer and the renderer (state.h).
std::unique_ptr<StateTracker> g_tracker;
std::atomic<uint32_t> g_frame{0};
std::atomic<uint32_t> g_render_thread{0};  // the thread that presents
thread_local int t_depth = 0;
// Whether this thread's outermost call, and so the calls nested in it, goes to the state tracker and the
// renderer: a device call, or a call that writes into the device's command buffer (kPackets). The others
// (Lock, Unlock, the cache flush) are only traced: games make them from other threads too, while the
// render thread is inside a device call, and the tracker's call stack is not shared between threads (D61).
thread_local bool t_tracked = false;
// This thread's outermost call is a setter followed lean (D140): its device and where its packets would start.
thread_local bool t_lean = false;
thread_local uint32_t t_lean_device = 0, t_lean_begin = 0;

}  // namespace

const char* Describe() {
  return "NativeXRenderer 0.10 (--native_x_renderer draws with NativeXRenderer, the Xenos path otherwise; trace and observer)";
}

const char* CallName(Call call) {
  return size_t(call) < kCallCount ? internal::Info(call).name : "?";
}

void Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window,
                const std::filesystem::path& cache_root) {
  REXLOG_INFO("[native_x_renderer] profile: {} (XDK {})", profile.game, profile.xdk_version);
  g_trace = trace::Initialize(profile);
  g_observer = observer::Initialize(profile, graphics);
  g_renderer = renderer::Initialize(profile, graphics, window, cache_root);
  if (g_observer || g_renderer) g_tracker = std::make_unique<StateTracker>(profile);
  // The observer's verification wants every call; the renderer only those that changed what the tracker models.
  // The observer reads the tracker's register file; the renderer applies the tracker's records on its own thread.
  if (g_tracker) {
    g_tracker->set_skip_empty_calls(!g_observer);
    g_tracker->set_apply_locally(g_observer);
    g_tracker->set_keep_record(g_renderer);
  }
  g_lean = g_renderer && !g_trace && !g_observer && REXCVAR_GET(native_x_renderer_lean_setters);
  detail::g_hooks_active = g_trace || g_observer || g_renderer;
}

bool Renders() { return g_renderer; }

double AspectFactor() { return g_renderer ? renderer::AspectFactor() : 1.0; }

void SetGameAspectBox(bool on) {
  if (g_renderer) renderer::SetGameAspectBox(on);
}

void BeginView(const float* scene_view_projection) {
  if (g_renderer) renderer::BeginView(scene_view_projection);
}

void EndViewGeometry() {
  if (g_renderer) renderer::EndViewGeometry();
}

void ResetTemporalHistory() {
  if (g_renderer) renderer::ResetTemporalHistory();
}

namespace detail {

bool g_hooks_active = false;

void Enter(Call call, PPCContext& ctx, uint8_t* base) {
  if (t_depth == 0 && g_tracker) {
    // A game that commits its inline draws itself (an inlined EndVertices, D77): the draw its last BeginVertices
    // tracked is handed on as an EndVertices before the next device call, if the game committed it.
    const internal::CallInfo& info = internal::Info(call);
    if (info.device || (info.flags & internal::kPackets)) {
      if (const TrackedCall* tracked = g_tracker->TakeCommittedInlineDraw(base)) {
        uint64_t r3 = ctx.r3.u64;
        ctx.r3.u64 = tracked->device;
        if (g_trace) {
          trace::OnEnter(Call::kEndVertices, ctx, base, false, false);
          trace::OnLeave(Call::kEndVertices, ctx, base);
        }
        if (g_observer) observer::OnLeave(*tracked, *g_tracker);
        if (g_renderer) {
          renderer::OnEnter(Call::kEndVertices, ctx, base, false);
          renderer::OnLeave(*tracked, *g_tracker, ctx, base);
        }
        ctx.r3.u64 = r3;
      }
    }
  }
  bool nested = t_depth++ > 0;
  if (!nested) {
    const internal::CallInfo& info = internal::Info(call);
    t_tracked = info.device || (info.flags & internal::kPackets);
    // D140: a setter with the renderer alone: where its packets would start, nothing else.
    if (g_lean && (info.flags & internal::kSetter) && g_tracker) {
      t_lean = true;
      t_lean_device = ctx.r3.u32;
      t_lean_begin = g_tracker->EnterLean(t_lean_device, base);
      return;
    }
  } else if (t_lean) {
    // A call inside the lean setter (a segment change in its reservation): the setter becomes a tracked call first.
    t_lean = false;
    g_tracker->PromoteLean(t_lean_device, t_lean_begin);
  }
  if (g_trace) {
    uint32_t render = g_render_thread.load(std::memory_order_relaxed);
    trace::OnEnter(call, ctx, base, nested, render && render != internal::CurrentThreadId());
  }
  if (!t_tracked) return;
  if (g_tracker) g_tracker->Enter(call, ctx.r3.u32, base, nested);
  if (g_renderer) renderer::OnEnter(call, ctx, base, nested);
}

void Leave(Call call, PPCContext& ctx, uint8_t* base) {
  bool nested = --t_depth > 0;
  if (!nested && t_lean) {
    // D140: nothing to do unless the setter wrote packets; then they are read as a tracked call's.
    t_lean = false;
    if (const TrackedCall* tracked = g_tracker->LeaveLean(call, t_lean_device, t_lean_begin, base)) {
      if (g_renderer) renderer::OnLeave(*tracked, *g_tracker, ctx, base);
    }
    return;
  }
  if (g_trace) trace::OnLeave(call, ctx, base);
  if (g_tracker && t_tracked) {
    if (const TrackedCall* tracked = g_tracker->Leave(call, base, nested)) {
      if (g_observer) observer::OnLeave(*tracked, *g_tracker);
      if (g_renderer) renderer::OnLeave(*tracked, *g_tracker, ctx, base);
    }
  }
  if ((internal::Info(call).flags & internal::kFrameEnd) && !nested) {
    g_render_thread.store(internal::CurrentThreadId(), std::memory_order_relaxed);
    uint32_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_trace) trace::OnFrameEnd(frame);
    if (g_observer) observer::OnFrameEnd(frame);
    if (g_renderer) renderer::OnFrameEnd(frame);
  }
}

}  // namespace detail

}  // namespace native_x_renderer
