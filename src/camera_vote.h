// NativeXRenderer - the camera's reprojection found by a vote of the main view's draws (internal, D118).
//
// The temporal upscalers' camera motion (upscaler.cpp, D47, D117) needs the matrix that takes this frame's clip
// position to the last frame's. A game's view hook can give the view-projection matrix (BeginView); without one, the
// draws tell it, as King Kong Recompiled's renderer does (its CameraReprojectionVote and LooksLikePerspectiveRows,
// adapted; Copyright (c) 2026 TekRantGaming and contributors, MIT: thirdparty/king-kong-recompiled/LICENSE):
//   - each draw of the main 3D view that writes depth, matched with its last-frame self (D48's key and closest
//     constants), has a world-view-projection in four consecutive vertex shader registers: the first four that look
//     like a perspective (two rows almost parallel: the clip z and w);
//   - last frame's matrix times the inverse of this frame's takes this frame's clip space to the last frame's, the
//     world cancelling out for whatever did not move on its own;
//   - the draws offer that matrix, those agreeing within 2e-3 of their scale are counted together, and the one most
//     agree on, their mean, is the camera's.
// The registers hold the matrix either as the rows of clip = M x position (each register a dp4 with the position:
// King Kong's games) or as the rows of position x M for a row vector (each register an input axis: POTCAWE's c0-c3).
// Both are tried.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace native_x_renderer {

// 4x4, rows: m[r * 4 + c]. A clip position (column) = m x a position (column).
using Mat4d = std::array<double, 16>;

class CameraVote {
 public:
  // The first four consecutive registers among `registers` (`count` of them, ascending, each 16 bytes of `constants`)
  // that hold a perspective matrix: `clip_from_position` in column form, `layout_transposed` when the registers are
  // its columns (position x M rows). False if none.
  static bool FindMatrix(const uint8_t* constants, const uint8_t* registers, uint32_t count, uint32_t& index,
                         bool& layout_transposed);
  // The matrix the four registers from `index` hold, in column form.
  static Mat4d MatrixAt(const uint8_t* constants, uint32_t index, bool layout_transposed);

  // A new vote (the main view's draws of one frame).
  void Begin();
  // A draw's matrices this frame and the last (the same registers of the same draw, column form). Returns the draw's
  // reprojection (valid until the next call), or nullptr when this frame's matrix is not invertible.
  const Mat4d* Offer(const Mat4d& current, const Mat4d& previous);
  // How far apart two reprojections put points across the screen (x, y at -0.75, 0, 0.75) and the depth range (z 0.5
  // to 0.9999: a perspective's depth crowds near the far plane): the largest distance, in pixels of an image of
  // `width` x `height`. Points either puts behind the camera are left out.
  static double DistancePx(const Mat4d& a, const Mat4d& b, double width, double height);
  // Whether that distance is within `limit` pixels, cheaper for a draw (D122): 15 points (the corners at +-0.75 and
  // the centre, at z 0.5, 0.99 and 0.9999), the first beyond the limit ending it.
  static bool WithinPx(const Mat4d& a, const Mat4d& b, double width, double height, double limit);
  // The reprojection most offers agreed on (column form: last clip = r x this clip), with how many agreed and how many
  // offered; during the frame, the one leading so far. False when nothing was offered.
  bool Result(Mat4d& reprojection, uint32_t& votes, uint32_t& offers) const;

  static constexpr double kTolerance = 2e-3;

 private:
  struct Candidate {
    Mat4d first;  // what the next offers are compared with
    Mat4d sum;
    uint32_t votes = 0;
  };
  std::vector<Candidate> candidates_;
  uint32_t offers_ = 0;
  // The last offer: consecutive draws with the same matrices (a field of grass under one camera) join the same
  // candidate without another inversion.
  bool last_valid_ = false;
  Mat4d last_current_{}, last_previous_{};
  int last_candidate_ = -1;
  bool last_counted_ = false;
  Mat4d last_r_{};
};

// Matrix helpers (column form, doubles).
Mat4d Mat4dMultiply(const Mat4d& a, const Mat4d& b);
bool Mat4dInvert(const Mat4d& m, Mat4d& out);

}  // namespace native_x_renderer
