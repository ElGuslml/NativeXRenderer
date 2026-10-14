// XenosNative - helpers shared by the library's features (internal).

#pragma once

#include <xenos_native/xenos_native.h>

#include <rex/ppc/context.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace xenos_native::internal {

enum CallFlags : uint32_t { kDraw = 1u << 0, kFrameEnd = 1u << 1 };

struct CallInfo {
  const char* name;
  const char* arguments;  // see calls.inc
  uint32_t flags;
  bool device;  // r3 is the D3D device
};

const CallInfo& Info(Call call);

uint32_t CurrentThreadId();

// Time spent creating host objects (shader analyses and translations, pipelines), for the renderer's log:
// creations on the render thread stall the frame.
struct CreationTime {
  uint32_t count = 0;
  double ms = 0, max_ms = 0;

  void Add(double t) {
    ++count;
    ms += t;
    max_ms = std::max(max_ms, t);
  }
  void Merge(const CreationTime& other) {
    count += other.count;
    ms += other.ms;
    max_ms = std::max(max_ms, other.max_ms);
  }
};

// Adds the time from its construction to its destruction to a CreationTime.
class ScopedCreationTime {
 public:
  explicit ScopedCreationTime(CreationTime& time) : time_(time), start_(std::chrono::steady_clock::now()) {}
  ~ScopedCreationTime() {
    time_.Add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count());
  }

 private:
  CreationTime& time_;
  std::chrono::steady_clock::time_point start_;
};

// Guest memory is big-endian.
inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return std::byteswap(value);
}

inline uint64_t Load64(const uint8_t* base, uint32_t address) {
  uint64_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return std::byteswap(value);
}

inline float LoadFloat(const uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(Load32(base, address));
}

// The physical address the GPU uses for a CPU virtual address, as the XDK computes it (the
// 0xE0000000 view is offset by 4 KB).
inline uint32_t GpuAddress(uint32_t address) {
  return (address & 0x1FFFFFFF) + (((address >> 20) + 0x200) & 0x1000);
}

}  // namespace xenos_native::internal
