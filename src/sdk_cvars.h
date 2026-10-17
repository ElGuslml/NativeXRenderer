// NativeXRenderer - forwarding the SDK graphics cvars to the GPU plugin's registration (internal).
// See sdk_cvars.cpp.

#pragma once

#include <rex/cvar.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <type_traits>

namespace native_x_renderer::sdk {

// Bumped once per frame (NextFrame, declared in internal.h): forwarded values are looked up again after it.
extern std::atomic<uint32_t> g_frame_generation;

// The value the GPU plugin registered under one name (or `fallback` before it is registered), per thread.
// Looking it up by name takes the registry's lock and converts it from text, which the draw path did
// several times per draw (D68): it is looked up once per frame instead. Returned by reference, as the
// SDK's cvar accessors are; each accessor has its own.
template <typename T>
class Forwarded {
 public:
  T& Get(const char* name, const T& fallback) {
    uint32_t generation = g_frame_generation.load(std::memory_order_relaxed);
    if (generation != generation_) {
      generation_ = generation;
      value_ = rex::cvar::Query<T>(name);
      if constexpr (std::is_same_v<T, std::string>) {
        if (value_.empty()) value_ = fallback;
      }
    }
    return value_;
  }

 private:
  T value_{};
  uint32_t generation_ = 0;  // g_frame_generation starts at 1: the first Get looks up
};

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
  bool& FLAGS_##name##_storage_() {                                                              \
    thread_local ::native_x_renderer::sdk::Forwarded<bool> forwarded;                            \
    return forwarded.Get(#name, default_val);                                                    \
  }                                                                                              \
  [[maybe_unused]] static constexpr auto native_x_renderer_cvar_##name = ::native_x_renderer::sdk::IgnoredRegistration {}
#undef REXCVAR_DEFINE_INT32
#define REXCVAR_DEFINE_INT32(name, default_val, category, desc)                                  \
  int32_t& FLAGS_##name##_storage_() {                                                           \
    thread_local ::native_x_renderer::sdk::Forwarded<int32_t> forwarded;                         \
    return forwarded.Get(#name, default_val);                                                    \
  }                                                                                              \
  [[maybe_unused]] static constexpr auto native_x_renderer_cvar_##name = ::native_x_renderer::sdk::IgnoredRegistration {}
