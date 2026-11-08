// NativeXRenderer - small fix-ups to SPIR-V modules before DiligentCore reflects them (internal, D18).
//
// DiligentCore refuses a module without OpSource and binds resources by name. The SDK's translator emits
// names but no OpSource; the SDK's precompiled utility shaders (texture loading) are stripped of both.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace native_x_renderer::spirv {

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

// The translator's textures and samplers of descriptor set `set` named by their place, for the game pipelines'
// one resource signature (D131): the image at binding b becomes "<prefix>t<b>", the sampler at binding T + j (the
// translator puts its samplers after its T images) "<prefix>s<j>"; each variable's own name is replaced. False, and
// the module unchanged, when the set holds more than `max_textures` images or `max_samplers` samplers, or bindings
// out of that order.
bool NamePositionally(std::vector<uint32_t>& module, uint32_t set, const std::string& prefix, uint32_t max_textures,
                      uint32_t max_samplers);

// The motion vertex shader of a translated guest vertex shader (D48): the same shader run twice, first
// with the last frame's float constants (a second uniform block, XeFloatConstantsPrevious, variable
// xe_uniform_float_constants_previous, in the float constants' descriptor set), then with this frame's.
// Its position output is this frame's; the two positions in clip space are also written to the vec4
// outputs at `previous_location` and `current_location`. Each load of a float constant becomes a select
// between the two blocks on a private flag, which the entry point sets around the two calls (the driver
// inlines them and folds the flag: each call reads one block).
// Returns an empty vector for a module this cannot patch: SPIR-V before 1.4 (a select with a scalar
// condition on vectors), no xe_uniform_float_constants, no position output, those locations in use, or a
// float constant pointer used other than by OpLoad.
std::vector<uint32_t> MakeMotionVertexShader(const uint32_t* words, size_t word_count, uint32_t previous_location,
                                             uint32_t current_location);

}  // namespace native_x_renderer::spirv
