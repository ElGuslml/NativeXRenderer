// XenosNative - observer: proves that the GPU state of every draw can be rebuilt from the D3D device.
//
// With xenos_native_verify on, the bound key (F10 by default) or xenos_native_verify_at verifies
// xenos_native_verify_frames frames:
//   - CPU side, when every outermost D3D draw returns: the state the tracker rebuilt (state.h, the rules
//     of decision D12), and the range of the command buffer the call wrote. Every other outermost call's
//     range is logged too.
//   - GPU side, through the SDK's draw observer (xenos_native/sdk_patches/0001): for every draw packet
//     the command processor executes, the snapshot whose range holds the packet, compared with the
//     register file. For the constants, only what the bound shaders read (the SDK's analysis); for
//     render state, the whole register shadow. A packet executed twice (predicated tiling) is also
//     compared with its first execution, which lists what differs per tile.
//   - Draw packets with no D3D draw behind them are attributed to the call whose range holds them
//     (ClearF, Resolve...), or to buffers outside the ring (prebuilt by the D3D library).
// A few frames later the observer is removed and the report goes to the log. The renderer uses the same
// tracker, so a clean report also means the renderer draws with the right state.

#include "observer.h"

#include "internal.h"
#include "state.h"

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/keybinds.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_BOOL(xenos_native_verify, false, "XenosNative",
                    "Observer: the verify key (or xenos_native_verify_at) compares the GPU state of every "
                    "draw with the D3D device's, through the SDK's draw observer");
REXCVAR_DEFINE_INT32(xenos_native_verify_frames, 1, "XenosNative", "Observer: how many frames one verification covers");
REXCVAR_DEFINE_INT32(xenos_native_verify_at, 0, "XenosNative",
                     "Observer: verify from the frame with this number without pressing the key (0 = off)");

namespace xenos_native::observer {
namespace {

using namespace internal;
using ConstantMap = rex::graphics::Shader::ConstantRegisterMap;

// Xenos register file.
constexpr uint32_t kFloatConstants = 0x4000;  // 512 vec4, the VS and PS windows set by SQ_VS/PS_CONST
constexpr uint32_t kFloatVec4 = 512;
constexpr uint32_t kFetchConstants = 0x4800;  // 32 slots x 6 dwords (or 96 vertex fetch x 2)
constexpr uint32_t kFetchDwords = 192;
constexpr uint32_t kBoolConstants = 0x4900;  // 256 bits
constexpr uint32_t kLoopConstants = 0x4908;  // 32 dwords
constexpr uint32_t kSqVsConst = 0x2307;
constexpr uint32_t kSqPsConst = 0x2308;
constexpr uint32_t kVgtDrawInitiator = 0x21FC;
constexpr uint32_t kRbModeControl = 0x2208;
constexpr uint32_t kTileStateFirst = 0x2000;  // kept from a packet's first execution, for tile diffs
constexpr uint32_t kTileStateEnd = 0x4928;

// Registers draws do not use, left out of the comparison: the resolve's copy registers, and the
// occlusion query's sample count address (written by Query Issue).
bool IsIgnoredRegister(uint32_t reg) { return (reg >= 0x2318 && reg <= 0x2323) || reg == 0x2325; }

// Frames to wait after the last verified frame before the report: the GPU runs behind the CPU.
constexpr uint32_t kGpuLagFrames = 8;

struct RegisterNameEntry {
  uint32_t index;
  const char* name;
};
constexpr RegisterNameEntry kRegisterNames[] = {
#define XE_GPU_REGISTER(index, type, name) {index, #name},
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
};

std::string RegisterName(uint32_t reg) {
  static const std::unordered_map<uint32_t, const char*> names = [] {
    std::unordered_map<uint32_t, const char*> m;
    for (const auto& e : kRegisterNames) m.emplace(e.index, e.name);
    return m;
  }();
  auto it = names.find(reg);
  return it != names.end() ? fmt::format("{}({:04X})", it->second, reg) : fmt::format("{:04X}", reg);
}

struct Snapshot {
  uint32_t sequence = 0;
  Call call{};
  uint32_t frame = 0;
  uint32_t begin = 0, end = 0;  // GPU addresses of the command buffer the call wrote
  std::vector<uint32_t> registers;  // expected register shadow groups, flattened in profile order
  std::array<uint32_t, kFetchDwords> fetch{};
  std::array<uint32_t, 4 * kFloatVec4> alu{};
  std::array<uint32_t, 40> bool_loop{};
  bool scan_truncated = false;  // the call's packets were not all read
  // The programs the tracker holds (hash of the copied microcode, as the SDK computes it; 0 = none), and
  // whether they were loaded inline (IM_LOAD_IMMEDIATE).
  std::array<uint64_t, 2> programs{};
  std::array<bool, 2> program_immediate{};
  uint32_t executions = 0;
  std::vector<uint32_t> first_execution;  // kTileStateFirst..kTileStateEnd of the first execution
};

struct Range {
  uint32_t begin, end;
  Call call;
};

struct Unmatched {
  uint32_t address;
  uint32_t initiator;
  uint32_t surface_info, color_info, depth_info, mode_control;
  uint64_t vertex_shader, pixel_shader;
  uint32_t after;  // external draws: the last draw packet run from the ring before this one
};

struct MismatchStat {
  uint32_t count = 0;
  uint32_t example_cpu = 0, example_gpu = 0, example_sequence = 0;
};

// Draws whose program (vertex, pixel) is not the microcode the command processor loaded.
struct ProgramMismatches {
  uint32_t count = 0, immediate = 0;
  std::set<std::pair<uint64_t, uint64_t>> pairs;  // (tracker, command processor), the first few
};

enum class State { kIdle, kCapturing, kWaiting };

// Where the GPU is relative to the verified frames. The command buffer is a ring: the frames after
// the verified ones reuse its addresses, so draw packets are matched only from the first one the GPU
// executes inside the verified range until the first one outside it.
enum class GpuPhase { kBefore, kInside, kAfter };

const Profile* g_profile = nullptr;
rex::system::IGraphicsSystem* g_graphics = nullptr;
std::atomic<bool> g_requested{false};

// Everything below is guarded by g_mutex: the render thread writes snapshots, the GPU thread reads them.
std::mutex g_mutex;
State g_state = State::kIdle;
uint32_t g_first_frame = 0, g_last_frame = 0, g_current_frame = 0;
uint32_t g_sequence = 0;
std::map<uint32_t, std::unique_ptr<Snapshot>> g_snapshots;  // by end address
std::vector<Range> g_ranges;
// The command buffer the verified frames wrote: contiguous segments of the ring (a new one when the
// write pointer jumps: ring wrap, new segment), and a wide region around them that stands for "the
// ring". Draw packets outside that region come from other buffers (prebuilt by the D3D library).
std::vector<std::pair<uint32_t, uint32_t>> g_segments;
uint32_t g_region_low = UINT32_MAX, g_region_high = 0;
constexpr uint32_t kSegmentGap = 0x10000;
constexpr uint32_t kRegionMargin = 0x800000;
GpuPhase g_gpu_phase = GpuPhase::kBefore;
uint64_t g_gpu_before = 0, g_gpu_after = 0;
uint32_t g_last_window_draw = 0;  // address of the last draw packet the GPU ran inside the window
std::vector<Unmatched> g_unmatched;
std::vector<Unmatched> g_external;
uint64_t g_gpu_draws = 0, g_matched_executions = 0, g_draws_clean = 0, g_unanalyzed = 0;
std::map<uint32_t, MismatchStat> g_register_mismatch, g_vertex_fetch_mismatch, g_texture_fetch_mismatch,
    g_vs_constant_mismatch, g_ps_constant_mismatch, g_bool_mismatch, g_loop_mismatch, g_tile_difference;
uint64_t g_vs_constants_compared = 0, g_ps_constants_compared = 0;
std::array<ProgramMismatches, 2> g_program_mismatch;
std::set<uint64_t> g_memexport_shaders;
std::set<std::pair<uint32_t, uint32_t>> g_constant_windows;  // SQ_VS_CONST, SQ_PS_CONST seen
std::vector<std::string> g_examples;
// Packet statistics over the verified frames.
uint64_t g_scanned_calls = 0, g_truncated_scans = 0, g_literal_loads = 0, g_indirect_buffers = 0;

void Record(std::map<uint32_t, MismatchStat>& stats, uint32_t key, uint32_t cpu, uint32_t gpu,
            uint32_t sequence) {
  MismatchStat& s = stats[key];
  if (!s.count++) s.example_cpu = cpu, s.example_gpu = gpu, s.example_sequence = sequence;
}

// The rebuilt state the GPU should have at a draw, copied from the tracker (render thread).
std::unique_ptr<Snapshot> BuildSnapshot(Call call, const StateTracker& tracker) {
  const uint32_t* r = tracker.values();
  auto s = std::make_unique<Snapshot>();
  s->call = call;
  for (const auto& g : g_profile->device.register_groups) {
    for (uint32_t i = 0; i < g.count; ++i) s->registers.push_back(r[g.first_register + i]);
  }
  std::copy_n(r + kFetchConstants, kFetchDwords, s->fetch.begin());
  std::copy_n(r + kFloatConstants, 4 * kFloatVec4, s->alu.begin());
  std::copy_n(r + kBoolConstants, 40, s->bool_loop.begin());
  for (int t = 0; t < 2; ++t) {
    const StateTracker::Program& program = t ? tracker.pixel_program() : tracker.vertex_program();
    s->programs[t] = program.ucode.empty() ? 0 : XXH3_64bits(program.ucode.data(), program.ucode.size() * 4);
    s->program_immediate[t] = program.immediate;
  }
  return s;
}

bool Bit(const uint64_t* bits, uint32_t i) { return (bits[i / 64] >> (i % 64)) & 1; }
bool Bit32(const uint32_t* bits, uint32_t i) { return (bits[i / 32] >> (i % 32)) & 1; }

// ---------------------------------------------------------------------------------------------------
// GPU side.

// GPU thread, with g_mutex held.
void Compare(Snapshot& s, const rex::system::GpuDrawInfo& info) {
  const uint32_t* r = info.registers;
  std::string first_problem;
  auto note = [&](const std::string& what) {
    if (first_problem.empty()) first_problem = what;
  };

  // Render state: the whole register shadow, with the packet registers.
  size_t k = 0;
  for (const auto& g : g_profile->device.register_groups) {
    for (uint32_t i = 0; i < g.count; ++i, ++k) {
      uint32_t reg = g.first_register + i;
      if (IsIgnoredRegister(reg)) continue;
      if (s.registers[k] != r[reg]) {
        Record(g_register_mismatch, reg, s.registers[k], r[reg], s.sequence);
        note(fmt::format("{} cpu={:#x} gpu={:#x}", RegisterName(reg), s.registers[k], r[reg]));
      }
    }
  }

  // Fetch constants: vertex fetch by the vertex shader's usage; texture fetch slots all (usage is not
  // reported by the SDK), kept apart.
  const auto* vs_map = static_cast<const ConstantMap*>(info.vertex_shader_constants);
  const auto* ps_map = static_cast<const ConstantMap*>(info.pixel_shader_constants);
  if (vs_map) {
    for (uint32_t i = 0; i < 96; ++i) {
      if (!Bit32(vs_map->vertex_fetch_bitmap, i)) continue;
      for (uint32_t j = 0; j < 2; ++j) {
        uint32_t dw = 2 * i + j;
        if (s.fetch[dw] != r[kFetchConstants + dw]) {
          Record(g_vertex_fetch_mismatch, i, s.fetch[dw], r[kFetchConstants + dw], s.sequence);
          note(fmt::format("vertex fetch {} dword {} cpu={:#x} gpu={:#x}", i, j, s.fetch[dw], r[kFetchConstants + dw]));
        }
      }
    }
  } else {
    ++g_unanalyzed;
  }
  for (uint32_t slot = 0; slot < 32; ++slot) {
    for (uint32_t j = 0; j < 6; ++j) {
      uint32_t dw = 6 * slot + j;
      if (s.fetch[dw] != r[kFetchConstants + dw]) {
        Record(g_texture_fetch_mismatch, slot, s.fetch[dw], r[kFetchConstants + dw], s.sequence);
        break;
      }
    }
  }

  // Float constants the shaders read, in the windows SQ_VS_CONST / SQ_PS_CONST select.
  uint32_t vs_base = r[kSqVsConst] & 0x1FF, ps_base = r[kSqPsConst] & 0x1FF;
  g_constant_windows.emplace(r[kSqVsConst], r[kSqPsConst]);
  auto compare_constants = [&](const ConstantMap* map, uint32_t window, std::map<uint32_t, MismatchStat>& stats,
                               uint64_t& compared, const char* stage) {
    if (!map) return;
    for (uint32_t c = 0; c < 256; ++c) {
      if (!map->float_dynamic_addressing && !Bit(map->float_bitmap, c)) continue;
      ++compared;
      uint32_t vec4 = (window + c) % kFloatVec4;
      for (uint32_t j = 0; j < 4; ++j) {
        uint32_t cpu = s.alu[4 * vec4 + j], gpu = r[kFloatConstants + 4 * vec4 + j];
        if (cpu != gpu) {
          Record(stats, c, cpu, gpu, s.sequence);
          note(fmt::format("{} c{}.{} cpu={:#x} gpu={:#x}", stage, c, "xyzw"[j], cpu, gpu));
          break;
        }
      }
    }
  };
  compare_constants(vs_map, vs_base, g_vs_constant_mismatch, g_vs_constants_compared, "VS");
  compare_constants(ps_map, ps_base, g_ps_constant_mismatch, g_ps_constants_compared, "PS");

  // Bool and loop constants the shaders read.
  for (const ConstantMap* map : {vs_map, ps_map}) {
    if (!map) continue;
    for (uint32_t b = 0; b < 256; ++b) {
      if (!Bit32(map->bool_bitmap, b)) continue;
      uint32_t cpu = (s.bool_loop[b / 32] >> (b % 32)) & 1, gpu = (r[kBoolConstants + b / 32] >> (b % 32)) & 1;
      if (cpu != gpu) {
        Record(g_bool_mismatch, b, cpu, gpu, s.sequence);
        note(fmt::format("bool {} cpu={} gpu={}", b, cpu, gpu));
      }
    }
    for (uint32_t l = 0; l < 32; ++l) {
      if (!((map->loop_bitmap >> l) & 1)) continue;
      if (s.bool_loop[8 + l] != r[kLoopConstants + l]) {
        Record(g_loop_mismatch, l, s.bool_loop[8 + l], r[kLoopConstants + l], s.sequence);
        note(fmt::format("loop {} cpu={:#x} gpu={:#x}", l, s.bool_loop[8 + l], r[kLoopConstants + l]));
      }
    }
  }

  // Programs: the microcode the tracker copied against the one the command processor loaded.
  const uint64_t gpu_programs[2] = {info.vertex_shader_hash, info.pixel_shader_hash};
  for (int t = 0; t < 2; ++t) {
    if (!gpu_programs[t] || s.programs[t] == gpu_programs[t]) continue;
    ProgramMismatches& m = g_program_mismatch[t];
    ++m.count;
    if (s.program_immediate[t]) ++m.immediate;
    if (m.pairs.size() < 8) m.pairs.emplace(s.programs[t], gpu_programs[t]);
    note(fmt::format("{} program cpu={:016x}{} gpu={:016x}", t ? "pixel" : "vertex", s.programs[t],
                     s.program_immediate[t] ? " (inline)" : "", gpu_programs[t]));
  }

  if (info.vertex_shader_memexport) g_memexport_shaders.insert(info.vertex_shader_hash);
  if (info.pixel_shader_memexport) g_memexport_shaders.insert(info.pixel_shader_hash);

  // A second execution of the same packet (another tile): what differs from the first.
  if (s.executions == 0) {
    s.first_execution.assign(r + kTileStateFirst, r + kTileStateEnd);
    if (first_problem.empty()) {
      ++g_draws_clean;
    } else if (g_examples.size() < 24) {
      g_examples.push_back(fmt::format("#{} {} (frame {}{}) vs={:016x} ps={:016x}: {}", s.sequence, CallName(s.call),
                                       s.frame, s.scan_truncated ? ", packets not fully read" : "",
                                       info.vertex_shader_hash, info.pixel_shader_hash, first_problem));
    }
  } else {
    for (uint32_t reg = kTileStateFirst; reg < kTileStateEnd; ++reg) {
      uint32_t first = s.first_execution[reg - kTileStateFirst];
      if (first != r[reg]) Record(g_tile_difference, reg, first, r[reg], s.sequence);
    }
  }
  ++s.executions;
}

bool InWindow(uint32_t address) {
  for (const auto& [begin, end] : g_segments) {
    if (address >= begin && address < end) return true;
  }
  return false;
}

void AddToWindow(uint32_t begin, uint32_t end) {
  // A call that started a new segment wrote its packets at the start of the new one.
  if (begin > end || end - begin > kSegmentGap) begin = end > kSegmentGap / 4 ? end - kSegmentGap / 4 : 0;
  if (!g_segments.empty() && begin >= g_segments.back().second && begin - g_segments.back().second < kSegmentGap) {
    g_segments.back().second = end;
  } else {
    g_segments.emplace_back(begin, end);
  }
  g_region_low = std::min(g_region_low, begin > kRegionMargin ? begin - kRegionMargin : 0);
  g_region_high = std::max(g_region_high, end + kRegionMargin);
}

void OnGpuDraw(void* /*user*/, const rex::system::GpuDrawInfo& info) {
  std::lock_guard lock(g_mutex);
  if (g_state == State::kIdle) return;
  uint32_t address = info.packet_address;
  const uint32_t* r = info.registers;
  if (address < g_region_low || address >= g_region_high) {
    // Another buffer, called from the ring (INDIRECT_BUFFER).
    if (g_gpu_phase == GpuPhase::kInside) {
      g_external.push_back({address, r[kVgtDrawInitiator], r[0x2000], r[0x2001], r[0x2002], r[kRbModeControl],
                            info.vertex_shader_hash, info.pixel_shader_hash, g_last_window_draw});
    }
    return;
  }
  bool inside = InWindow(address);
  switch (g_gpu_phase) {
    case GpuPhase::kBefore:
      if (!inside) {
        ++g_gpu_before;
        return;
      }
      g_gpu_phase = GpuPhase::kInside;
      break;
    case GpuPhase::kInside:
      if (!inside) {
        g_gpu_phase = GpuPhase::kAfter;
        ++g_gpu_after;
        return;
      }
      break;
    case GpuPhase::kAfter:
      ++g_gpu_after;
      return;
  }
  ++g_gpu_draws;
  g_last_window_draw = address;
  auto it = g_snapshots.upper_bound(address);
  if (it != g_snapshots.end()) {
    Snapshot& s = *it->second;
    // A call that started a new command buffer segment wrote its packets at the start of the new one.
    bool in_call = s.begin <= s.end ? address >= s.begin : address + 0x10000 >= s.end;
    if (in_call) {
      ++g_matched_executions;
      Compare(s, info);
      return;
    }
  }
  g_unmatched.push_back({address, r[kVgtDrawInitiator], r[0x2000], r[0x2001], r[0x2002], r[kRbModeControl],
                         info.vertex_shader_hash, info.pixel_shader_hash, 0});
}

// ---------------------------------------------------------------------------------------------------
// Report.

std::string Top(const std::map<uint32_t, MismatchStat>& stats, size_t limit,
                const std::function<std::string(uint32_t)>& name) {
  std::vector<std::pair<uint32_t, MismatchStat>> v(stats.begin(), stats.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
  std::string out;
  for (size_t i = 0; i < v.size() && i < limit; ++i) {
    out += fmt::format("\n    {} x{} (e.g. #{} cpu={:#x} gpu={:#x})", name(v[i].first), v[i].second.count,
                       v[i].second.example_sequence, v[i].second.example_cpu, v[i].second.example_gpu);
  }
  if (v.size() > limit) out += fmt::format("\n    ... {} more", v.size() - limit);
  return out.empty() ? " none" : out;
}

// Render thread, with g_mutex held.
void Report() {
  auto log = [](const std::string& s) { REXLOG_INFO("[xenos_native] verify: {}", s); };
  std::map<Call, uint32_t> cpu_draws;
  std::map<uint32_t, uint32_t> executions;
  for (const auto& [end, s] : g_snapshots) {
    ++cpu_draws[s->call];
    ++executions[s->executions];
  }
  std::string draws;
  for (const auto& [call, n] : cpu_draws) draws += fmt::format(" {}={}", CallName(call), n);
  std::string execs;
  for (const auto& [n, count] : executions) execs += fmt::format(" {}x:{}", n, count);
  log(fmt::format("frames {}..{}: CPU draw calls {}:{}; executions per call:{}", g_first_frame, g_last_frame,
                  g_snapshots.size(), draws, execs));
  uint64_t first_executions = 0;
  for (const auto& [n, count] : executions) first_executions += n ? count : 0;
  log(fmt::format("GPU draw packets of the verified frames {} (the GPU {} them; before: {}, after: {}): {} matched "
                  "to a D3D draw call; {} of {} draw calls fully match the rebuilt state ({:.1f} %); {} packets "
                  "not matched; shaders without analysis: {}",
                  g_gpu_draws, g_gpu_phase == GpuPhase::kAfter ? "finished" : "did NOT finish", g_gpu_before,
                  g_gpu_after, g_matched_executions, g_draws_clean, first_executions,
                  first_executions ? 100.0 * g_draws_clean / first_executions : 0.0, g_unmatched.size(), g_unanalyzed));
  log(fmt::format("packets read: {} calls ({} not fully), {} indirect buffers, {} literal vec4 loads",
                  g_scanned_calls, g_truncated_scans, g_indirect_buffers, g_literal_loads));

  // Every unmatched packet is inside the verified range: find the hooked call whose range holds it.
  std::sort(g_ranges.begin(), g_ranges.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
  std::map<std::string, uint32_t> by_call;
  std::vector<uint32_t> loose;
  for (const Unmatched& u : g_unmatched) {
    auto it = std::upper_bound(g_ranges.begin(), g_ranges.end(), u.address,
                               [](uint32_t a, const Range& r) { return a < r.begin; });
    std::string key;
    if (it != g_ranges.begin() && std::prev(it)->end > u.address) {
      key = fmt::format("{} (prim {} count {} surface {:#x} color {:#x} depth {:#x} mode {})", CallName(std::prev(it)->call),
                        u.initiator & 0x3F, u.initiator >> 16, u.surface_info, u.color_info, u.depth_info,
                        u.mode_control & 7);
    } else {
      if (loose.size() < 16) loose.push_back(u.address);
      key = "written outside any hooked call";
    }
    ++by_call[key];
  }
  std::string unmatched;
  for (const auto& [key, n] : by_call) unmatched += fmt::format("\n    {} x{}", key, n);
  log("unmatched draw packets by the call that wrote them:" + (unmatched.empty() ? std::string(" none") : unmatched));
  if (!loose.empty()) {
    std::string a;
    for (uint32_t x : loose) a += fmt::format(" {:#x}", x);
    log("  draw packets written outside any hooked call, first addresses:" + a);
  }
  // Draw packets from other buffers, by the call whose ring packets ran just before them.
  std::map<std::string, uint32_t> external;
  for (const Unmatched& u : g_external) {
    auto it = std::upper_bound(g_ranges.begin(), g_ranges.end(), u.after,
                               [](uint32_t a, const Range& r) { return a < r.begin; });
    std::string call = it != g_ranges.begin() && std::prev(it)->end > u.after ? CallName(std::prev(it)->call) : "?";
    ++external[fmt::format("after {}: at {:#x}.. prim {} count {} surface {:#x} color {:#x} depth {:#x} mode {} vs {:016x} ps {:016x}",
                           call, u.address & ~0xFFFu, u.initiator & 0x3F, u.initiator >> 16, u.surface_info,
                           u.color_info, u.depth_info, u.mode_control & 7, u.vertex_shader, u.pixel_shader)];
  }
  std::string ext;
  for (const auto& [key, n] : external) ext += fmt::format("\n    {} x{}", key, n);
  log(fmt::format("draw packets from buffers outside the command buffer ring: {}{}", g_external.size(), ext));

  log("render state mismatches:" + Top(g_register_mismatch, 24, RegisterName));
  log("vertex fetch mismatches (constants the VS reads):" +
      Top(g_vertex_fetch_mismatch, 12, [](uint32_t i) { return fmt::format("vfetch{}", i); }));
  log("texture fetch slot differences (all slots, read or not):" +
      Top(g_texture_fetch_mismatch, 12, [](uint32_t i) { return fmt::format("tfetch{}", i); }));
  log(fmt::format("VS float constant mismatches ({} compared):", g_vs_constants_compared) +
      Top(g_vs_constant_mismatch, 16, [](uint32_t c) { return fmt::format("c{}", c); }));
  log(fmt::format("PS float constant mismatches ({} compared):", g_ps_constants_compared) +
      Top(g_ps_constant_mismatch, 16, [](uint32_t c) { return fmt::format("c{}", c); }));
  for (int t = 0; t < 2; ++t) {
    const ProgramMismatches& m = g_program_mismatch[t];
    std::string pairs;
    for (const auto& [cpu, gpu] : m.pairs) pairs += fmt::format(" {:016x}/{:016x}", cpu, gpu);
    log(fmt::format("{} program mismatches (tracker's microcode vs the command processor's): {} draws, {} with an "
                    "inline program; tracker/CP hashes:{}",
                    t ? "pixel" : "vertex", m.count, m.immediate, pairs.empty() ? " none" : pairs));
  }
  log("bool constant mismatches:" + Top(g_bool_mismatch, 8, [](uint32_t b) { return fmt::format("b{}", b); }));
  log("loop constant mismatches:" + Top(g_loop_mismatch, 8, [](uint32_t l) { return fmt::format("i{}", l); }));
  std::string windows;
  for (const auto& [vs, ps] : g_constant_windows) windows += fmt::format(" (VS {:#x}, PS {:#x})", vs, ps);
  log("SQ_VS_CONST / SQ_PS_CONST seen:" + windows);
  log("registers that differ between executions of the same packet (tiles):" +
      Top(g_tile_difference, 32, RegisterName));
  std::string memexport;
  for (uint64_t h : g_memexport_shaders) memexport += fmt::format(" {:016x}", h);
  log("shaders with memexport:" + (memexport.empty() ? std::string(" none") : memexport));
  for (const auto& e : g_examples) log("  first mismatch of " + e);
}

void Reset() {
  g_snapshots.clear();
  g_ranges.clear();
  g_segments.clear();
  g_region_low = UINT32_MAX, g_region_high = 0;
  g_external.clear();
  g_last_window_draw = 0;
  g_gpu_phase = GpuPhase::kBefore;
  g_gpu_before = g_gpu_after = 0;
  g_unmatched.clear();
  g_gpu_draws = g_matched_executions = g_draws_clean = g_unanalyzed = 0;
  g_vs_constants_compared = g_ps_constants_compared = 0;
  g_program_mismatch = {};
  for (auto* m : {&g_register_mismatch, &g_vertex_fetch_mismatch, &g_texture_fetch_mismatch, &g_vs_constant_mismatch,
                  &g_ps_constant_mismatch, &g_bool_mismatch, &g_loop_mismatch, &g_tile_difference}) {
    m->clear();
  }
  g_memexport_shaders.clear();
  g_constant_windows.clear();
  g_examples.clear();
  g_sequence = 0;
  g_scanned_calls = g_truncated_scans = g_literal_loads = g_indirect_buffers = 0;
}

}  // namespace

bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics) {
  g_profile = &profile;
  g_graphics = graphics;
  if (!REXCVAR_GET(xenos_native_verify)) {
    return false;
  }
  const DeviceLayout& d = profile.device;
  if (!graphics || d.command_buffer_write == kUnknown || d.dirty_masks == kUnknown || d.fetch_constants == kUnknown ||
      d.vs_float_constants == kUnknown || d.ps_float_constants == kUnknown || d.bool_loop_constants == kUnknown) {
    REXLOG_WARN("[xenos_native] verify: needs the graphics system and the device layout; off");
    return false;
  }
  rex::ui::RegisterBind("bind_xenos_native_verify", "F10", "XenosNative: verify the next frames' draws",
                        [] { g_requested.store(true); });
  REXLOG_INFO("[xenos_native] verify on: F10 (bind_xenos_native_verify) verifies {} frame(s){}",
              REXCVAR_GET(xenos_native_verify_frames),
              REXCVAR_GET(xenos_native_verify_at) > 0
                  ? fmt::format(", automatically from frame {}", REXCVAR_GET(xenos_native_verify_at))
                  : std::string());
  return true;
}

void OnLeave(const TrackedCall& tracked, const StateTracker& tracker) {
  std::lock_guard lock(g_mutex);
  if (g_state != State::kCapturing) return;
  ++g_scanned_calls;
  if (!tracked.scan->complete) ++g_truncated_scans;
  g_indirect_buffers += tracked.scan->indirect;
  g_literal_loads += tracked.scan->loads.size() / 4;
  g_ranges.push_back({tracked.begin, tracked.end, tracked.call});
  AddToWindow(tracked.begin, tracked.end);
  if (tracked.draw) {
    auto s = BuildSnapshot(tracked.call, tracker);
    s->sequence = g_sequence++;
    s->frame = g_current_frame;
    s->begin = tracked.begin;
    s->end = tracked.end;
    s->scan_truncated = !tracked.scan->complete;
    g_snapshots[tracked.end] = std::move(s);
  }
}

void OnFrameEnd(uint32_t frame) {
  State state;
  {
    std::lock_guard lock(g_mutex);
    g_current_frame = frame + 1;
    state = g_state;
    if (state == State::kCapturing && frame >= g_last_frame) {
      g_state = State::kWaiting;
      return;
    }
  }
  if (state == State::kWaiting && frame >= g_last_frame + kGpuLagFrames) {
    g_graphics->SetDrawObserver(nullptr, nullptr);
    std::lock_guard lock(g_mutex);
    Report();
    Reset();
    g_state = State::kIdle;
    return;
  }
  if (state != State::kIdle) return;

  int32_t at = REXCVAR_GET(xenos_native_verify_at);
  bool at_frame = at > 0 && frame + 1 == uint32_t(at);
  if (!g_requested.exchange(false) && !at_frame) return;
  uint32_t frames = uint32_t(std::max(1, REXCVAR_GET(xenos_native_verify_frames)));
  {
    std::lock_guard lock(g_mutex);
    Reset();
    g_first_frame = frame + 1;
    g_last_frame = frame + frames;
    g_state = State::kCapturing;
  }
  if (!g_graphics->SetDrawObserver(&OnGpuDraw, nullptr)) {
    REXLOG_WARN("[xenos_native] verify: the graphics system has no draw observer (SDK patch missing?)");
    std::lock_guard lock(g_mutex);
    g_state = State::kIdle;
    return;
  }
  REXLOG_INFO("[xenos_native] verify: capturing frames {}..{}", g_first_frame, g_last_frame);
}

}  // namespace xenos_native::observer
