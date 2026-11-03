// NativeXRenderer - the motion of objects that move on their own, for DLAA (internal, D48).
//
// DLAA's motion vectors come from the depth and the camera (upscaler.cpp), which is right for everything
// that stands still. Characters (their bones are vertex shader constants) and the props a game animates
// move on their own; for those, the main 3D view's draws that write depth are matched with the last
// frame's:
//   - a draw is known by a key: its vertex shader, its vertex and index data, its vertex count. Several
//     draws with one key (one mesh drawn in several places) are told apart by their constants: the
//     closest of the last frame's is the same object;
//   - it moved if its vertex shader's float constants changed, apart from the registers the whole view
//     shares: the camera's, which every draw that uses them sees with one value, and which half of the
//     view's draws or more use. They are found in each frame, for the next;
//   - a draw that moved is drawn again before DLSS with its motion vertex shader (spirv_patch.h), its
//     constants of this frame and of the last, and the same viewport, depth test equal: it writes its
//     motion where it is what the scene shows (draw.cpp, DrawObjectMotion).

#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <utility>
#include <vector>

#include "GraphicsTypes.h"

namespace native_x_renderer {

// The motion vectors DLSS takes: from this frame's position to the last frame's, in host pixels.
constexpr Diligent::TEXTURE_FORMAT kMotionVectorFormat = Diligent::TEX_FORMAT_RG16_FLOAT;

class ObjectMotion {
 public:
  // A draw of the main 3D view that writes depth. `constants`: its vertex shader's float constants, 16 bytes
  // each, from the registers listed in `registers` (`count` of them). Returns the last frame's constants of
  // the same object if it moved since then; nullptr if it did not, or if it is new. The pointer is valid
  // until EndFrame. `matched`, when given: the last frame's constants of the same object whether it moved or not
  // (the camera vote's, D118), or nullptr if it is new.
  const uint8_t* Match(uint64_t key, const uint8_t* constants, const uint8_t* registers, uint32_t count,
                       const uint8_t** matched = nullptr);
  // Right after Match returned `previous` for a draw: whether its `constants` differ from those only at the positions
  // [from, to) of its register list and in the registers the view shares. Its own animation (bones) is elsewhere, so
  // then it moved only as its matrix says (D120).
  bool ChangedOnlyIn(const uint8_t* constants, const uint8_t* previous, uint32_t from, uint32_t to) const;
  // After the frame: its draws become the last frame's.
  void EndFrame();

  // Draws matched and draws that moved, since the last call.
  struct Stats {
    uint32_t matched = 0, moved = 0;
  };
  Stats TakeStats() {
    Stats stats = stats_;
    stats_ = {};
    return stats;
  }

 private:
  struct Entry {
    uint64_t key;
    uint32_t offset;  // in data
    uint32_t count;
  };
  struct Frame {
    std::vector<Entry> entries;
    std::vector<uint8_t> data;
    // (key, entry) sorted, made at the frame's end: the entries of a key are one run, in the order drawn (D122: a hash
    // map allocated a node for every draw).
    std::vector<std::pair<uint64_t, uint32_t>> index;
    void Clear() {
      entries.clear();
      data.clear();
      index.clear();
    }
  };
  Frame frames_[2];
  uint32_t current_ = 0;
  // A draw's runs of constants the view does not share, as positions [first, second) in its list (D122).
  std::vector<std::pair<uint32_t, uint32_t>> runs_;

  // The registers the view shares (the last frame's), and this frame's count of them.
  std::bitset<256> shared_;
  std::array<std::array<uint32_t, 4>, 256> view_values_{};
  std::array<uint32_t, 256> view_users_{};
  std::bitset<256> view_mixed_;
  uint32_t view_draws_ = 0;
  Stats stats_;
};

}  // namespace native_x_renderer
