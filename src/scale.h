// XenosNative - the render scale: host pixels per guest pixel (internal, D37).
//
// Everything the game renders is drawn at this scale: render targets, viewports, scissors, resolves and the
// textures they produce. It follows the screen (the largest at which the game's image fits the output), so
// it need not be a whole number. It is chosen once, at startup, per axis; both axes are equal for now (a
// different aspect ratio would make them differ).
//
// A guest pixel coordinate becomes a host one by HostX() and HostY(), with the same rounding for both edges
// of a rectangle: rectangles that touch in guest pixels touch on the host too, with no gap and no overlap.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xenos_native {

struct RenderScale {
  float x = 1.0f, y = 1.0f;

  bool scaled() const { return x != 1.0f || y != 1.0f; }
  // A guest pixel coordinate (an edge, or a size from 0) on the host.
  int32_t HostX(int64_t guest) const { return int32_t(std::floor(double(guest) * x + 0.5)); }
  int32_t HostY(int64_t guest) const { return int32_t(std::floor(double(guest) * y + 0.5)); }
  uint32_t HostWidth(uint32_t guest) const { return uint32_t(std::max(HostX(guest), int32_t(1))); }
  uint32_t HostHeight(uint32_t guest) const { return uint32_t(std::max(HostY(guest), int32_t(1))); }
};

}  // namespace xenos_native
