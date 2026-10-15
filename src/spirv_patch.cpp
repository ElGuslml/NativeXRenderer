// NativeXRenderer - see spirv_patch.h.

#include "spirv_patch.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <unordered_map>

namespace native_x_renderer::spirv {

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

namespace {

// More of the specification, for the motion vertex shader.
constexpr uint32_t kOpSourceContinued = 2, kOpSourceExtension = 4, kOpMemberName = 6, kOpString = 7,
                   kOpTypeVoid = 19, kOpTypeBool = 20, kOpTypeInt = 21, kOpTypeFloat = 22, kOpTypeVector = 23,
                   kOpTypeStruct = 30, kOpConstantTrue = 41, kOpConstantFalse = 42, kOpConstant = 43,
                   kOpFunction = 54, kOpFunctionEnd = 56, kOpFunctionCall = 57, kOpLoad = 61, kOpStore = 62,
                   kOpAccessChain = 65, kOpInBoundsAccessChain = 66, kOpMemberDecorate = 72, kOpDecorationGroup = 73,
                   kOpGroupDecorate = 74, kOpGroupMemberDecorate = 75, kOpSelect = 169, kOpLabel = 248,
                   kOpReturn = 253, kOpModuleProcessed = 330, kOpDecorateId = 332, kOpDecorateString = 5632,
                   kOpMemberDecorateString = 5633;
constexpr uint32_t kDecorationBlock = 2, kDecorationBuiltIn = 11, kDecorationLocation = 30, kDecorationOffset = 35;
constexpr uint32_t kBuiltInPosition = 0;
constexpr uint32_t kStorageClassUniform = 2, kStorageClassOutput = 3, kStorageClassPrivate = 6;

void Append(std::vector<uint32_t>& out, uint32_t opcode, std::initializer_list<uint32_t> operands) {
  out.push_back((uint32_t(operands.size() + 1) << 16) | opcode);
  out.insert(out.end(), operands);
}

// A literal string operand, up to its nul.
std::string ReadString(const uint32_t* operand, size_t words) {
  std::string string;
  for (size_t i = 0; i < words * 4; ++i) {
    char c = char((operand[i / 4] >> (8 * (i % 4))) & 0xFF);
    if (!c) break;
    string += c;
  }
  return string;
}

}  // namespace

std::vector<uint32_t> MakeMotionVertexShader(const uint32_t* words, size_t word_count, uint32_t previous_location,
                                             uint32_t current_location) {
  if (word_count < kHeaderWords || words[1] < 0x00010400) return {};
  uint32_t bound = words[3];

  // What the patch needs, in one pass.
  uint32_t float_constants = 0, main_function = 0, main_type = 0;
  uint32_t type_void = 0, type_bool = 0, type_float = 0, type_vec4 = 0, type_int = 0;
  uint32_t position_struct = 0, position_member = 0;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;  // pointer type -> storage class, pointee
  std::unordered_map<uint32_t, uint32_t> variables;                      // variable -> pointer type
  std::unordered_map<uint32_t, std::vector<uint32_t>> structs;           // struct type -> member types
  std::unordered_map<uint32_t, uint32_t> sets, bindings, locations;      // variable -> decoration
  for (size_t i = kHeaderWords; i < word_count;) {
    uint32_t opcode = words[i] & 0xFFFF, count = words[i] >> 16;
    if (!count || i + count > word_count) return {};
    const uint32_t* op = words + i + 1;
    switch (opcode) {
      case kOpName:
        if (count >= 3 && ReadString(op + 1, count - 2) == "xe_uniform_float_constants") float_constants = op[0];
        break;
      case kOpEntryPoint:
        if (count >= 3) main_function = op[1];
        break;
      case kOpTypeVoid: type_void = op[0]; break;
      case kOpTypeBool: type_bool = op[0]; break;
      case kOpTypeFloat:
        if (op[1] == 32) type_float = op[0];
        break;
      case kOpTypeInt:
        if (op[1] == 32 && !type_int) type_int = op[0];
        break;
      case kOpTypeVector:
        if (type_float && op[1] == type_float && op[2] == 4) type_vec4 = op[0];
        break;
      case kOpTypeStruct: structs[op[0]].assign(op + 1, op + count - 1); break;
      case kOpTypePointer:
        if (count >= 4) pointers[op[0]] = {op[1], op[2]};
        break;
      case kOpVariable:
        if (count >= 4) variables[op[1]] = op[0];
        break;
      case kOpDecorate:
        if (count >= 4 && op[1] == kDecorationDescriptorSet) sets[op[0]] = op[2];
        if (count >= 4 && op[1] == kDecorationBinding) bindings[op[0]] = op[2];
        if (count >= 4 && op[1] == kDecorationLocation) locations[op[0]] = op[2];
        break;
      case kOpMemberDecorate:
        if (count >= 5 && op[2] == kDecorationBuiltIn && op[3] == kBuiltInPosition) {
          position_struct = op[0];
          position_member = op[1];
        }
        break;
      case kOpFunction:
        if (count >= 5 && op[1] == main_function) main_type = op[3];
        break;
      default: break;
    }
    i += count;
  }
  auto pointee = [&](uint32_t variable, uint32_t storage_class) -> uint32_t {
    auto type = variables.find(variable);
    if (type == variables.end()) return 0;
    auto pointer = pointers.find(type->second);
    return pointer != pointers.end() && pointer->second.first == storage_class ? pointer->second.second : 0;
  };
  uint32_t float_constants_struct = pointee(float_constants, kStorageClassUniform);
  auto float_constants_members = structs.find(float_constants_struct);
  uint32_t position_variable = 0;
  for (const auto& [variable, type] : variables) {
    uint32_t output = pointee(variable, kStorageClassOutput);
    if (position_struct && output == position_struct) position_variable = variable;
    auto location = locations.find(variable);
    if (output && location != locations.end() &&
        (location->second == previous_location || location->second == current_location)) {
      return {};
    }
  }
  if (!float_constants_struct || float_constants_members == structs.end() ||
      float_constants_members->second.size() != 1 || !sets.count(float_constants) || !position_variable ||
      !main_function || !main_type || !type_void || !type_bool || !type_vec4 || !type_int) {
    return {};
  }
  // A binding of its own in the float constants' set (DiligentCore assigns its own when it creates the
  // pipeline; the module's only has to be unique).
  uint32_t set = sets[float_constants], binding = 0;
  for (const auto& [variable, variable_set] : sets) {
    auto variable_binding = bindings.find(variable);
    if (variable_set == set && variable_binding != bindings.end()) {
      binding = std::max(binding, variable_binding->second + 1);
    }
  }

  uint32_t struct_previous = bound++, pointer_previous = bound++, variable_previous = bound++;
  uint32_t pointer_bool = bound++, constant_true = bound++, constant_false = bound++, flag = bound++;
  uint32_t pointer_output = bound++, output_previous = bound++, output_current = bound++, constant_member = bound++;
  uint32_t new_main = bound++;

  // The module again, by section (SPIR-V specification, 2.4), with the loads of float constants turned
  // into selects.
  std::vector<uint32_t> preamble, debug, processed, annotations, globals, functions;
  // A pointer into the float constants -> the same pointer into the previous block.
  std::unordered_map<uint32_t, uint32_t> previous_chains;
  bool in_functions = false;
  for (size_t i = kHeaderWords; i < word_count;) {
    uint32_t opcode = words[i] & 0xFFFF, count = words[i] >> 16;
    const uint32_t* instruction = words + i;
    const uint32_t* op = instruction + 1;
    i += count;
    if (opcode == kOpFunction) in_functions = true;
    if (in_functions) {
      if ((opcode == kOpAccessChain || opcode == kOpInBoundsAccessChain) && count >= 4 && op[2] == float_constants) {
        uint32_t previous = bound++;
        functions.insert(functions.end(), instruction, instruction + count);
        functions.insert(functions.end(), instruction, instruction + count);
        functions[functions.size() - count + 2] = previous;
        functions[functions.size() - count + 3] = variable_previous;
        previous_chains[op[1]] = previous;
        continue;
      }
      if (opcode == kOpLoad && count >= 4 && previous_chains.count(op[2])) {
        uint32_t current = bound++, previous = bound++, use_previous = bound++;
        functions.push_back(instruction[0]);
        functions.insert(functions.end(), {op[0], current, op[2]});
        functions.insert(functions.end(), op + 3, op + count - 1);  // memory operands
        functions.push_back(instruction[0]);
        functions.insert(functions.end(), {op[0], previous, previous_chains[op[2]]});
        functions.insert(functions.end(), op + 3, op + count - 1);
        Append(functions, kOpLoad, {type_bool, use_previous, flag});
        Append(functions, kOpSelect, {op[0], op[1], use_previous, previous, current});
        continue;
      }
      // Any other use of the float constants or of a pointer into them (or a literal operand with the
      // same value): the shader gets no motion, which is safe.
      for (uint32_t k = 0; k + 1 < count; ++k) {
        if (op[k] == float_constants || previous_chains.count(op[k])) return {};
      }
      functions.insert(functions.end(), instruction, instruction + count);
      continue;
    }
    switch (opcode) {
      case kOpCapability: case kOpExtension: case kOpExtInstImport: case kOpMemoryModel:
        preamble.insert(preamble.end(), instruction, instruction + count);
        break;
      case kOpEntryPoint: {
        // The new entry point, with the new variables in its interface (since SPIR-V 1.4, every global
        // variable it uses).
        size_t start = preamble.size();
        preamble.insert(preamble.end(), instruction, instruction + count);
        preamble[start + 2] = new_main;
        preamble.insert(preamble.end(), {variable_previous, flag, output_previous, output_current});
        preamble[start] = (uint32_t(preamble.size() - start) << 16) | kOpEntryPoint;
        break;
      }
      case kOpExecutionMode: case kOpExecutionModeId: {
        size_t start = preamble.size();
        preamble.insert(preamble.end(), instruction, instruction + count);
        if (preamble[start + 1] == main_function) preamble[start + 1] = new_main;
        break;
      }
      case kOpSourceContinued: case kOpSource: case kOpSourceExtension: case kOpName: case kOpMemberName:
      case kOpString:
        debug.insert(debug.end(), instruction, instruction + count);
        break;
      case kOpModuleProcessed:
        processed.insert(processed.end(), instruction, instruction + count);
        break;
      case kOpDecorate: case kOpMemberDecorate: case kOpDecorationGroup: case kOpGroupDecorate:
      case kOpGroupMemberDecorate: case kOpDecorateId: case kOpDecorateString: case kOpMemberDecorateString:
        annotations.insert(annotations.end(), instruction, instruction + count);
        break;
      default:
        globals.insert(globals.end(), instruction, instruction + count);
        break;
    }
  }
  if (previous_chains.empty()) return {};

  // The previous block: the layout of XeFloatConstants (its array type, with its stride, is shared).
  AppendName(debug, struct_previous, "XeFloatConstantsPrevious");
  AppendName(debug, variable_previous, "xe_uniform_float_constants_previous");
  AppendName(debug, output_previous, "xe_out_motion_previous");
  AppendName(debug, output_current, "xe_out_motion_current");
  Append(annotations, kOpDecorate, {struct_previous, kDecorationBlock});
  Append(annotations, kOpMemberDecorate, {struct_previous, 0, kDecorationOffset, 0});
  Append(annotations, kOpDecorate, {variable_previous, kDecorationDescriptorSet, set});
  Append(annotations, kOpDecorate, {variable_previous, kDecorationBinding, binding});
  Append(annotations, kOpDecorate, {output_previous, kDecorationLocation, previous_location});
  Append(annotations, kOpDecorate, {output_current, kDecorationLocation, current_location});
  Append(globals, kOpTypeStruct, {struct_previous, float_constants_members->second[0]});
  Append(globals, kOpTypePointer, {pointer_previous, kStorageClassUniform, struct_previous});
  Append(globals, kOpVariable, {pointer_previous, variable_previous, kStorageClassUniform});
  Append(globals, kOpTypePointer, {pointer_bool, kStorageClassPrivate, type_bool});
  Append(globals, kOpConstantTrue, {type_bool, constant_true});
  Append(globals, kOpConstantFalse, {type_bool, constant_false});
  Append(globals, kOpVariable, {pointer_bool, flag, kStorageClassPrivate, constant_false});
  Append(globals, kOpTypePointer, {pointer_output, kStorageClassOutput, type_vec4});
  Append(globals, kOpVariable, {pointer_output, output_previous, kStorageClassOutput});
  Append(globals, kOpVariable, {pointer_output, output_current, kStorageClassOutput});
  Append(globals, kOpConstant, {type_int, constant_member, position_member});

  // The entry point: the shader with the last frame's constants, its position kept, then with this frame's.
  uint32_t label = bound++, call_previous = bound++, call_current = bound++, position_pointer = bound++;
  uint32_t position_previous = bound++, position_current = bound++;
  Append(functions, kOpFunction, {type_void, new_main, 0 /* FunctionControlMaskNone */, main_type});
  Append(functions, kOpLabel, {label});
  Append(functions, kOpStore, {flag, constant_true});
  Append(functions, kOpFunctionCall, {type_void, call_previous, main_function});
  Append(functions, kOpAccessChain, {pointer_output, position_pointer, position_variable, constant_member});
  Append(functions, kOpLoad, {type_vec4, position_previous, position_pointer});
  Append(functions, kOpStore, {output_previous, position_previous});
  Append(functions, kOpStore, {flag, constant_false});
  Append(functions, kOpFunctionCall, {type_void, call_current, main_function});
  Append(functions, kOpLoad, {type_vec4, position_current, position_pointer});
  Append(functions, kOpStore, {output_current, position_current});
  Append(functions, kOpReturn, {});
  Append(functions, kOpFunctionEnd, {});

  std::vector<uint32_t> module(words, words + kHeaderWords);
  module[3] = bound;
  for (const std::vector<uint32_t>* section : {&preamble, &debug, &processed, &annotations, &globals, &functions}) {
    module.insert(module.end(), section->begin(), section->end());
  }
  return module;
}

}  // namespace native_x_renderer::spirv
