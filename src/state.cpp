// NativeXRenderer - see state.h.

#include "state.h"

#include "internal.h"

#include <rex/graphics/register_file.h>
#include <rex/logging.h>

#include <fmt/format.h>
#include <xxhash.h>

#include <algorithm>
#include <bit>
#include <iterator>
#include <string>

namespace native_x_renderer {
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

// Registers a draw packet carries in its payload.
constexpr uint32_t kVgtDmaBase = 0x21FA;
constexpr uint32_t kVgtDmaSize = 0x21FB;
constexpr uint32_t kVgtDrawInitiator = 0x21FC;

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

// DRAW_INDX (viz query token, then as DRAW_INDX_2) and DRAW_INDX_2 (VGT_DRAW_INITIATOR, then VGT_DMA_BASE
// and VGT_DMA_SIZE for DMA indices): the command processor writes these registers from the packet
// (ExecutePacketType3Draw). `p` is the first payload dword, `count` the payload size.
void ScanDraw(const uint8_t* base, uint32_t opcode, uint32_t p, uint32_t count, PacketScan& out) {
  uint32_t q = p + (opcode == 0x22 ? 4 : 0), q_end = p + 4 * count;
  if (q + 4 <= q_end) {
    uint32_t initiator = Load32(base, q);
    out.writes.emplace_back(kVgtDrawInitiator, initiator);
    if (((initiator >> 6) & 3) == 0 && q + 12 <= q_end) {  // source_select kDMA
      out.writes.emplace_back(kVgtDmaBase, Load32(base, q + 4));
      out.writes.emplace_back(kVgtDmaSize, Load32(base, q + 8));
    }
  }
  ++out.draws;
}

// A draw's flush writes its dirty float constant groups and the bool and loop constants as packets; the tracker
// takes those from the shadow instead (Apply). With `skip_constants` (a draw's scan) they are left out: in GRAW about
// 165 of a draw's 200 register writes, mostly the bone matrices (D72).
bool SkippedConstants(bool skip_constants, uint32_t reg, uint32_t count) {
  return skip_constants && ((reg >= kFloatConstants && reg + count <= kFloatConstants + kFloatDwords) ||
                            (reg >= kBoolLoopConstants && reg + count <= kBoolLoopConstants + kBoolLoopDwords));
}

// `copy_microcode`: a shader load's microcode is copied (a scan when a segment ends: the library may reuse its
// memory before the call returns); otherwise it is read where it is, before the call returns (D72).
void ScanPackets(const uint8_t* base, uint32_t begin, uint32_t end, PacketScan& out, int depth,
                 bool skip_constants, bool copy_microcode) {
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
        if (!SkippedConstants(skip_constants, reg, one ? 1 : count)) {
          for (uint32_t i = 0; i < count; ++i) out.writes.emplace_back(one ? reg : reg + i, Load32(base, p + 4 * i));
        }
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
        if ((header & 1) && (opcode == 0x22 || opcode == 0x36)) {
          // The XDK bin-predicates every draw packet, tiled or not (the GPU runs it in each bin it
          // touches); the draw is the same in every bin.
          ++out.predicated;
          ScanDraw(base, opcode, p, count, out);
        } else if (header & 1) {
          ++out.predicated;  // bin-predicated: per tile
          if (opcode == 0x2D) {  // SET_CONSTANT
            uint32_t ot = Load32(base, p);
            uint32_t reg = ConstantBase((ot >> 16) & 0xFF) + (ot & 0x7FF);
            for (uint32_t i = 1; i < count; ++i) out.predicated_writes.emplace_back(reg + i - 1, Load32(base, p + 4 * i));
          }
        } else {
          switch (opcode) {
            case 0x2D: {  // SET_CONSTANT
              uint32_t ot = Load32(base, p);
              uint32_t reg = ConstantBase((ot >> 16) & 0xFF) + (ot & 0x7FF);
              if (SkippedConstants(skip_constants, reg, count - 1)) break;
              for (uint32_t i = 1; i < count; ++i) out.writes.emplace_back(reg + i - 1, Load32(base, p + 4 * i));
              break;
            }
            case 0x55:    // SET_CONSTANT2
            case 0x56: {  // SET_SHADER_CONSTANTS
              uint32_t reg = Load32(base, p) & 0xFFFF;
              if (SkippedConstants(skip_constants, reg, count - 1)) break;
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
            case 0x27:    // IM_LOAD
            case 0x2B: {  // IM_LOAD_IMMEDIATE: the microcode follows in the packet
              uint32_t type = Load32(base, p);
              uint32_t start_size = Load32(base, p + 4);
              bool immediate = opcode == 0x2B;
              uint32_t address = immediate ? p + 8 : PhysicalToVirtual(type & ~3u);
              uint32_t dwords = start_size & 0xFFFF;
              if (immediate && dwords > count - 2) dwords = count - 2;
              const auto* ucode = reinterpret_cast<const uint32_t*>(base + address);
              if (copy_microcode) {
                out.shader_loads.push_back(
                    {type & 3, address, dwords, p - 4, immediate, uint32_t(out.shader_ucode.size())});
                out.shader_ucode.insert(out.shader_ucode.end(), ucode, ucode + dwords);
              } else {
                out.shader_loads.push_back({type & 3, address, dwords, p - 4, immediate, PacketScan::kInGuestMemory});
              }
              break;
            }
            case 0x3F: {  // INDIRECT_BUFFER: the library's prebuilt buffers
              uint32_t target = PhysicalToVirtual(Load32(base, p));
              uint32_t length = Load32(base, p + 4) & 0xFFFFF;
              ++out.indirect;
              if (depth == 0) {
                ScanPackets(base, target, target + 4 * length, out, depth + 1, skip_constants, copy_microcode);
              }
              break;
            }
            case 0x22:  // DRAW_INDX
            case 0x36:  // DRAW_INDX_2
              ScanDraw(base, opcode, p, count, out);
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

bool FetchSlotDirty(uint64_t mask, uint32_t slot0_bit, uint32_t slot) { return (mask >> (slot0_bit - slot)) & 1; }
bool ConstantGroupDirty(uint64_t mask, uint32_t group) { return (mask >> (63 - group)) & 1; }

}  // namespace

bool DeviceLayoutComplete(const DeviceLayout& d, const char* feature) {
  std::string missing;
  auto need = [&](uint32_t offset, const char* name) {
    if (offset == kUnknown) missing += fmt::format("{}{}", missing.empty() ? "" : ", ", name);
  };
  need(d.command_buffer_write, "command_buffer_write");
  need(d.vs_constants_dirty, "vs_constants_dirty");
  need(d.ps_constants_dirty, "ps_constants_dirty");
  need(d.fetch_constants_dirty, "fetch_constants_dirty");
  if (d.fetch_dirty_slot0_bit < 31 || d.fetch_dirty_slot0_bit > 63) need(kUnknown, "fetch_dirty_slot0_bit");
  need(d.fetch_constants, "fetch_constants");
  need(d.vs_float_constants, "vs_float_constants");
  need(d.ps_float_constants, "ps_float_constants");
  need(d.bool_loop_constants, "bool_loop_constants");
  if (missing.empty()) return true;
  REXLOG_WARN("[native_x_renderer] {}: the profile's device layout lacks {}; off", feature, missing);
  return false;
}

void PacketScan::Clear() {
  writes.clear();
  predicated_writes.clear();
  loads.clear();
  shader_loads.clear();
  shader_ucode.clear();
  draws = indirect = predicated = 0;
  complete = true;
}

const StateTracker::Program StateTracker::kNoProgram{};

StateTracker::StateTracker(const Profile& profile)
    : profile_(profile), registers_(std::make_unique<rex::graphics::RegisterFile>()) {
  for (const auto& g : profile_.device.register_groups) {
    uint32_t run = 0;
    for (uint32_t i = 0; i <= g.count; ++i) {
      if (i < g.count && !IsPacketRegister(g.first_register + i)) continue;
      if (i > run) shadow_runs_.push_back({g.first_register + run, g.offset + 4 * run, i - run});
      run = i + 1;
    }
  }
  Reset();
}

StateTracker::~StateTracker() = default;

const uint32_t* StateTracker::values() const { return registers_->values; }

void StateTracker::Reset() {
  std::fill(std::begin(registers_->values), std::end(registers_->values), 0u);
  initialized_ = false;
  fetch_overridden_ = {};
  programs_ = {};
  changed_.clear();
  all_changed_ = true;
}

void StateTracker::Enter(Call call, uint32_t device, const uint8_t* base, bool nested) {
  Pending& p = stack_.emplace_back();
  const DeviceLayout& d = profile_.device;
  if (nested) {
    // A new command buffer segment inside the tracked call: read the finished segment's packets now (the
    // library may reuse its memory before the call returns); the call's packets continue elsewhere. The
    // library moves to a new segment in KickSegment, and in NewSegment, which KickSegment calls and the
    // library's reservation may call again: whatever comes first reads up to the write pointer. A segment
    // change inside another one reads nothing: what the outer one wrote is its own bookkeeping (POTCAWE: the
    // segment's fence; GRAW: the fence and a submission record past the submitted range, not packets).
    Pending& outer = stack_.front();
    if ((call == Call::kKickSegment || call == Call::kNewSegment) && outer.active) {
      p.kick = true;
      bool inside_segment_change = false;
      for (size_t i = 1; i + 1 < stack_.size(); ++i) inside_segment_change |= stack_[i].kick;
      if (inside_segment_change) return;
      uint32_t segment_end = Load32(base, outer.device + d.command_buffer_write) + 4;
      if (segment_end >= outer.scan_from && segment_end - outer.scan_from < 0x400000) {
        ScanPackets(base, outer.scan_from, segment_end, scan_, 0, outer.draw, true);
        outer.scan_from = segment_end;
      } else {
        scan_.complete = false;
      }
    }
    return;
  }
  bool device_call = Info(call).device;
  if (!device_call && !((Info(call).flags & kPackets) && last_device_)) return;
  if (device_call) last_device_ = device;
  device = device_call ? device : last_device_;
  // The dirty masks matter to draws, and to an inline draw's begin, whose flush its end draws with.
  bool draw = Info(call).flags & kDraw;
  uint64_t vs_dirty = 0, ps_dirty = 0, fetch_dirty = 0;
  if (draw || (Info(call).flags & kInlineBegin)) {
    vs_dirty = Load64(base, device + d.vs_constants_dirty);
    ps_dirty = Load64(base, device + d.ps_constants_dirty);
    fetch_dirty = Load64(base, device + d.fetch_constants_dirty);
  }
  if (Info(call).flags & kInlineBegin) {
    // Its flush commits its packets (state, programs) as usual, and this call reads them; its fetch constant
    // and draw packets stay after the write pointer until EndVertices commits them. EndVertices is the draw,
    // with the dirty masks this call's flush sees now.
    inline_begin_ = {true, vs_dirty, ps_dirty, fetch_dirty};
  }
  if ((Info(call).flags & kInlineEnd) && inline_begin_.pending) {
    vs_dirty = inline_begin_.vs_dirty;
    ps_dirty = inline_begin_.ps_dirty;
    fetch_dirty = inline_begin_.fetch_dirty;
    inline_begin_.pending = false;
  }
  p.active = true;
  p.draw = draw;
  p.device = device;
  // The write pointer holds the address of the last dword written (the library writes with stwu): the
  // call's packets start right after it.
  p.begin = Load32(base, p.device + d.command_buffer_write) + 4;
  p.scan_from = p.begin;
  p.vs_dirty = vs_dirty;
  p.ps_dirty = ps_dirty;
  p.fetch_dirty = fetch_dirty;
  scan_.Clear();
}

const TrackedCall* StateTracker::Leave(Call call, const uint8_t* base, bool nested) {
  Pending p = std::move(stack_.back());
  stack_.pop_back();
  const DeviceLayout& d = profile_.device;
  if (p.kick && !stack_.empty()) {
    // The new segment: the call's next packets start after the write pointer.
    Pending& outer = stack_.front();
    outer.scan_from = Load32(base, outer.device + d.command_buffer_write) + 4;
    return nullptr;
  }
  if (nested || !p.active) return nullptr;

  // The packets of the call's last segment (the earlier ones were read when the segments ended).
  uint32_t end = Load32(base, p.device + d.command_buffer_write) + 4;
  bool draw = p.draw;
  // A setter that only wrote the shadow (D72): nothing to read or apply. One that moved to another segment wrote
  // packets (begin != scan_from).
  if (skip_empty_calls_ && !draw && end == p.begin && p.scan_from == p.begin && initialized_) return nullptr;
  if (end >= p.scan_from && end - p.scan_from < 0x400000) {
    ScanPackets(base, p.scan_from, end, scan_, 0, draw, false);
  } else {
    scan_.complete = false;
  }

  Apply(p, call, draw, base);

  last_.call = call;
  last_.draw = draw;
  last_.device = p.device;
  last_.begin = GpuAddress(p.begin);
  last_.end = GpuAddress(end);
  last_.scan = &scan_;
  return &last_;
}

void StateTracker::Apply(const Pending& p, Call call, bool draw, const uint8_t* base) {
  const DeviceLayout& d = profile_.device;
  uint32_t* r = registers_->values;
  auto shadow = [&](uint32_t offset) { return Load32(base, p.device + offset); };
  // `count` consecutive registers from the shadow at `offset`, byte-swapped, noting the ones that change. In
  // blocks of 16: most blocks are as the last draw left them, which one vectorized pass finds.
  const uint8_t* device = base + p.device;
  auto copy = [this, r, device](uint32_t first_register, uint32_t offset, uint32_t count) {
    const uint8_t* src = device + offset;
    uint32_t* dst = r + first_register;
    uint32_t values[16];
    auto changed = [&](uint32_t block, uint32_t n) {
      uint32_t diff = 0;
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t value;
        std::memcpy(&value, src + 4 * (block + i), sizeof(value));
        values[i] = std::byteswap(value);
        diff |= values[i] ^ dst[block + i];
      }
      return diff != 0;
    };
    uint32_t block = 0;
    // A block that changed is written whole and noted as one run (D72).
    for (; block + 16 <= count; block += 16) {
      if (!changed(block, 16)) continue;
      std::memcpy(dst + block, values, 16 * sizeof(uint32_t));
      Changed(first_register + block, 16);
    }
    if (uint32_t n = count - block; n && changed(block, n)) {
      std::memcpy(dst + block, values, n * sizeof(uint32_t));
      Changed(first_register + block, n);
    }
  };

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
    all_changed_ = true;
  }

  if (draw) {
    // The flush uploads the dirty float constant groups (4 vec4 each) from the shadow, before the program
    // flush loads the literals.
    for (uint32_t g = 0; g < 64; ++g) {
      if (ConstantGroupDirty(p.vs_dirty, g)) copy(kFloatConstants + 16 * g, d.vs_float_constants + 64 * g, 16);
      if (ConstantGroupDirty(p.ps_dirty, g)) {
        copy(kFloatConstants + kPixelConstantsDword + 16 * g, d.ps_float_constants + 64 * g, 16);
      }
    }
    // A fetch slot flushed from the shadow loses what packets wrote there before.
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (FetchSlotDirty(p.fetch_dirty, d.fetch_dirty_slot0_bit, slot)) {
        for (uint32_t j = 0; j < 6; ++j) SetOverridden(6 * slot + j, false);
      }
    }
  }

  for (const auto& [reg, value] : scan_.writes) {
    if (reg >= rex::graphics::RegisterFile::kRegisterCount) continue;
    if (reg >= kFloatConstants && reg < kFloatConstants + kFloatDwords) {
      // A draw's float constant writes are its flush, taken from the shadow above.
      if (!draw) Set(r, reg, value);
    } else if (reg >= kFetchConstants && reg < kFetchConstants + kFetchDwords) {
      uint32_t dw = reg - kFetchConstants;
      if (!draw || value != shadow(d.fetch_constants + 4 * dw)) {
        fetch_override_[dw] = value;
        SetOverridden(dw, true);
      } else {
        // The draw's flush wrote the shadow's value: the slot comes from the shadow again, even if its dirty
        // bit was set during the flush (GRAW's flush writes the stream fetch constants and marks them then).
        SetOverridden(dw, false);
      }
      Set(r, reg, value);
    } else {
      Set(r, reg, value);
    }
  }
  for (const auto& [reg, value] : scan_.loads) {
    if (reg < rex::graphics::RegisterFile::kRegisterCount) Set(r, reg, value);
  }
  for (const auto& load : scan_.shader_loads) {
    if (load.type >= 2) continue;
    // One shared copy per program: the renderer's thread may still draw with the one before (D71). A program
    // loaded before is taken from the cache, without a copy.
    const uint32_t* ucode = load.ucode_offset == PacketScan::kInGuestMemory
                                ? reinterpret_cast<const uint32_t*>(base + load.address)
                                : scan_.shader_ucode.data() + load.ucode_offset;
    uint64_t hash = XXH3_64bits(ucode, size_t(load.dwords) * sizeof(uint32_t));
    const Program*& recent = recent_programs_[hash & 255];
    if (recent && recent->hash == hash && recent->ucode.size() == load.dwords && recent->immediate == load.immediate) {
      programs_[load.type] = recent;
      continue;
    }
    std::shared_ptr<const Program>& cached = program_cache_[hash];
    if (!cached || cached->ucode.size() != load.dwords || cached->immediate != load.immediate) {
      // One it replaces stays alive: a command may still point at it (D72).
      if (cached) replaced_programs_.push_back(std::move(cached));
      auto program = std::make_shared<Program>();
      program->address = load.address;
      program->ucode.assign(ucode, ucode + load.dwords);
      program->hash = hash;
      program->loaded_by = call;
      program->packet = load.packet;
      program->immediate = load.immediate;
      cached = std::move(program);
    }
    programs_[load.type] = recent = cached.get();
  }

  if (draw) {
    // Render state, bool and loop constants: the shadow, as the draw left it. Fetch constants below.
    for (const ShadowRun& run : shadow_runs_) copy(run.first_register, run.offset, run.count);
    // Fetch constants: the slots the draw's flush uploads (dirty when the call started) take the shadow's
    // values; the others keep what the GPU has, which is what the shadow held when they were last flushed (D72).
    // A slot the flush dirties as it goes (GRAW's streams) is in the draw's packets, applied above.
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (FetchSlotDirty(p.fetch_dirty, d.fetch_dirty_slot0_bit, slot)) {
        copy(kFetchConstants + 6 * slot, d.fetch_constants + 24 * slot, 6);
      }
    }
    for (uint32_t word = 0; word < 3; ++word) {
      for (uint64_t bits = fetch_overridden_[word]; bits; bits &= bits - 1) {
        uint32_t i = 64 * word + uint32_t(std::countr_zero(bits));
        Set(r, kFetchConstants + i, fetch_override_[i]);
      }
    }
    copy(kBoolLoopConstants, d.bool_loop_constants, kBoolLoopDwords);
  }
}

}  // namespace native_x_renderer
