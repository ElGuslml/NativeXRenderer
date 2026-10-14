// XenosNative - trace mode.
//
// With xenos_native_trace on, every hooked D3D call is counted per frame:
//   - "engine" calls come from game code, "internal" ones from inside another hooked call (Clear calls
//     ClearF, EndTiling calls Resolve…);
//   - calls from a thread other than the one that presents are counted apart.
// A summary of the last frame goes to the log every xenos_native_trace_interval frames. The bound key
// (F9 by default) or xenos_native_trace_dump_at dumps whole frames: one line per call, in order, with
// its arguments, for draws the state the draw will use, read from the device, and the return address
// in the caller ("from"), which finds the engine function behind a call.
//
// The trace is a library feature for bringing up a game (docs/integrating-a-game.md), off by default.
// When it is off the hooks never call into this file.

#include "trace.h"

#include "internal.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

REXCVAR_DEFINE_BOOL(xenos_native_trace, false, "XenosNative",
                    "Count the game's D3D calls per frame and log a summary; the trace key or "
                    "xenos_native_trace_dump_at dumps whole frames, call by call");
REXCVAR_DEFINE_INT32(xenos_native_trace_interval, 300, "XenosNative",
                     "Trace: log the call counts of one frame every this many frames (0 = never)");
REXCVAR_DEFINE_INT32(xenos_native_trace_dump_frames, 1, "XenosNative",
                     "Trace: how many consecutive frames one dump covers");
REXCVAR_DEFINE_INT32(xenos_native_trace_dump_at, 0, "XenosNative",
                     "Trace: dump the frame with this number without pressing the key (0 = off)");

namespace xenos_native::trace {
namespace {

using namespace internal;

const Profile* g_profile = nullptr;

// Counters of the frame in progress: [engine, internal, other thread].
std::array<std::array<std::atomic<uint32_t>, kCallCount>, 3> g_counts{};
std::atomic<uint32_t> g_device{0};
std::atomic<bool> g_device_changed{false};

// Dump state. g_dump_left > 0 while frames are being dumped; the lines are written to the log at the
// end of each dumped frame.
std::atomic<bool> g_dump_requested{false};
std::atomic<int32_t> g_dump_left{0};
std::mutex g_dump_mutex;
std::vector<std::string> g_dump_lines;
uint32_t g_dump_sequence = 0;

// A call in progress on this thread (hooked calls nest: Clear -> ClearF).
struct Pending {
  Call call;
  uint32_t r[8];  // r3..r10
  double f1;
  uint32_t write_before;
  uint32_t lr;  // the return address: the caller
  bool other_thread;
  std::string state;  // draws, while dumping
};
thread_local std::vector<Pending> t_stack;

// "dev=... prim=0x4 ..." from the argument list of calls.inc.
std::string FormatArguments(const CallInfo& info, const Pending& p) {
  std::string out;
  std::string_view names(info.arguments);
  int index = 0;
  while (!names.empty() && index < 8) {
    size_t end = names.find(' ');
    std::string_view name = names.substr(0, end);
    if (name == "dev") {
      // The device is the same in every call; the dump header prints it once.
    } else if (name.front() == '.') {
      if (!out.empty()) out += ' ';
      out += fmt::format("{}={}", name.substr(1), p.f1);
    } else {
      if (!out.empty()) out += ' ';
      out += fmt::format("{}={:#x}", name, p.r[index]);
    }
    ++index;
    if (end == std::string_view::npos) break;
    names.remove_prefix(end + 1);
  }
  return out;
}

// The state a draw uses, read from the device before the draw runs.
std::string DrawState(const uint8_t* base, uint32_t device) {
  const DeviceLayout& d = g_profile->device;
  auto at = [&](uint32_t offset) { return offset != kUnknown ? Load32(base, device + offset) : 0u; };
  std::string out = fmt::format("vs={:#x} ps={:#x} decl={:#x} ib={:#x} rt0={:#x} ds={:#x}", at(d.vertex_shader),
                                at(d.pixel_shader), at(d.vertex_declaration), at(d.index_buffer),
                                at(d.render_targets), at(d.depth_stencil));
  // RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO: the first three registers of the 0x2000 group.
  for (const auto& group : d.register_groups) {
    if (group.count && group.first_register == 0x2000) {
      out += fmt::format(" surface={:#x} color={:#x} depth={:#x}", Load32(base, device + group.offset),
                         Load32(base, device + group.offset + 4), Load32(base, device + group.offset + 8));
    }
  }
  if (d.predication_mask != kUnknown) out += fmt::format(" pred={:#x}", at(d.predication_mask));
  if (d.flags != kUnknown) out += fmt::format(" flags={:#010x}", at(d.flags));
  return out;
}

// Extra detail for a few calls, read while the call is still in progress.
std::string Detail(const uint8_t* base, const Pending& p) {
  auto surface = [&](uint32_t s) {
    // XDK surface header: +24 RB_SURFACE_INFO (pitch, MSAA), +28 RB_COLOR_INFO / RB_DEPTH_INFO.
    return s ? fmt::format(" [surface info={:#x} color/depth info={:#x}]", Load32(base, s + 24), Load32(base, s + 28))
             : std::string();
  };
  auto texture = [&](uint32_t t) {
    // XDK texture header: the 6-dword fetch constant at +28; dword 1 = base and format, 2 = size.
    return t ? fmt::format(" [fetch1={:#x} fetch2={:#x}]", Load32(base, t + 32), Load32(base, t + 36))
             : std::string();
  };
  auto rect = [&](uint32_t r) {
    return r ? fmt::format(" [rect {} {} {} {}]", Load32(base, r), Load32(base, r + 4), Load32(base, r + 8),
                           Load32(base, r + 12))
             : std::string();
  };
  switch (p.call) {
    case Call::kSetRenderTarget:
      return surface(p.r[2]);
    case Call::kSetDepthStencilSurface:
      return surface(p.r[1]);
    case Call::kSetTexture:
      return texture(p.r[2]);
    case Call::kResolve:
      return rect(p.r[2]) + texture(p.r[3]);
    case Call::kClearF:
      return rect(p.r[2]) + (p.r[3] ? fmt::format(" [color {} {} {} {}]", LoadFloat(base, p.r[3]),
                                                  LoadFloat(base, p.r[3] + 4), LoadFloat(base, p.r[3] + 8),
                                                  LoadFloat(base, p.r[3] + 12))
                                    : std::string());
    case Call::kBeginTiling: {
      std::string out;
      for (uint32_t i = 0; i < p.r[2] && i < 8 && p.r[3]; ++i) out += rect(p.r[3] + 16 * i);
      return out;
    }
    case Call::kEndTiling:
      return texture(p.r[3]);
    case Call::kVertexBufferLock:
    case Call::kVertexBufferUnlock:
    case Call::kIndexBufferLock:
    case Call::kIndexBufferUnlock:
    case Call::kTextureUnlock:
      // XDK resource header: the low bits of Common (+0) are the type (1 VB, 2 IB, 3 texture, 4 surface).
      return p.r[0] ? fmt::format(" [type {} base={:#x} size={:#x}]", Load32(base, p.r[0]) & 0xF,
                                  Load32(base, p.r[0] + 24), Load32(base, p.r[0] + 28))
                    : std::string();
    default:
      return {};
  }
}

void LogCounts(uint32_t frame, const std::array<std::array<uint32_t, kCallCount>, 3>& counts) {
  std::string line;
  for (size_t i = 0; i < kCallCount; ++i) {
    uint32_t engine = counts[0][i], internal = counts[1][i], other = counts[2][i];
    if (!engine && !internal && !other) continue;
    line += fmt::format(" {}={}", Info(Call(i)).name, engine);
    if (internal) line += fmt::format("+{}i", internal);
    if (other) line += fmt::format("+{}t", other);
  }
  REXLOG_INFO("[xenos_native] trace frame {} (engine+internal(i)+other threads(t)):{}", frame, line);
}

}  // namespace

void OnFrameEnd(uint32_t frame) {
  std::array<std::array<uint32_t, kCallCount>, 3> counts{};
  for (size_t k = 0; k < 3; ++k) {
    for (size_t i = 0; i < kCallCount; ++i) counts[k][i] = g_counts[k][i].exchange(0, std::memory_order_relaxed);
  }

  int32_t interval = REXCVAR_GET(xenos_native_trace_interval);
  bool dumping = g_dump_left.load(std::memory_order_relaxed) > 0;
  if (dumping || (interval > 0 && frame % uint32_t(interval) == 0)) LogCounts(frame, counts);
  if (g_device_changed.exchange(false)) {
    REXLOG_WARN("[xenos_native] trace: the device pointer changed, now {:#x}", g_device.load());
  }

  if (dumping) {
    std::vector<std::string> lines;
    {
      std::lock_guard lock(g_dump_mutex);
      lines.swap(g_dump_lines);
    }
    for (const auto& line : lines) REXLOG_INFO("[xenos_native] {}", line);
    REXLOG_INFO("[xenos_native] trace: end of dumped frame {} ({} calls)", frame, lines.size());
    g_dump_left.fetch_sub(1, std::memory_order_relaxed);
  }

  // Start a dump with the next frame.
  int32_t dump_at = REXCVAR_GET(xenos_native_trace_dump_at);
  bool at_frame = dump_at > 0 && frame + 1 == uint32_t(dump_at);
  if ((g_dump_requested.exchange(false) || at_frame) && g_dump_left.load() <= 0) {
    int32_t frames = std::max(1, REXCVAR_GET(xenos_native_trace_dump_frames));
    g_dump_sequence = 0;
    REXLOG_INFO("[xenos_native] trace: dumping frames {}..{} (device {:#x}, render thread {})", frame + 1,
                frame + frames, g_device.load(), CurrentThreadId());
    g_dump_left.store(frames, std::memory_order_relaxed);
  }
}

bool Initialize(const Profile& profile) {
  g_profile = &profile;
  if (!REXCVAR_GET(xenos_native_trace)) {
    return false;
  }
  rex::ui::RegisterBind("bind_xenos_native_trace", "F9", "XenosNative: dump the next frame's D3D calls",
                        [] { g_dump_requested.store(true); });
  REXLOG_INFO("[xenos_native] trace on: summary every {} frames, F9 (bind_xenos_native_trace) dumps {} frame(s){}",
              REXCVAR_GET(xenos_native_trace_interval), REXCVAR_GET(xenos_native_trace_dump_frames),
              REXCVAR_GET(xenos_native_trace_dump_at) > 0
                  ? fmt::format(", automatic dump at frame {}", REXCVAR_GET(xenos_native_trace_dump_at))
                  : std::string());
  return true;
}

void OnEnter(Call call, PPCContext& ctx, uint8_t* base, bool nested, bool other_thread) {
  const CallInfo& info = Info(call);
  g_counts[other_thread ? 2 : (nested ? 1 : 0)][size_t(call)].fetch_add(1, std::memory_order_relaxed);

  Pending& p = t_stack.emplace_back();
  p.call = call;
  p.r[0] = ctx.r3.u32, p.r[1] = ctx.r4.u32, p.r[2] = ctx.r5.u32, p.r[3] = ctx.r6.u32;
  p.r[4] = ctx.r7.u32, p.r[5] = ctx.r8.u32, p.r[6] = ctx.r9.u32, p.r[7] = ctx.r10.u32;
  p.f1 = ctx.f1.f64;
  p.write_before = 0;
  p.lr = uint32_t(ctx.lr);
  p.other_thread = other_thread;

  if (info.device) {
    uint32_t previous = g_device.exchange(p.r[0], std::memory_order_relaxed);
    if (previous && previous != p.r[0]) g_device_changed.store(true, std::memory_order_relaxed);
  }
  if (g_dump_left.load(std::memory_order_relaxed) > 0 && info.device) {
    const DeviceLayout& d = g_profile->device;
    if (d.command_buffer_write != kUnknown) p.write_before = Load32(base, p.r[0] + d.command_buffer_write);
    if (info.flags & kDraw) p.state = DrawState(base, p.r[0]);
  }
}

void OnLeave(Call call, PPCContext& ctx, uint8_t* base) {
  const CallInfo& info = Info(call);
  Pending p = std::move(t_stack.back());
  t_stack.pop_back();
  if (g_dump_left.load(std::memory_order_relaxed) <= 0) return;

  std::string line = fmt::format("{}{} {}", std::string(2 * t_stack.size(), ' '), info.name, FormatArguments(info, p));
  std::string detail = Detail(base, p);
  if (!detail.empty()) line += detail;
  if (!p.state.empty()) line += " | " + p.state;
  line += fmt::format(" -> {:#x} from {:#x}", ctx.r3.u32, p.lr);
  const DeviceLayout& d = g_profile->device;
  if (p.write_before && d.command_buffer_write != kUnknown) {
    uint32_t after = Load32(base, p.r[0] + d.command_buffer_write);
    line += fmt::format(" cb {:#x}..{:#x}", p.write_before, after);
  }
  if (p.other_thread) line += fmt::format(" thread={}", CurrentThreadId());
  std::lock_guard lock(g_dump_mutex);
  g_dump_lines.push_back(fmt::format("#{} {}", g_dump_sequence++, line));
}

}  // namespace xenos_native::trace
