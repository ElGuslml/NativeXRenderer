// NativeXRenderer - forwarding the SDK graphics cvars to the GPU plugin's registration (internal).
// See sdk_cvars.cpp.

#pragma once

#include <rex/cvar.h>

#include <string>
#include <type_traits>

namespace native_x_renderer::sdk {

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

// Takes the registration options the SDK chains after a definition (`.range(min, max)`, `.lifecycle(...)`),
// and ignores them: the plugin's registration has them.
struct IgnoredRegistration {
  template <typename... Args>
  constexpr IgnoredRegistration range(Args&&...) const {
    return *this;
  }
  template <typename... Args>
  constexpr IgnoredRegistration lifecycle(Args&&...) const {
    return *this;
  }
};

}  // namespace native_x_renderer::sdk

// An SDK source compiled into NativeXRenderer defines its cvars through these: accessors that forward to the
// plugin's registered value, and no registration.
#undef REXCVAR_DEFINE_BOOL
#define REXCVAR_DEFINE_BOOL(name, default_val, category, desc)                                   \
  bool& FLAGS_##name##_storage_() { return ::native_x_renderer::sdk::Forward<bool>(#name, default_val); } \
  [[maybe_unused]] static constexpr auto native_x_renderer_cvar_##name = ::native_x_renderer::sdk::IgnoredRegistration {}
#undef REXCVAR_DEFINE_INT32
#define REXCVAR_DEFINE_INT32(name, default_val, category, desc)                                      \
  int32_t& FLAGS_##name##_storage_() { return ::native_x_renderer::sdk::Forward<int32_t>(#name, default_val); } \
  [[maybe_unused]] static constexpr auto native_x_renderer_cvar_##name = ::native_x_renderer::sdk::IgnoredRegistration {}
