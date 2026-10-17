// NativeXRenderer - the guest GPU state, rebuilt from the D3D device and the D3D calls' own packets (internal).
//
// The rules (docs/xdk-d3d-reference.md, "Rebuilding a draw's GPU state"; decision D12), which the
// observer verifies at 100 %:
//   - render state: the device's register shadow after the draw call returns, except the registers the
//     library writes only into the command buffer (the program control registers, VGT_INDX_OFFSET): their
//     last value written there;
//   - fetch constants: the shadow, except dwords packets wrote outside the flush (inline draws, the
//     library's own clears and resolves), until the slot is flushed from the shadow again;
//   - float constants: dirty groups take the shadow's values at a draw; LOAD_ALU_CONSTANT (shader
//     literals) and the library's own draws overwrite them, and that persists;
//   - bool and loop constants: the shadow;
//   - the other registers (the resolve's copy registers, window offsets...): their last value written
//     in the command buffer by a hooked call;
//   - the shader programs: the last vertex and pixel microcode loaded (IM_LOAD, IM_LOAD_IMMEDIATE),
//     copied when the call that loads it returns: the GPU copies the microcode when it executes the
//     IM_LOAD, and the memory may be reused afterwards (it is, while a level loads).
//
// The tracker keeps one register file, updated by every outermost device call on the render thread. It
// lists the registers it writes, for a renderer that keeps a copy of the file on a thread of its own (D71). The
// packets are read right after the call wrote them, a few hundred bytes per draw: when the call returns,
// and also when it moves to a new command buffer segment (KickSegment, NewSegment), because the library
// may reuse a finished segment's memory before the call returns (D21), and because the call's packets
// continue somewhere else (D26). An inline draw's packets (BeginVertices) are read when EndVertices commits
// them, with the dirty masks the begin's flush saw.

#pragma once

#include <native_x_renderer/native_x_renderer.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rex::graphics {
class RegisterFile;
}  // namespace rex::graphics

namespace native_x_renderer {

// Whether the profile's device layout has everything the state tracker reads. If not, logs what is missing
// for `feature` (the observer, the renderer), which then stays off.
bool DeviceLayoutComplete(const DeviceLayout& d, const char* feature);

// What one call wrote into the command buffer, as far as the rebuild needs it.
struct PacketScan {
  // (register, value), in order: type-0/1 writes and SET_CONSTANT*, from the ring and from the indirect
  // buffers it calls. Bin-predicated packets (per tile) are not included.
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  // Register writes in bin-predicated packets (per tile: window offsets, the occlusion query's sample
  // count address), in order. Not applied to the register file.
  std::vector<std::pair<uint32_t, uint32_t>> predicated_writes;
  // LOAD_ALU_CONSTANT and friends: (register, value) read from memory when the packet was read.
  std::vector<std::pair<uint32_t, uint32_t>> loads;
  struct ShaderLoad {
    uint32_t type;     // 0 vertex, 1 pixel
    uint32_t address;  // CPU address of the microcode (in the command buffer for IM_LOAD_IMMEDIATE)
    uint32_t dwords;
    uint32_t packet;   // CPU address of the IM_LOAD packet
    bool immediate;    // IM_LOAD_IMMEDIATE
    uint32_t ucode_offset;  // where its microcode starts in shader_ucode
  };
  std::vector<ShaderLoad> shader_loads;
  // The shader loads' microcode, copied when the packets were read, big-endian, one after the other. One
  // buffer that keeps its capacity: no allocation per load (D68).
  std::vector<uint32_t> shader_ucode;
  uint32_t draws = 0, indirect = 0, predicated = 0;
  bool complete = true;  // every byte between the call's start and end was read as packets

  void Clear();
};

// One finished outermost device call.
struct TrackedCall {
  Call call{};
  bool draw = false;
  uint32_t device = 0;
  // GPU (physical) addresses of the command buffer the call wrote: [begin, end). A call that started a
  // new segment has begin > end, or a range much longer than its packets.
  uint32_t begin = 0, end = 0;
  const PacketScan* scan = nullptr;
};

class StateTracker {
 public:
  explicit StateTracker(const Profile& profile);
  ~StateTracker();

  // Forgets the model: the next call starts again from the shadow (literals are then unknown until the
  // shaders that use them are bound again: give the tracker a warm-up frame).
  void Reset();

  void Enter(Call call, uint32_t device, const uint8_t* base, bool nested);
  // The finished call if it was an outermost device call, else nullptr. Valid until the next Leave.
  const TrackedCall* Leave(Call call, const uint8_t* base, bool nested);

  // The GPU state after the last finished call. In a draw's Leave, the state that draw runs with.
  const rex::graphics::RegisterFile& registers() const { return *registers_; }
  const uint32_t* values() const;

  // The registers whose value changed since the last ClearChanges, in order, with repeats; all of them when
  // all_changed() (the first call, a reset): what a copy of the register file on another thread has to take
  // (D71).
  const std::vector<uint32_t>& changed() const { return changed_; }
  bool all_changed() const { return all_changed_; }
  void ClearChanges() {
    changed_.clear();
    all_changed_ = false;
  }

  // The programs the GPU runs: the last microcode loaded for each stage.
  struct Program {
    uint32_t address = 0;          // CPU address it was loaded from
    std::vector<uint32_t> ucode;   // a copy, big-endian as in guest memory
    uint64_t hash = 0;             // XXH3 of the microcode, computed once per load (ShaderCache's key)
    // Where it came from, for diagnostics.
    Call loaded_by{};
    uint32_t packet = 0;
    bool immediate = false;
  };
  const Program& vertex_program() const { return programs_[0] ? *programs_[0] : kNoProgram; }
  const Program& pixel_program() const { return programs_[1] ? *programs_[1] : kNoProgram; }
  // The same as shared, immutable copies, which stay valid while the tracker loads other programs (D71);
  // nullptr before the first load.
  const std::shared_ptr<const Program>& vertex_program_shared() const { return programs_[0]; }
  const std::shared_ptr<const Program>& pixel_program_shared() const { return programs_[1]; }
  static const Program kNoProgram;

 private:
  struct Pending {
    bool active = false;  // an outermost device call being tracked
    bool kick = false;    // a nested segment change (KickSegment, NewSegment) of the active call
    uint32_t device = 0;
    uint32_t begin = 0;      // CPU address of the call's first packet
    uint32_t scan_from = 0;  // CPU address of the first packet not read yet
    // The flush's dirty masks when the call started (DeviceLayout): float constant groups, fetch slots.
    uint64_t vs_dirty = 0;
    uint64_t ps_dirty = 0;
    uint64_t fetch_dirty = 0;
  };

  void Apply(const Pending& p, Call call, bool draw, const uint8_t* base);
  // Writes a register, noting it if its value changes (D71).
  void Set(uint32_t* r, uint32_t reg, uint32_t value) {
    if (r[reg] == value) return;
    r[reg] = value;
    changed_.push_back(reg);
  }

  const Profile& profile_;
  std::unique_ptr<rex::graphics::RegisterFile> registers_;
  std::vector<Pending> stack_;
  bool initialized_ = false;
  // The register groups as runs of consecutive registers, without the registers the library writes only into
  // the command buffer: what a draw copies from the shadow (D68).
  struct ShadowRun {
    uint32_t first_register, offset, count;
  };
  std::vector<ShadowRun> shadow_runs_;
  std::bitset<192> fetch_overridden_;
  std::array<uint32_t, 192> fetch_override_{};
  std::array<std::shared_ptr<const Program>, 2> programs_{};
  // Every program loaded so far, by the hash of its microcode: one shared copy each, loaded again without a copy.
  std::unordered_map<uint64_t, std::shared_ptr<const Program>> program_cache_;
  std::vector<uint32_t> changed_;
  bool all_changed_ = true;
  uint32_t last_device_ = 0;  // the device of the last device call (for kPackets calls)
  // The dirty masks of the last BeginVertices / BeginIndexedVertices, for the EndVertices that draws it.
  struct InlineBegin {
    bool pending = false;
    uint64_t vs_dirty = 0;
    uint64_t ps_dirty = 0;
    uint64_t fetch_dirty = 0;
  } inline_begin_;
  PacketScan scan_;
  TrackedCall last_;
};

}  // namespace native_x_renderer
