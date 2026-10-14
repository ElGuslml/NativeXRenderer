// XenosNative - see spirv_patch.h.

#include "spirv_patch.h"

#include <unordered_map>

namespace xenos_native::spirv {

namespace {

constexpr uint32_t kHeaderWords = 5;

// Opcodes and operands used here (SPIR-V 1.0 specification).
constexpr uint32_t kOpSource = 3, kOpName = 5, kOpExtension = 10, kOpExtInstImport = 11, kOpMemoryModel = 14,
                   kOpEntryPoint = 15, kOpExecutionMode = 16, kOpCapability = 17, kOpTypePointer = 32,
                   kOpVariable = 59, kOpDecorate = 71, kOpExecutionModeId = 331;
constexpr uint32_t kDecorationBinding = 33, kDecorationDescriptorSet = 34;
constexpr uint32_t kStorageClassPushConstant = 9;

void AppendName(std::vector<uint32_t>& out, uint32_t id, const std::string& name) {
  // The literal string is nul-terminated and padded to whole words.
  uint32_t string_words = uint32_t(name.size() / 4 + 1);
  out.push_back(((2 + string_words) << 16) | kOpName);
  out.push_back(id);
  size_t first = out.size();
  out.resize(first + string_words, 0);
  for (size_t i = 0; i < name.size(); ++i) {
    out[first + i / 4] |= uint32_t(uint8_t(name[i])) << (8 * (i % 4));
  }
}

}  // namespace

std::vector<uint32_t> PrepareForDiligent(const uint32_t* words, size_t word_count,
                                         const std::vector<ResourceName>& names) {
  std::vector<uint32_t> module(words, words + word_count);
  if (word_count < kHeaderWords) return module;

  // One pass: where debug instructions go, whether there is an OpSource, and what to name.
  size_t debug_position = 0;
  bool has_source = false;
  std::unordered_map<uint32_t, uint32_t> sets, bindings;  // variable id -> decoration
  std::unordered_map<uint32_t, uint32_t> pointee;         // pointer type id -> pointee type id
  struct Variable {
    uint32_t id, pointer_type, storage_class;
  };
  std::vector<Variable> variables;
  for (size_t i = kHeaderWords; i < word_count;) {
    uint32_t opcode = words[i] & 0xFFFF, count = words[i] >> 16;
    if (!count || i + count > word_count) break;
    const uint32_t* op = words + i + 1;
    bool preamble = opcode == kOpCapability || opcode == kOpExtension || opcode == kOpExtInstImport ||
                    opcode == kOpMemoryModel || opcode == kOpEntryPoint || opcode == kOpExecutionMode ||
                    opcode == kOpExecutionModeId;
    if (!preamble && !debug_position) debug_position = i;
    if (opcode == kOpSource) has_source = true;
    if (opcode == kOpDecorate && count >= 4) {
      if (op[1] == kDecorationDescriptorSet) sets[op[0]] = op[2];
      if (op[1] == kDecorationBinding) bindings[op[0]] = op[2];
    }
    if (opcode == kOpTypePointer && count >= 4) pointee[op[0]] = op[2];
    if (opcode == kOpVariable && count >= 4) variables.push_back({op[1], op[0], op[2]});
    i += count;
  }
  if (!debug_position) debug_position = word_count;

  std::vector<uint32_t> inserted;
  if (!has_source) {
    inserted.insert(inserted.end(), {(3u << 16) | kOpSource, 0 /* SourceLanguageUnknown */, 0 /* version */});
  }
  for (const ResourceName& resource : names) {
    for (const Variable& variable : variables) {
      bool match;
      if (resource.push_constants) {
        match = variable.storage_class == kStorageClassPushConstant;
      } else {
        auto set = sets.find(variable.id);
        auto binding = bindings.find(variable.id);
        match = set != sets.end() && binding != bindings.end() && set->second == resource.set &&
                binding->second == resource.binding;
      }
      if (!match) continue;
      AppendName(inserted, variable.id, resource.name);
      auto type = pointee.find(variable.pointer_type);
      if (type != pointee.end()) AppendName(inserted, type->second, resource.name);
    }
  }
  // Debug instructions come in three groups: OpString/OpSource..., then OpName/OpMemberName, then
  // OpModuleProcessed. Inserting before the first non-preamble instruction keeps that order for the two
  // kinds of modules here: the translator's (names, no OpSource; only OpSource is added) and the stripped
  // utility shaders (no debug instructions at all).
  module.insert(module.begin() + debug_position, inserted.begin(), inserted.end());
  return module;
}

}  // namespace xenos_native::spirv
