// XenosNative - forwarding the SDK graphics cvars to the GPU plugin's registration (internal).
// See sdk_cvars.cpp.

#pragma once

#include <rex/cvar.h>

#include <string>
#include <type_traits>

namespace xenos_native::sdk {

// The value the GPU plugin registered under `name` (or `fallback` before it is registered). Returned by
// reference, as the SDK's cvar accessors are: the reference is to a per-thread copy, refreshed on every
// call.
template <typename T>
T& Forward(const char* name, T fallback) {
  thread_local T value;
  value = rex::cvar::Query<T>(name);
  if constexpr (std::is_same_v<T, std::string>) {
    if (value.empty()) value = fallback;
  }
  (void)fallback;
  return value;
}

}  // namespace xenos_native::sdk

// An SDK source compiled into XenosNative defines its cvars through these: accessors that forward to the
// plugin's registered value, and no registration.
#undef REXCVAR_DEFINE_BOOL
#define REXCVAR_DEFINE_BOOL(name, default_val, category, desc) \
  bool& FLAGS_##name##_storage_() { return ::xenos_native::sdk::Forward<bool>(#name, default_val); }
