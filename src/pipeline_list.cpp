// NativeXRenderer - see pipeline_list.h.

#include "pipeline_list.h"

#include <native_x_renderer/native_x_renderer.h>
#include <rex/graphics/format/ucode.h>
#include <rex/logging.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_set>

#include <xxhash.h>

// The raw deflate decoder of stb_image (the SDK's thirdparty), private to this file: the Neversoft engines' CHNK
// files (GH World Tour's compressed MaterialLibrary.bin.xen).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

namespace native_x_renderer {

namespace ucode = rex::graphics::ucode;

namespace {

constexpr uint32_t kMagic = 0x4C504E58;  // "XNPL"
// 2: patches of ALU instructions that export (D136), each patch's fields as 32 bits and its slot's top bit set for those.
// A version 1 list reads the same.
constexpr uint32_t kListVersion = 2;
constexpr uint16_t kAluExportSlot = 0x8000;
constexpr uint8_t kShaderTag = 'S', kTranslationTag = 'T', kPipelineTag = 'P';
// An XDK shader container's magic: 0x102A, the shader compiler's version (0x11 in XDK 2.0.7978, 0x0E in 2.0.2417), then
// 0 for a pixel shader or 1 for a vertex shader.
constexpr uint32_t kContainerMagicMask = 0xFFFF00FE, kContainerMagic = 0x102A0000;

// A Neversoft CHNK file decompressed: chunks with a 0x80-byte big-endian header (dword 1 its size, 2 the compressed
// size, 3 the offset of the next chunk or 0, 5 the uncompressed size, 6 its offset in the output), raw deflate after it.
// Empty if it is not one, or damaged.
std::vector<uint8_t> DecompressChnk(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out;
  // The chunks follow one another until one's next offset is 0 or leads past the file's end (GH World Tour's last
  // chunk points past it).
  for (size_t pos = 0; pos <= in.size() && in.size() - pos >= 0x80 && !std::memcmp(in.data() + pos, "CHNK", 4);) {
    uint32_t h[8];
    std::memcpy(h, in.data() + pos, sizeof(h));
    for (uint32_t& v : h) v = __builtin_bswap32(v);
    const uint32_t header = h[1], compressed = h[2], next = h[3], size = h[5], offset = h[6];
    if (header < 0x80 || header > in.size() - pos || in.size() - pos - header < compressed || size > (64u << 20) ||
        offset > (256u << 20)) {
      return {};
    }
    if (out.size() < size_t(offset) + size) out.resize(size_t(offset) + size);
    int decoded = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(out.data() + offset), int(size),
                                                   reinterpret_cast<const char*>(in.data() + pos + header),
                                                   int(compressed));
    if (decoded != int(size)) return {};
    if (!next) break;
    pos += next;
  }
  return out;
}

struct Header {
  uint32_t magic, version, storage_version, state_size, title_id, reserved[3];
};

// The vfetch fields the D3D library sets for a vertex declaration (ucode::VertexFetchInstruction): the dword of the
// instruction, the first bit and the width. The source register, the opcode and the predicate are the shader's own,
// never patched.
struct Field {
  uint8_t dword, low, bits;
};
constexpr Field kFetchFields[] = {
    {0, 20, 5},   // const_index
    {0, 25, 2},   // const_index_sel
    {0, 27, 3},   // prefetch_count
    {1, 0, 12},   // dst_swiz
    {1, 12, 1},   // fomat_comp_all (signed)
    {1, 13, 1},   // num_format_all (normalized or not)
    {1, 14, 1},   // signed_rf_mode_all
    {1, 15, 1},   // is_index_rounded
    {1, 16, 6},   // format
    {1, 24, 6},   // exp_adjust
    {1, 30, 1},   // is_mini_fetch
    {2, 0, 8},    // stride
    {2, 8, 23},   // offset
    {0, 12, 6},   // dst_reg: the register an element is fetched into, which an older XDK's library sets for the
                  // declaration too (GRAW, XDK 2.0.2417); last, so that a list made before it reads the same
};

// The fields of an ALU instruction (ucode::AluInstruction), every bit of it, for those that export. POTCAWE's D3D
// library (XDK 2.0.5426) links a vertex shader to the pixel shader it draws with (D136): an export to an interpolator the
// pixel shader does not read becomes an instruction that writes nothing (export_data and the write masks 0, dwords 1
// and 2 a no-op's: 0 and 0x02000000), the other interpolators are numbered again (vector_dest), and a position export's
// placeholder source registers (255) become the register its vertex fetch writes. A patch carries only the fields that
// changed: register numbers, masks, swizzles, opcodes.
constexpr Field kAluFields[] = {
    {0, 0, 6},   // vector_dest
    {0, 6, 1},   // vector_dest_rel
    {0, 7, 1},   // abs_constants
    {0, 8, 6},   // scalar_dest
    {0, 14, 1},  // scalar_dest_rel
    {0, 15, 1},  // export_data
    {0, 16, 4},  // vector_write_mask
    {0, 20, 4},  // scalar_write_mask
    {0, 24, 1},  // vector_clamp
    {0, 25, 1},  // scalar_clamp
    {0, 26, 6},  // scalar_opc
    {1, 0, 8},   // src3_swiz
    {1, 8, 8},   // src2_swiz
    {1, 16, 8},  // src1_swiz
    {1, 24, 1},  // src3_reg_negate
    {1, 25, 1},  // src2_reg_negate
    {1, 26, 1},  // src1_reg_negate
    {1, 27, 1},  // pred_condition
    {1, 28, 1},  // is_predicated
    {1, 29, 1},  // const_address_register_relative
    {1, 30, 1},  // const_1_rel_abs
    {1, 31, 1},  // const_0_rel_abs
    {2, 0, 8},   // src3_reg
    {2, 8, 8},   // src2_reg
    {2, 16, 8},  // src1_reg
    {2, 24, 5},  // vector_opc
    {2, 29, 1},  // src3_sel
    {2, 30, 1},  // src2_sel
    {2, 31, 1},  // src1_sel
};
constexpr uint32_t kExportBit = 1u << 15;  // export_data, in an ALU instruction's dword 0

struct FieldTable {
  const Field* fields;
  uint32_t count;
};
FieldTable Table(InstructionPatch::Kind kind) {
  if (kind == InstructionPatch::Kind::kAluExport) return {kAluFields, uint32_t(std::size(kAluFields))};
  return {kFetchFields, uint32_t(std::size(kFetchFields))};
}

uint32_t Mask(const Field& f) { return f.bits == 32 ? ~0u : ((1u << f.bits) - 1) << f.low; }
// The bits of a vertex fetch instruction's dword that a patch may change.
uint32_t PatchableBits(uint32_t dword) {
  uint32_t mask = 0;
  for (const Field& f : kFetchFields) {
    if (f.dword == dword) mask |= Mask(f);
  }
  return mask;
}

uint32_t Swap(uint32_t value) { return __builtin_bswap32(value); }

template <typename T>
void Put(std::vector<uint8_t>& out, const T& value) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool Get(const std::vector<uint8_t>& in, size_t& pos, T& value) {
  if (in.size() - pos < sizeof(T)) return false;
  std::memcpy(&value, in.data() + pos, sizeof(T));
  pos += sizeof(T);
  return true;
}

uint64_t Hash(const uint32_t* ucode, uint32_t dwords) { return XXH3_64bits(ucode, size_t(dwords) * 4); }

// The fields of `kind`'s table in which the instruction at `slot` differs between the two programs.
InstructionPatch DiffFields(InstructionPatch::Kind kind, uint32_t slot, const uint32_t* original, const uint32_t* patched) {
  InstructionPatch patch;
  patch.kind = kind;
  patch.slot = uint16_t(slot);
  const FieldTable table = Table(kind);
  for (uint32_t f = 0; f < table.count; ++f) {
    const Field& field = table.fields[f];
    uint32_t a = Swap(original[slot * 3 + field.dword]) & Mask(field);
    uint32_t b = Swap(patched[slot * 3 + field.dword]) & Mask(field);
    if (a != b) {
      patch.fields |= 1u << f;
      patch.values.push_back(b >> field.low);
    }
  }
  return patch;
}

// The patches that turn `original` into `patched` (both big-endian, same length), if they differ only in the fetch
// fields of vertex fetch instructions and in ALU instructions that export (in either program, D136).
bool DerivePatches(const uint32_t* original, const uint32_t* patched, uint32_t dwords,
                   const std::vector<uint32_t>& fetch_slots, const std::vector<uint32_t>& alu_slots,
                   std::vector<InstructionPatch>& patches, bool& other_fields) {
  other_fields = false;
  patches.clear();
  std::vector<bool> patchable(dwords, false);
  for (uint32_t slot : fetch_slots) {
    for (uint32_t i = 0; i < 3 && slot * 3 + i < dwords; ++i) patchable[slot * 3 + i] = true;
  }
  std::vector<uint32_t> export_slots;
  for (uint32_t slot : alu_slots) {
    if (slot * 3 + 2 >= dwords || !((Swap(original[slot * 3]) | Swap(patched[slot * 3])) & kExportBit)) continue;
    export_slots.push_back(slot);
    for (uint32_t i = 0; i < 3; ++i) patchable[slot * 3 + i] = true;
  }
  for (uint32_t i = 0; i < dwords; ++i) {
    if (original[i] != patched[i] && !patchable[i]) return false;
  }
  for (uint32_t slot : fetch_slots) {
    if (slot * 3 + 2 >= dwords) continue;
    for (uint32_t d = 0; d < 3; ++d) {
      uint32_t a = Swap(original[slot * 3 + d]), b = Swap(patched[slot * 3 + d]);
      if ((a ^ b) & ~PatchableBits(d)) {
        other_fields = true;
        return false;
      }
    }
    InstructionPatch patch = DiffFields(InstructionPatch::Kind::kVertexFetch, slot, original, patched);
    if (patch.fields) patches.push_back(std::move(patch));
  }
  for (uint32_t slot : export_slots) {
    InstructionPatch patch = DiffFields(InstructionPatch::Kind::kAluExport, slot, original, patched);
    if (patch.fields) patches.push_back(std::move(patch));
  }
  return true;
}

void ApplyPatches(std::vector<uint32_t>& ucode, const std::vector<InstructionPatch>& patches) {
  for (const InstructionPatch& patch : patches) {
    const FieldTable table = Table(patch.kind);
    size_t value = 0;
    for (uint32_t f = 0; f < table.count; ++f) {
      if (!(patch.fields & (1u << f)) || value >= patch.values.size()) continue;
      const Field& field = table.fields[f];
      size_t index = size_t(patch.slot) * 3 + field.dword;
      if (index >= ucode.size()) continue;
      uint32_t host = Swap(ucode[index]);
      host = (host & ~Mask(field)) | ((patch.values[value++] << field.low) & Mask(field));
      ucode[index] = Swap(host);
    }
  }
}

}  // namespace

bool GameShaders::Load(const std::filesystem::path& root, const Profile& profile) {
  for (uint32_t i = 0; i < profile.shader_file_count; ++i) {
    std::filesystem::path path = root / profile.shader_files[i];
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      REXLOG_WARN("[native_x_renderer] pipeline list: the game's shader file {} is missing", path.string());
      continue;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() >= 4 && !std::memcmp(data.data(), "CHNK", 4)) {
      std::vector<uint8_t> decompressed = DecompressChnk(data);
      if (decompressed.empty()) {
        REXLOG_WARN("[native_x_renderer] pipeline list: {} could not be decompressed", path.string());
        continue;
      }
      data = std::move(decompressed);
    }
    const size_t found = Scan(data.data(), data.size());
    REXLOG_INFO("[native_x_renderer] pipeline list: {} shader containers in {}", found, path.string());
  }
  if (profile.read_shader_archives) {
    const size_t before = containers_.size();
    size_t buffers = 0, found = 0, bytes = 0;
    profile.read_shader_archives(root, [&](const uint8_t* data, size_t size) {
      ++buffers;
      bytes += size;
      found += Scan(data, size);
    });
    REXLOG_INFO("[native_x_renderer] pipeline list: {} shader containers in {} buffers ({} MB) from the game's archives, "
                "{} programs not found before",
                found, buffers, bytes >> 20, containers_.size() - before);
  }
  return !containers_.empty();
}

size_t GameShaders::Scan(const uint8_t* d, size_t size) {
  size_t found = 0;
  // Containers start on a dword; their header: magic, header size (= the microcode section's offset), section size.
  for (size_t at = 0; at + 12 <= size; at += 4) {
    uint32_t magic;
    std::memcpy(&magic, d + at, 4);
    magic = Swap(magic);
    if ((magic & kContainerMagicMask) != kContainerMagic) continue;
    uint32_t header_size, section_size;
    std::memcpy(&header_size, d + at + 4, 4);
    std::memcpy(&section_size, d + at + 8, 4);
    header_size = Swap(header_size);
    section_size = Swap(section_size);
    if (header_size < 12 || header_size > 0x10000 || !section_size || section_size % 4 ||
        at + uint64_t(header_size) + section_size > size || (at + header_size) % 4) {
      continue;
    }
    ++found;
    const uint8_t type = (magic & 1) ? 0 : 1;
    const uint8_t* section = d + at + header_size;
    if (!kept_.insert(XXH3_64bits(section, section_size) ^ type).second) continue;
    Container container{type, std::vector<uint32_t>(section_size / 4)};
    std::memcpy(container.section.data(), section, section_size);
    containers_.push_back(std::move(container));
    index_.clear();
  }
  return found;
}

std::vector<const uint32_t*> GameShaders::Candidates(uint8_t type, uint32_t dwords) const {
  std::vector<const uint32_t*> candidates;
  for (const Container& c : containers_) {
    if (c.type == type && c.section.size() >= dwords) candidates.push_back(c.section.data() + (c.section.size() - dwords));
  }
  return candidates;
}

const uint32_t* GameShaders::Find(uint8_t type, uint32_t dwords, uint64_t hash) {
  uint64_t key = (uint64_t(type) << 32) | dwords;
  auto it = index_.find(key);
  if (it == index_.end()) {
    std::unordered_map<uint64_t, const uint32_t*> tails;
    for (const uint32_t* tail : Candidates(type, dwords)) tails.emplace(Hash(tail, dwords), tail);
    it = index_.emplace(key, std::move(tails)).first;
  }
  auto found = it->second.find(hash);
  return found != it->second.end() ? found->second : nullptr;
}

void ExecSlots(const uint32_t* be, uint32_t dwords, std::vector<uint32_t>* vertex_fetch, std::vector<uint32_t>* alu) {
  std::vector<uint32_t> slots, alu_slots;
  // Control flow comes first, two instructions in three dwords, up to the first instructions an exec runs.
  uint32_t bound = dwords;
  for (uint32_t i = 0; i + 3 <= bound; i += 3) {
    uint32_t words[3] = {Swap(be[i]), Swap(be[i + 1]), Swap(be[i + 2])};
    ucode::ControlFlowInstruction cf[2];
    ucode::UnpackControlFlowInstructions(words, cf);
    for (const ucode::ControlFlowInstruction& c : cf) {
      uint32_t address = 0, count = 0, sequence = 0;
      switch (c.opcode()) {
        case ucode::ControlFlowOpcode::kExec:
        case ucode::ControlFlowOpcode::kExecEnd:
          address = c.exec.address(), count = c.exec.count(), sequence = c.exec.sequence();
          break;
        case ucode::ControlFlowOpcode::kCondExec:
        case ucode::ControlFlowOpcode::kCondExecEnd:
          address = c.cond_exec.address(), count = c.cond_exec.count(), sequence = c.cond_exec.sequence();
          break;
        case ucode::ControlFlowOpcode::kCondExecPred:
        case ucode::ControlFlowOpcode::kCondExecPredEnd:
        case ucode::ControlFlowOpcode::kCondExecPredClean:
        case ucode::ControlFlowOpcode::kCondExecPredCleanEnd:
          address = c.cond_exec_pred.address(), count = c.cond_exec_pred.count(),
          sequence = c.cond_exec_pred.sequence();
          break;
        default:
          continue;
      }
      if (!count) continue;
      bound = std::min(bound, address * 3);
      for (uint32_t k = 0; k < count; ++k) {
        uint32_t slot = address + k;
        if (slot * 3 + 2 >= dwords) continue;
        if (!((sequence >> (k * 2)) & 1)) {
          alu_slots.push_back(slot);
        } else if ((Swap(be[slot * 3]) & 0x1F) == uint32_t(ucode::FetchOpcode::kVertexFetch)) {
          slots.push_back(slot);
        }
      }
    }
  }
  for (std::vector<uint32_t>* list : {&slots, &alu_slots}) {
    std::sort(list->begin(), list->end());
    list->erase(std::unique(list->begin(), list->end()), list->end());
  }
  if (vertex_fetch) *vertex_fetch = std::move(slots);
  if (alu) *alu = std::move(alu_slots);
}

bool PipelineList::Write(const std::filesystem::path& path) const {
  std::vector<uint8_t> out;
  Header header{kMagic, kListVersion, PipelineStorage::kVersion, uint32_t(sizeof(PipelineState)), title_id, {}};
  Put(out, header);
  for (const ListShader& s : shaders) {
    out.push_back(kShaderTag);
    Put(out, s.type);
    Put(out, s.dwords);
    Put(out, s.hash);
    Put(out, s.source_hash);
    Put(out, uint16_t(s.patches.size()));
    for (const InstructionPatch& p : s.patches) {
      const bool alu = p.kind == InstructionPatch::Kind::kAluExport;
      Put(out, uint16_t(p.slot | (alu ? kAluExportSlot : 0)));
      Put(out, p.fields);
      for (uint32_t value : p.values) Put(out, value);
    }
  }
  for (const auto& t : translations) {
    out.push_back(kTranslationTag);
    Put(out, t.type);
    Put(out, t.hash);
    Put(out, t.modification);
  }
  for (const auto& p : pipelines) {
    out.push_back(kPipelineTag);
    Put(out, p);
  }
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    if (!file) return false;
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  return !error;
}

bool PipelineList::Read(const std::filesystem::path& path, std::string& error) {
  std::vector<uint8_t> data;
  {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      error = "missing";
      return false;
    }
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  Header header{};
  size_t pos = 0;
  if (!Get(data, pos, header) || header.magic != kMagic) {
    error = "not a pipeline list";
    return false;
  }
  if ((header.version != 1 && header.version != kListVersion) || header.storage_version != PipelineStorage::kVersion ||
      header.state_size != sizeof(PipelineState)) {
    error = "made for another NativeXRenderer version";
    return false;
  }
  const bool version_1 = header.version == 1;  // vertex fetch patches only, their fields as 16 bits
  title_id = header.title_id;
  while (pos < data.size()) {
    uint8_t tag = data[pos++];
    if (tag == kShaderTag) {
      ListShader s;
      uint16_t patch_count = 0;
      if (!Get(data, pos, s.type) || !Get(data, pos, s.dwords) || !Get(data, pos, s.hash) ||
          !Get(data, pos, s.source_hash) || !Get(data, pos, patch_count)) {
        break;
      }
      bool ok = true;
      for (uint16_t i = 0; i < patch_count && ok; ++i) {
        InstructionPatch p;
        if (version_1) {
          uint16_t fields = 0;
          ok = Get(data, pos, p.slot) && Get(data, pos, fields);
          p.fields = fields;
        } else {
          ok = Get(data, pos, p.slot) && Get(data, pos, p.fields);
          if (p.slot & kAluExportSlot) {
            p.kind = InstructionPatch::Kind::kAluExport;
            p.slot &= uint16_t(~kAluExportSlot);
          }
        }
        const FieldTable table = Table(p.kind);
        for (uint32_t f = 0; ok && f < table.count; ++f) {
          if (!(p.fields & (1u << f))) continue;
          uint32_t value = 0;
          ok = Get(data, pos, value);
          p.values.push_back(value);
        }
        s.patches.push_back(std::move(p));
      }
      if (!ok) break;
      shaders.push_back(std::move(s));
    } else if (tag == kTranslationTag) {
      PipelineStorage::Translation t;
      if (!Get(data, pos, t.type) || !Get(data, pos, t.hash) || !Get(data, pos, t.modification)) break;
      translations.push_back(t);
    } else if (tag == kPipelineTag) {
      PipelineStorage::PipelineRecord p;
      if (!Get(data, pos, p)) break;
      p.state.vertex_shader = nullptr;
      p.state.pixel_shader = nullptr;
      pipelines.push_back(p);
    } else {
      error = "damaged";
      return false;
    }
  }
  return true;
}

bool MakePipelineList(GameShaders& game, uint32_t title_id, const std::vector<std::filesystem::path>& storages,
                      PipelineList& list, MakeReport& report) {
  list = {};
  list.title_id = title_id;
  std::vector<PipelineStorage::Shader> shaders;
  std::vector<PipelineStorage::Translation> translations;
  std::vector<PipelineStorage::PipelineRecord> pipelines;
  for (const auto& path : storages) {
    if (!PipelineStorage::Read(path, shaders, translations, pipelines)) {
      REXLOG_ERROR("[native_x_renderer] pipeline list: {} is missing or of another version", path.string());
      return false;
    }
    ++report.storages;
  }
  // The shaders, once each, those the game's files have.
  std::unordered_set<uint64_t> seen, kept;
  auto shader_key = [](uint8_t type, uint64_t hash) { return hash ^ (0x9E3779B97F4A7C15ull * (uint64_t(type) + 1)); };
  for (const auto& s : shaders) {
    if (!seen.insert(shader_key(s.type, s.hash)).second) continue;
    ++report.shaders;
    const uint32_t dwords = uint32_t(s.ucode.size());
    ListShader entry{s.type, dwords, s.hash, s.hash, {}};
    bool found = false;
    if (s.type == 1) {
      found = game.Find(1, dwords, s.hash) != nullptr;
      report.pixel_found += found;
    } else {
      std::vector<uint32_t> fetch_slots, alu_slots;
      ExecSlots(s.ucode.data(), dwords, &fetch_slots, &alu_slots);
      bool other_fields = false;
      size_t best_values = SIZE_MAX;
      for (const uint32_t* candidate : game.Candidates(0, dwords)) {
        std::vector<InstructionPatch> patches;
        bool other = false;
        if (!DerivePatches(candidate, s.ucode.data(), dwords, fetch_slots, alu_slots, patches, other)) {
          other_fields |= other;
          continue;
        }
        std::vector<uint32_t> rebuilt(candidate, candidate + dwords);
        ApplyPatches(rebuilt, patches);
        if (Hash(rebuilt.data(), dwords) != s.hash) continue;
        // The program the fewest fields away: the same shader, where the files have several that could be patched into
        // it (variants that differ only in their exports).
        size_t values = 0;
        for (const InstructionPatch& p : patches) values += p.values.size();
        if (values >= best_values) continue;
        best_values = values;
        entry.source_hash = Hash(candidate, dwords);
        entry.patches = std::move(patches);
        found = true;
        if (!values) break;
      }
      if (!found && other_fields) ++report.other_fields;
      report.vertex_found += found;
      report.vertex_linked += found && std::any_of(entry.patches.begin(), entry.patches.end(), [](const auto& p) {
                                return p.kind == InstructionPatch::Kind::kAluExport;
                              });
    }
    if (!found) {
      ++report.not_found;
      REXLOG_WARN("[native_x_renderer] pipeline list: {} shader {:016X} ({} dwords) left out: {}",
                  s.type ? "pixel" : "vertex", s.hash, dwords, "not in the game's shader files");
      continue;
    }
    kept.insert(shader_key(s.type, s.hash));
    list.shaders.push_back(std::move(entry));
  }
  std::unordered_set<uint64_t> translation_keys, pipeline_keys;
  for (const auto& t : translations) {
    const uint64_t fields[3] = {t.type, t.hash, t.modification};
    if (!translation_keys.insert(XXH3_64bits(fields, sizeof(fields))).second) continue;
    if (!kept.count(shader_key(t.type, t.hash))) {
      ++report.dropped_translations;
      continue;
    }
    list.translations.push_back(t);
  }
  for (const auto& p : pipelines) {
    if (!pipeline_keys.insert(XXH3_64bits(&p, sizeof(p))).second) continue;
    if (!kept.count(shader_key(0, p.vertex_hash)) || (p.pixel_hash && !kept.count(shader_key(1, p.pixel_hash)))) {
      ++report.dropped_pipelines;
      continue;
    }
    list.pipelines.push_back(p);
  }
  report.translations = uint32_t(list.translations.size());
  report.pipelines = uint32_t(list.pipelines.size());
  return true;
}

void ResolvePipelineList(const PipelineList& list, GameShaders& game, std::vector<PipelineStorage::Shader>& shaders,
                         std::vector<PipelineStorage::Translation>& translations,
                         std::vector<PipelineStorage::PipelineRecord>& pipelines, ResolveReport& report) {
  std::unordered_set<uint64_t> kept;
  auto shader_key = [](uint8_t type, uint64_t hash) { return hash ^ (0x9E3779B97F4A7C15ull * (uint64_t(type) + 1)); };
  for (const ListShader& s : list.shaders) {
    const uint32_t* source = game.Find(s.type, s.dwords, s.source_hash);
    if (!source) {
      ++report.missing;
      continue;
    }
    PipelineStorage::Shader shader{s.type, s.hash, std::vector<uint32_t>(source, source + s.dwords)};
    ApplyPatches(shader.ucode, s.patches);
    if (Hash(shader.ucode.data(), s.dwords) != s.hash) {
      ++report.mismatched;
      continue;
    }
    kept.insert(shader_key(s.type, s.hash));
    shaders.push_back(std::move(shader));
    ++report.shaders;
  }
  for (const auto& t : list.translations) {
    if (!kept.count(shader_key(t.type, t.hash))) continue;
    translations.push_back(t);
    ++report.translations;
  }
  for (const auto& p : list.pipelines) {
    if (!kept.count(shader_key(0, p.vertex_hash)) || (p.pixel_hash && !kept.count(shader_key(1, p.pixel_hash)))) {
      continue;
    }
    pipelines.push_back(p);
    ++report.pipelines;
  }
}

}  // namespace native_x_renderer
