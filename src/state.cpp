// XenosNative - see state.h.

#include "state.h"

#include "internal.h"

#include <rex/graphics/register_file.h>

#include <algorithm>
#include <iterator>

namespace xenos_native {
namespace {

using namespace internal;

// Xenos register file.
constexpr uint32_t kFloatConstants = 0x4000;  // 512 vec4
constexpr uint32_t kFloatDwords = 2048;
constexpr uint32_t kFetchConstants = 0x4800;  // 32 slots x 6 dwords
constexpr uint32_t kFetchDwords = 192;
constexpr uint32_t kBoolLoopConstants = 0x4900;  // 8 bool dwords, then 32 loop dwords
constexpr uint32_t kBoolLoopDwords = 40;
// The XDK's constant windows: the shadow's VS constants go to vec4 0-255, the PS constants to 256-511.
constexpr uint32_t kPixelConstantsDword = 4 * 256;

// Registers the library writes only into the command buffer.
constexpr uint32_t kPacketRegisters[] = {0x2180, 0x2181, 0x2182, 0x2102};

bool IsPacketRegister(uint32_t reg) {
  return std::find(std::begin(kPacketRegisters), std::end(kPacketRegisters), reg) != std::end(kPacketRegisters);
}

// Physical address -> the guest's 0xA0000000 view of physical memory.
uint32_t PhysicalToVirtual(uint32_t physical) { return 0xA0000000u | (physical & 0x1FFFFFFFu); }

uint32_t ConstantBase(uint32_t type) {
  switch (type) {
    case 0: return kFloatConstants;
    case 1: return kFetchConstants;
    case 2: return kBoolLoopConstants;
    case 3: return kBoolLoopConstants + 8;
    case 4: return 0x2000;
    default: return 0;
  }
}

void ScanPackets(const uint8_t* base, uint32_t begin, uint32_t end, PacketScan& out, int depth) {
  uint32_t p = begin;
  while (p + 4 <= end) {
    uint32_t header = Load32(base, p);
    p += 4;
    if (!header) continue;
    switch (header >> 30) {
      case 0: {
        uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        uint32_t reg = header & 0x7FFF;
        bool one = (header >> 15) & 1;
        if (p + 4 * count > end) return void(out.complete = false);
        for (uint32_t i = 0; i < count; ++i) out.writes.emplace_back(one ? reg : reg + i, Load32(base, p + 4 * i));
        p += 4 * count;
        break;
      }
      case 1:
        if (p + 8 > end) return void(out.complete = false);
        out.writes.emplace_back(header & 0x7FF, Load32(base, p));
        out.writes.emplace_back((header >> 11) & 0x7FF, Load32(base, p + 4));
        p += 8;
        break;
      case 2:
        break;
      case 3: {
        uint32_t opcode = (header >> 8) & 0x7F;
        uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        if (p + 4 * count > end) return void(out.complete = false);
        if (header & 1) {
          ++out.predicated;  // bin-predicated: per tile
        } else {
          switch (opcode) {
            case 0x2D: {  // SET_CONSTANT
              uint32_t ot = Load32(base, p);
              uint32_t reg = ConstantBase((ot >> 16) & 0xFF) + (ot & 0x7FF);
              for (uint32_t i = 1; i < count; ++i) out.writes.emplace_back(reg + i - 1, Load32(base, p + 4 * i));
              break;
            }
            case 0x55:    // SET_CONSTANT2
            case 0x56: {  // SET_SHADER_CONSTANTS
              uint32_t reg = Load32(base, p) & 0xFFFF;
              for (uint32_t i = 1; i < count; ++i) out.writes.emplace_back(reg + i - 1, Load32(base, p + 4 * i));
              break;
            }
            case 0x2F: {  // LOAD_ALU_CONSTANT
              uint32_t address = PhysicalToVirtual(Load32(base, p) & 0x3FFFFFFF);
              uint32_t ot = Load32(base, p + 4);
              uint32_t size = Load32(base, p + 8) & 0xFFF;
              uint32_t reg = ConstantBase((ot >> 16) & 0xFF) + (ot & 0x7FF);
              for (uint32_t i = 0; i < size; ++i) out.loads.emplace_back(reg + i, Load32(base, address + 4 * i));
              break;
            }
            case 0x27: {  // IM_LOAD
              uint32_t addr_type = Load32(base, p);
              uint32_t start_size = Load32(base, p + 4);
              out.shader_loads.push_back({addr_type & 3, PhysicalToVirtual(addr_type & ~3u), start_size & 0xFFFF});
              break;
            }
            case 0x2B: {  // IM_LOAD_IMMEDIATE: the microcode follows in the packet
              uint32_t type = Load32(base, p);
              uint32_t start_size = Load32(base, p + 4);
              out.shader_loads.push_back({type & 3, p + 8, start_size & 0xFFFF});
              break;
            }
            case 0x3F: {  // INDIRECT_BUFFER: the library's prebuilt buffers
              uint32_t target = PhysicalToVirtual(Load32(base, p));
              uint32_t length = Load32(base, p + 4) & 0xFFFFF;
              ++out.indirect;
              if (depth == 0) ScanPackets(base, target, target + 4 * length, out, depth + 1);
              break;
            }
            case 0x22:  // DRAW_INDX
            case 0x36:  // DRAW_INDX_2
              ++out.draws;
              break;
            default:
              break;
          }
        }
        p += 4 * count;
        break;
      }
    }
  }
}

bool FetchSlotDirty(const std::array<uint64_t, 6>& dirty, uint32_t slot) { return (dirty[3] >> (31 - slot)) & 1; }
bool ConstantGroupDirty(uint64_t mask, uint32_t group) { return (mask >> (63 - group)) & 1; }

}  // namespace

void PacketScan::Clear() {
  writes.clear();
  loads.clear();
  shader_loads.clear();
  draws = indirect = predicated = 0;
  complete = true;
}

StateTracker::StateTracker(const Profile& profile)
    : profile_(profile), registers_(std::make_unique<rex::graphics::RegisterFile>()) {
  Reset();
}

StateTracker::~StateTracker() = default;

const uint32_t* StateTracker::values() const { return registers_->values; }

void StateTracker::Reset() {
  std::fill(std::begin(registers_->values), std::end(registers_->values), 0u);
  initialized_ = false;
  fetch_overridden_.reset();
  programs_ = {};
}

void StateTracker::Enter(Call call, uint32_t device, const uint8_t* base, bool nested) {
  Pending& p = stack_.emplace_back();
  const DeviceLayout& d = profile_.device;
  if (nested) {
    // A new command buffer segment inside the tracked call: its packets continue elsewhere.
    Pending& outer = stack_.front();
    if (call == Call::kKickSegment && outer.active) {
      p.kick = true;
      outer.kicks.emplace_back(Load32(base, outer.device + d.command_buffer_write) + 4, 0);
    }
    return;
  }
  if (!Info(call).device) return;
  p.active = true;
  p.device = device;
  // The write pointer holds the address of the last dword written (the library writes with stwu): the
  // call's packets start right after it.
  p.begin = Load32(base, device + d.command_buffer_write) + 4;
  for (int i = 0; i < 6; ++i) p.dirty[i] = Load64(base, device + d.dirty_masks + 8 * i);
}

const TrackedCall* StateTracker::Leave(Call call, const uint8_t* base, bool nested) {
  Pending p = std::move(stack_.back());
  stack_.pop_back();
  const DeviceLayout& d = profile_.device;
  if (p.kick && !stack_.empty()) {
    Pending& outer = stack_.front();
    if (!outer.kicks.empty()) outer.kicks.back().second = Load32(base, outer.device + d.command_buffer_write) + 4;
    return nullptr;
  }
  if (nested || !p.active) return nullptr;

  // The packets this call wrote: from where it started to where it ended, skipping the jumps to new
  // segments.
  uint32_t end = Load32(base, p.device + d.command_buffer_write) + 4;
  scan_.Clear();
  uint32_t from = p.begin;
  for (const auto& [segment_end, next_start] : p.kicks) {
    if (segment_end >= from) {
      ScanPackets(base, from, segment_end, scan_, 0);
    } else {
      scan_.complete = false;
    }
    from = next_start;
  }
  if (end >= from && end - from < 0x400000) {
    ScanPackets(base, from, end, scan_, 0);
  } else {
    scan_.complete = false;
  }

  bool draw = Info(call).flags & kDraw;
  Apply(p, draw, base);

  last_.call = call;
  last_.draw = draw;
  last_.device = p.device;
  last_.begin = GpuAddress(p.begin);
  last_.end = GpuAddress(end);
  last_.scan = &scan_;
  return &last_;
}

void StateTracker::Apply(const Pending& p, bool draw, const uint8_t* base) {
  const DeviceLayout& d = profile_.device;
  uint32_t* r = registers_->values;
  auto shadow = [&](uint32_t offset) { return Load32(base, p.device + offset); };

  if (!initialized_) {
    // The first call: start from the shadow. Literal constants loaded before are unknown until the
    // shaders that use them are bound again.
    for (const auto& g : d.register_groups) {
      for (uint32_t i = 0; i < g.count; ++i) r[g.first_register + i] = shadow(g.offset + 4 * i);
    }
    for (uint32_t i = 0; i < kFetchDwords; ++i) r[kFetchConstants + i] = shadow(d.fetch_constants + 4 * i);
    for (uint32_t i = 0; i < 1024; ++i) {
      r[kFloatConstants + i] = shadow(d.vs_float_constants + 4 * i);
      r[kFloatConstants + kPixelConstantsDword + i] = shadow(d.ps_float_constants + 4 * i);
    }
    for (uint32_t i = 0; i < kBoolLoopDwords; ++i) r[kBoolLoopConstants + i] = shadow(d.bool_loop_constants + 4 * i);
    initialized_ = true;
  }

  if (draw) {
    // The flush uploads the dirty float constant groups (4 vec4 each) from the shadow, before the program
    // flush loads the literals.
    for (uint32_t g = 0; g < 64; ++g) {
      if (ConstantGroupDirty(p.dirty[0], g)) {
        for (uint32_t i = 0; i < 16; ++i) r[kFloatConstants + 16 * g + i] = shadow(d.vs_float_constants + 4 * (16 * g + i));
      }
      if (ConstantGroupDirty(p.dirty[1], g)) {
        for (uint32_t i = 0; i < 16; ++i)
          r[kFloatConstants + kPixelConstantsDword + 16 * g + i] = shadow(d.ps_float_constants + 4 * (16 * g + i));
      }
    }
    // A fetch slot flushed from the shadow loses what packets wrote there before.
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (FetchSlotDirty(p.dirty, slot)) {
        for (uint32_t j = 0; j < 6; ++j) fetch_overridden_.reset(6 * slot + j);
      }
    }
  }

  for (const auto& [reg, value] : scan_.writes) {
    if (reg >= rex::graphics::RegisterFile::kRegisterCount) continue;
    if (reg >= kFloatConstants && reg < kFloatConstants + kFloatDwords) {
      // A draw's float constant writes are its flush, taken from the shadow above.
      if (!draw) r[reg] = value;
    } else if (reg >= kFetchConstants && reg < kFetchConstants + kFetchDwords) {
      uint32_t dw = reg - kFetchConstants;
      if (!draw || value != shadow(d.fetch_constants + 4 * dw)) {
        fetch_override_[dw] = value;
        fetch_overridden_.set(dw);
      }
      r[reg] = value;
    } else {
      r[reg] = value;
    }
  }
  for (const auto& [reg, value] : scan_.loads) {
    if (reg < rex::graphics::RegisterFile::kRegisterCount) r[reg] = value;
  }
  for (const auto& load : scan_.shader_loads) {
    if (load.type < 2) programs_[load.type] = {load.address, load.dwords};
  }

  if (draw) {
    // Render state, fetch constants, bool and loop constants: the shadow, as the draw left it.
    for (const auto& g : d.register_groups) {
      for (uint32_t i = 0; i < g.count; ++i) {
        uint32_t reg = g.first_register + i;
        if (!IsPacketRegister(reg)) r[reg] = shadow(g.offset + 4 * i);
      }
    }
    for (uint32_t i = 0; i < kFetchDwords; ++i) {
      r[kFetchConstants + i] = fetch_overridden_[i] ? fetch_override_[i] : shadow(d.fetch_constants + 4 * i);
    }
    for (uint32_t i = 0; i < kBoolLoopDwords; ++i) r[kBoolLoopConstants + i] = shadow(d.bool_loop_constants + 4 * i);
  }
}

}  // namespace xenos_native
