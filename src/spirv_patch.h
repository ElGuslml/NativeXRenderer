// XenosNative - small fix-ups to SPIR-V modules before DiligentCore reflects them (internal, D18).
//
// DiligentCore refuses a module without OpSource and binds resources by name. The SDK's translator emits
// names but no OpSource; the SDK's precompiled utility shaders (texture loading) are stripped of both.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xenos_native::spirv {

// A resource to name: the variable decorated with this descriptor set and binding, or, with
// push_constants, the push constant block. The variable and its block type get the name (DiligentCore
// names buffers by their block type).
struct ResourceName {
  uint32_t set = 0, binding = 0;
  bool push_constants = false;
  std::string name;
};

// Returns the module with "OpSource Unknown 0" (if it has no OpSource) and OpName instructions for the
// resources listed, in the debug section.
std::vector<uint32_t> PrepareForDiligent(const uint32_t* words, size_t word_count,
                                         const std::vector<ResourceName>& names = {});

}  // namespace xenos_native::spirv
