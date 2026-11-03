// NativeXRenderer - see motion.h.

#include "motion.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace native_x_renderer {

const uint8_t* ObjectMotion::Match(uint64_t key, const uint8_t* constants, const uint8_t* registers, uint32_t count,
                                   const uint8_t** matched) {
  if (matched) *matched = nullptr;
  // The registers the view shares, for the next frame (a register two draws already disagreed on is not compared
  // again).
  ++view_draws_;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t r = registers[i];
    if (!view_users_[r]++) {
      std::memcpy(view_values_[r].data(), constants + 16 * i, 16);
    } else if (!view_mixed_.test(r) && std::memcmp(view_values_[r].data(), constants + 16 * i, 16)) {
      view_mixed_.set(r);
    }
  }

  // This frame's, for the next.
  Frame& current = frames_[current_];
  uint32_t offset = uint32_t(current.data.size());
  current.data.insert(current.data.end(), constants, constants + 16 * size_t(count));
  current.entries.push_back({key, offset, count});

  // The last frame's.
  const Frame& previous = frames_[current_ ^ 1];
  auto it = std::lower_bound(previous.index.begin(), previous.index.end(), std::pair<uint64_t, uint32_t>{key, 0});
  if (it == previous.index.end() || it->first != key) return nullptr;
  auto end = it;
  while (end != previous.index.end() && end->first == key) ++end;
  ++stats_.matched;
  // The constants the view does not share, in runs compared whole (D122: one comparison per register was most of
  // this function's time with GH Metallica's 256 registers a draw).
  runs_.clear();
  for (uint32_t i = 0; i < count; ++i) {
    if (shared_.test(registers[i])) continue;
    if (!runs_.empty() && runs_.back().second == i) {
      ++runs_.back().second;
    } else {
      runs_.push_back({i, i + 1});
    }
  }
  const bool only = end - it == 1;
  const uint8_t* closest = nullptr;
  float closest_distance = INFINITY;
  for (; it != end; ++it) {
    const Entry& entry = previous.entries[it->second];
    if (entry.count != count) continue;
    const uint8_t* old = previous.data.data() + entry.offset;
    bool same = true;
    for (const auto& [from, to] : runs_) {
      if (std::memcmp(old + 16 * size_t(from), constants + 16 * size_t(from), 16 * size_t(to - from))) {
        same = false;
        break;
      }
    }
    // One of them did not move: it is this one.
    if (same) {
      if (matched) *matched = old;
      return nullptr;
    }
    // The only one of its key: the same object, however far (D122).
    if (only) {
      closest = old;
      break;
    }
    float distance = 0.0f;
    for (const auto& [from, to] : runs_) {
      for (uint32_t i = from; i < to; ++i) {
        float a[4], b[4];
        std::memcpy(a, old + 16 * i, 16);
        std::memcpy(b, constants + 16 * i, 16);
        for (uint32_t k = 0; k < 4; ++k) {
          float d = std::abs(a[k] - b[k]);
          distance += d < 1e30f ? d : 1e30f;  // NaN and infinities count as far
        }
      }
    }
    if (distance < closest_distance) {
      closest_distance = distance;
      closest = old;
    }
  }
  if (closest) ++stats_.moved;
  if (matched) *matched = closest;
  return closest;
}

bool ObjectMotion::ChangedOnlyIn(const uint8_t* constants, const uint8_t* previous, uint32_t from, uint32_t to) const {
  // The runs Match made for this draw, without [from, to).
  auto differ = [&](uint32_t a, uint32_t b) {
    return a < b && std::memcmp(constants + 16 * size_t(a), previous + 16 * size_t(a), 16 * size_t(b - a)) != 0;
  };
  for (const auto& [a, b] : runs_) {
    if (differ(a, std::min(b, from)) || differ(std::max(a, to), b)) return false;
  }
  return true;
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
        REXLOG_INFO("[native_x_renderer] object motion: the registers the main view shares:{}", list);
      }
      shared_ = shared;
    }
  }
  view_users_.fill(0);
  view_mixed_.reset();
  view_draws_ = 0;
  Frame& done = frames_[current_];
  done.index.resize(done.entries.size());
  for (uint32_t e = 0; e < uint32_t(done.entries.size()); ++e) done.index[e] = {done.entries[e].key, e};
  std::sort(done.index.begin(), done.index.end());
  current_ ^= 1;
  frames_[current_].Clear();
}

}  // namespace native_x_renderer
