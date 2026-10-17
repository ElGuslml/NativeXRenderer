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
// The tracker records the registers every outermost device call on the render thread changes (RegisterRecord):
// it compares the shadow with an image of the file the records build, and the renderer writes them into its own
// file on its own thread (D73). With set_apply_locally the tracker keeps such a file too (the observer). The
// packets are read right after the call wrote them, a few hundred bytes per draw: when the call returns,
// and also when it moves to a new command buffer segment (KickSegment, NewSegment), because the library
// may reuse a finished segment's memory before the call returns (D21), and because the call's packets
// continue somewhere else (D26). An inline draw's packets (BeginVertices) are read when EndVertices commits
// them, with the dirty masks the begin's flush saw.

#pragma once

#include <native_x_renderer/native_x_renderer.h>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rex::graphics {
class RegisterFile;
}  // namespace rex::graphics

namespace native_x_renderer {

// The registers that tracked calls changed, gathered on the game's thread and written into a register file on
// the renderer's (D73): runs of registers, each a header dword (kind << 24 | count), its first register and
// `count` values. The shadow's dwords travel as the guest stores them (big-endian) and are swapped where they are
// applied.
struct RegisterRecord {
  enum Kind : uint32_t {
    kShadow = 1,  // big-endian, from the shadow
    kValues,      // host values, from packets
  };
  std::vector<uint32_t> data;

  void Shadow(uint32_t first_register, const uint32_t* source, uint32_t count) {
    data.push_back((uint32_t(kShadow) << 24) | count);
    data.push_back(first_register);
    data.insert(data.end(), source, source + count);
  }
  // Joins the kValues run it continues when that is the last one.
  void Value(uint32_t reg, uint32_t value);
  void Clear() {
    data.clear();
    values_at_ = SIZE_MAX;
  }

 private:
  friend class StateTracker;
  size_t values_at_ = SIZE_MAX;  // the header of the last run, if it is a kValues run
};

// Writes a record into a register file.
void ApplyRecord(const uint32_t* record, size_t size, uint32_t* registers);

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
    uint32_t ucode_offset;  // where its microcode starts in shader_ucode; kInGuestMemory: at `address` (D72)
  };
  std::vector<ShaderLoad> shader_loads;
  // The shader loads' microcode, copied when the packets were read, big-endian, one after the other. One
  // buffer that keeps its capacity: no allocation per load (D68).
  std::vector<uint32_t> shader_ucode;
  // A load read when its call returns, whose microcode is still in guest memory at its address: not copied (D72).
  static constexpr uint32_t kInGuestMemory = UINT32_MAX;
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

  // A call that is not a draw and wrote nothing into the command buffer changes nothing the tracker models: Leave
  // returns nullptr for it, without reading or applying anything (D72). Most of a frame's calls are such setters,
  // which only write the device's shadow (GRAW: about 16,700 of 19,000). Off for a consumer that wants every
  // call (the observer's verification).
  void set_skip_empty_calls(bool skip) { skip_empty_calls_ = skip; }

  void Enter(Call call, uint32_t device, const uint8_t* base, bool nested);
  // The finished call if it was an outermost device call, else nullptr. Valid until the next Leave.
  const TrackedCall* Leave(Call call, const uint8_t* base, bool nested);

  // The GPU state after the last finished call (in a draw's Leave, the state that draw runs with), with
  // set_apply_locally on.
  const rex::graphics::RegisterFile& registers() const { return *registers_; }
  const uint32_t* values() const;
  // Applies every record to the tracker's own register file too, for a consumer that reads registers() (the
  // observer). Off, that file is not kept (D73).
  void set_apply_locally(bool on) { apply_locally_ = on; }
  // Keeps the record for TakeRecord (the renderer). Off, each call's record is dropped once applied locally.
  void set_keep_record(bool on) { keep_record_ = on; }

  // The registers the tracked calls changed since the last TakeRecord (D73), swapped into `into`; the tracker
  // gathers the next ones into the vector it gets back (cleared). Applied in order to a register file that
  // started zeroed, the records leave it equal to the tracker's.
  void TakeRecord(std::vector<uint32_t>& into) {
    into.clear();
    std::swap(into, record_.data);
    record_.values_at_ = SIZE_MAX;
    applied_to_ = 0;
    // The vector that comes back is a command's, which may have been small: one allocation now, not several.
    if (record_.data.capacity() < 2 * into.size()) record_.data.reserve(2 * into.size() + 256);
  }
  // The last value the finished call's packets wrote into `reg`; false if they wrote none.
  bool LastWrite(uint32_t reg, uint32_t& value) const;

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
  // The same as immutable copies, which stay valid as long as the tracker: it keeps every program it loaded, and
  // another thread may draw with one while the tracker loads others (D71, D72). nullptr before the first load.
  const Program* vertex_program_ptr() const { return programs_[0]; }
  const Program* pixel_program_ptr() const { return programs_[1]; }
  static const Program kNoProgram;

 private:
  struct Pending {
    bool active = false;  // an outermost device call being tracked
    bool kick = false;    // a nested segment change (KickSegment, NewSegment) of the active call
    bool draw = false;    // the call is a draw: its float constant writes are its flush, not needed (D72)
    uint32_t device = 0;
    uint32_t begin = 0;      // CPU address of the call's first packet
    uint32_t scan_from = 0;  // CPU address of the first packet not read yet
    // The flush's dirty masks when the call started (DeviceLayout): float constant groups, fetch slots.
    uint64_t vs_dirty = 0;
    uint64_t ps_dirty = 0;
    uint64_t fetch_dirty = 0;
  };

  // The registers a finished call changed, into record_ (applied to the tracker's own file too with
  // apply_locally_), and its programs.
  void Gather(const Pending& p, Call call, bool draw, const uint8_t* base);
  // Into record_: the shadow's dwords [first, first + count) that differ from image_, by blocks of 16.
  void Send(uint32_t first_register, const uint8_t* source, uint32_t count);
  // Into record_: a value written by a packet, if it differs from image_.
  void SendValue(uint32_t reg, uint32_t value);
  // The fetch constant dwords packets wrote outside the flush (fetch_override_), as bits: dword i is bit i % 64
  // of word i / 64.
  void SetOverridden(uint32_t dword, bool on) {
    uint64_t bit = uint64_t(1) << (dword & 63);
    fetch_overridden_[dword >> 6] = on ? fetch_overridden_[dword >> 6] | bit : fetch_overridden_[dword >> 6] & ~bit;
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
  std::array<uint64_t, 3> fetch_overridden_{};
  std::array<uint32_t, 192> fetch_override_{};
  // The register file the records have built so far, big-endian: compared with the shadow as it is (D73).
  std::unique_ptr<uint32_t[]> image_;
  RegisterRecord record_;
  bool apply_locally_ = false;
  bool keep_record_ = true;
  size_t applied_to_ = 0;  // how much of record_ the tracker's own file has taken
  std::array<const Program*, 2> programs_{};
  // Every program loaded so far, by the hash of its microcode: one shared copy each, loaded again without a copy.
  std::unordered_map<uint64_t, std::shared_ptr<const Program>> program_cache_;
  std::vector<std::shared_ptr<const Program>> replaced_programs_;  // the same hash loaded otherwise: kept alive
  // The programs loaded last, by the low bits of their hash: most loads find theirs here, without the map (D72).
  std::array<const Program*, 256> recent_programs_{};
  uint32_t last_device_ = 0;  // the device of the last device call (for kPackets calls)
  bool skip_empty_calls_ = false;
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
