// XenosNative - see motion.h.

#include "motion.h"

#include <rex/logging.h>

#include <cmath>
#include <cstring>
#include <string>

namespace xenos_native {

const uint8_t* ObjectMotion::Match(uint64_t key, const uint8_t* constants, const uint8_t* registers, uint32_t count) {
  // The registers the view shares, for the next frame.
  ++view_draws_;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t r = registers[i];
    if (!view_users_[r]++) {
      std::memcpy(view_values_[r].data(), constants + 16 * i, 16);
    } else if (std::memcmp(view_values_[r].data(), constants + 16 * i, 16)) {
      view_mixed_.set(r);
    }
  }

  // This frame's, for the next.
  Frame& current = frames_[current_];
  uint32_t offset = uint32_t(current.data.size());
  current.data.insert(current.data.end(), constants, constants + 16 * size_t(count));
  auto [first, inserted] = current.first.try_emplace(key, uint32_t(current.entries.size()));
  current.entries.push_back({key, offset, count, inserted ? kNone : first->second});
  if (!inserted) first->second = uint32_t(current.entries.size() - 1);

  // The last frame's.
  const Frame& previous = frames_[current_ ^ 1];
  auto it = previous.first.find(key);
  if (it == previous.first.end()) return nullptr;
  ++stats_.matched;
  const uint8_t* closest = nullptr;
  float closest_distance = INFINITY;
  for (uint32_t e = it->second; e != kNone; e = previous.entries[e].next) {
    const Entry& entry = previous.entries[e];
    if (entry.count != count) continue;
    const uint8_t* old = previous.data.data() + entry.offset;
    bool same = true;
    float distance = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
      if (shared_.test(registers[i]) || !std::memcmp(old + 16 * i, constants + 16 * i, 16)) continue;
      same = false;
      float a[4], b[4];
      std::memcpy(a, old + 16 * i, 16);
      std::memcpy(b, constants + 16 * i, 16);
      for (uint32_t k = 0; k < 4; ++k) {
        float d = std::abs(a[k] - b[k]);
        distance += d < 1e30f ? d : 1e30f;  // NaN and infinities count as far
      }
    }
    // One of them did not move: it is this one.
    if (same) return nullptr;
    if (distance < closest_distance) {
      closest_distance = distance;
      closest = old;
    }
  }
  if (closest) ++stats_.moved;
  return closest;
}

void ObjectMotion::EndFrame() {
  if (view_draws_) {
    std::bitset<256> shared;
    for (uint32_t r = 0; r < 256; ++r) {
      shared[r] = !view_mixed_[r] && view_users_[r] >= 2 && view_users_[r] * 2 >= view_draws_;
    }
    if (shared != shared_) {
      // Rarely: the camera's registers are the same from view to view.
      static uint32_t logged = 0;
      if (logged < 8) {
        ++logged;
        std::string list;
        for (uint32_t r = 0; r < 256; ++r) {
          if (shared[r]) list += " c" + std::to_string(r);
        }
        REXLOG_INFO("[xenos_native] object motion: the registers the main view shares:{}", list);
      }
      shared_ = shared;
    }
  }
  view_users_.fill(0);
  view_mixed_.reset();
  view_draws_ = 0;
  current_ ^= 1;
  frames_[current_].Clear();
}

}  // namespace xenos_native
