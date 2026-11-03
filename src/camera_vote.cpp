// NativeXRenderer - see camera_vote.h.

#include "camera_vote.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace native_x_renderer {

namespace {

// Clip z and w rows almost parallel (King Kong Recompiled's LooksLikePerspectiveRows): the x, y and w rows not zero,
// z = k w with 0.8 < k < 1.25 over the position's x, y and z, within 5 % of w's length.
bool LooksLikePerspective(const Mat4d& m) {
  auto dot3 = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  const double* x = m.data();
  const double* y = m.data() + 4;
  const double* z = m.data() + 8;
  const double* w = m.data() + 12;
  const double ww = dot3(w, w);
  if (!(ww > 1e-8) || !(dot3(x, x) > 1e-8) || !(dot3(y, y) > 1e-8)) return false;
  const double k = dot3(z, w) / ww;
  if (!(k > 0.8 && k < 1.25)) return false;
  double residual = 0.0;
  for (int i = 0; i < 3; ++i) residual += (z[i] - k * w[i]) * (z[i] - k * w[i]);
  return residual <= 0.0025 * ww;
}

bool Close(const Mat4d& a, const Mat4d& b) {
  double scale = 0.0;
  for (int i = 0; i < 16; ++i) scale = std::max({scale, std::abs(a[i]), std::abs(b[i])});
  if (scale == 0.0) return true;
  for (int i = 0; i < 16; ++i) {
    if (std::abs(a[i] - b[i]) > CameraVote::kTolerance * scale) return false;
  }
  return true;
}

}  // namespace

Mat4d Mat4dMultiply(const Mat4d& a, const Mat4d& b) {
  Mat4d out{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = sum;
    }
  }
  return out;
}

bool Mat4dInvert(const Mat4d& m, Mat4d& out) {
  double a[4][8];
  double scale = 0.0;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[r * 4 + c];
      a[r][c + 4] = r == c ? 1.0 : 0.0;
      scale = std::max(scale, std::abs(m[r * 4 + c]));
    }
  }
  if (!(scale > 0.0)) return false;
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r) {
      if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
    }
    if (!(std::abs(a[pivot][c]) > 1e-12 * scale)) return false;
    if (pivot != c) std::swap(a[pivot], a[c]);
    double inverse = 1.0 / a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] *= inverse;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      double factor = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= factor * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
  }
  return true;
}

Mat4d CameraVote::MatrixAt(const uint8_t* constants, uint32_t index, bool layout_transposed) {
  float values[16];
  std::memcpy(values, constants + size_t(index) * 16, sizeof(values));
  Mat4d m;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) m[r * 4 + c] = layout_transposed ? values[c * 4 + r] : values[r * 4 + c];
  }
  return m;
}

bool CameraVote::FindMatrix(const uint8_t* constants, const uint8_t* registers, uint32_t count, uint32_t& index,
                            bool& layout_transposed) {
  for (uint32_t i = 0; i + 4 <= count; ++i) {
    if (registers[i + 3] != registers[i] + 3) continue;  // four consecutive registers
    for (bool transposed : {false, true}) {
      if (LooksLikePerspective(MatrixAt(constants, i, transposed))) {
        index = i;
        layout_transposed = transposed;
        return true;
      }
    }
  }
  return false;
}

void CameraVote::Begin() {
  candidates_.clear();
  offers_ = 0;
  last_valid_ = false;
}

void CameraVote::Offer(const Mat4d& current, const Mat4d& previous) {
  if (last_valid_ && current == last_current_ && previous == last_previous_) {
    if (!last_counted_) return;
    ++offers_;
    if (last_candidate_ >= 0) {
      Candidate& candidate = candidates_[size_t(last_candidate_)];
      ++candidate.votes;
      for (int i = 0; i < 16; ++i) candidate.sum[i] += last_r_[i];
    }
    return;
  }
  last_valid_ = true;
  last_current_ = current;
  last_previous_ = previous;
  last_counted_ = false;
  last_candidate_ = -1;
  Mat4d inverse;
  if (!Mat4dInvert(current, inverse)) return;
  Mat4d r = Mat4dMultiply(previous, inverse);
  // The homogeneous scale kept at r[15] = 1 where it can be, so that equal reprojections compare equal.
  if (std::abs(r[15]) > 1e-9) {
    double scale = 1.0 / r[15];
    for (double& v : r) v *= scale;
  }
  ++offers_;
  last_counted_ = true;
  last_r_ = r;
  for (size_t k = 0; k < candidates_.size(); ++k) {
    Candidate& candidate = candidates_[k];
    if (!Close(candidate.first, r)) continue;
    // The members' mean: each draw's matrix carries the rounding of its float constants (worst far from the world's
    // origin), which averages out.
    ++candidate.votes;
    for (int i = 0; i < 16; ++i) candidate.sum[i] += r[i];
    last_candidate_ = int(k);
    return;
  }
  if (candidates_.size() < 16) {
    candidates_.push_back({r, r, 1});
    last_candidate_ = int(candidates_.size() - 1);
  }
}

bool CameraVote::Result(Mat4d& reprojection, uint32_t& votes, uint32_t& offers) const {
  offers = offers_;
  votes = 0;
  if (candidates_.empty()) return false;
  const Candidate* best = &candidates_[0];
  for (const Candidate& candidate : candidates_) {
    if (candidate.votes > best->votes) best = &candidate;
  }
  for (int i = 0; i < 16; ++i) reprojection[i] = best->sum[i] / double(best->votes);
  votes = best->votes;
  return true;
}

}  // namespace native_x_renderer
