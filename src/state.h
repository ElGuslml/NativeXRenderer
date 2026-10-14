// XenosNative - the guest GPU state, rebuilt from the D3D device and the D3D calls' own packets (internal).
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
//   - the shader programs: the last vertex and pixel microcode loaded (IM_LOAD, IM_LOAD_IMMEDIATE).
//
// The tracker keeps one register file, updated by every outermost device call on the render thread. The
// packets are read right after the call wrote them: a few hundred bytes per draw.

#pragma once

#include <xenos_native/xenos_native.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace rex::graphics {
class RegisterFile;
}  // namespace rex::graphics

namespace xenos_native {

// What one call wrote into the command buffer, as far as the rebuild needs it.
struct PacketScan {
  // (register, value), in order: type-0/1 writes and SET_CONSTANT*, from the ring and from the indirect
  // buffers it calls. Bin-predicated packets (per tile) are not included.
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  // LOAD_ALU_CONSTANT and friends: (register, value) read from memory when the call returned.
  std::vector<std::pair<uint32_t, uint32_t>> loads;
  struct ShaderLoad {
    uint32_t type;     // 0 vertex, 1 pixel
    uint32_t address;  // CPU address of the microcode (in the command buffer for IM_LOAD_IMMEDIATE)
    uint32_t dwords;
  };
  std::vector<ShaderLoad> shader_loads;
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

  // The programs the GPU runs: the last microcode loaded for each stage.
  struct Program {
    uint32_t address = 0;  // CPU address
    uint32_t dwords = 0;
  };
  const Program& vertex_program() const { return programs_[0]; }
  const Program& pixel_program() const { return programs_[1]; }

 private:
  struct Pending {
    bool active = false;  // an outermost device call being tracked
    bool kick = false;    // a nested segment start of the active call
    uint32_t device = 0;
    uint32_t begin = 0;  // CPU address of the call's first packet
    std::array<uint64_t, 6> dirty{};
    std::vector<std::pair<uint32_t, uint32_t>> kicks;  // segment end, next segment start (CPU)
  };

  void Apply(const Pending& p, bool draw, const uint8_t* base);

  const Profile& profile_;
  std::unique_ptr<rex::graphics::RegisterFile> registers_;
  std::vector<Pending> stack_;
  bool initialized_ = false;
  std::bitset<192> fetch_overridden_;
  std::array<uint32_t, 192> fetch_override_{};
  std::array<Program, 2> programs_{};
  PacketScan scan_;
  TrackedCall last_;
};

}  // namespace xenos_native
