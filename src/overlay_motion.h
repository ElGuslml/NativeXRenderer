// NativeXRenderer - the motion of the 2D layer drawn after the main 3D view, for frame generation (internal, D124).
//
// Frame generation (D123) takes the main 3D view's motion vectors; what a game draws after the view (its HUD, GH
// Metallica's note highway and notes) has none of its own there, and was moved as the scene behind it. In a game whose
// 2D layer is drawn in pixels of its output canvas, its vertices' first two floats the position (the profile's
// overlay_motion), each such draw after the view is matched with the last frame's draw of the same kind (its vertex and
// pixel shaders, vertex count, stride and primitive type) closest to it, and the movement of its vertices since then is
// drawn into the motion vectors over the scene's, with the near plane's depth: a note sprite moves as the game moved
// it, the HUD stays. A draw with no match (new this frame) stands still. The game may set the last positions itself
// where its vertices do not show the movement (Profile::overlay_motion_adjust: GH Metallica's board, its texture
// scrolling).

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"
#include "native_x_renderer/native_x_renderer.h"

namespace native_x_renderer {

class OverlayMotion {
 public:
  // A draw after the view: its kind (`key`), its vertex shader's hash, the guest's primitive type, and its vertices'
  // positions, x then y, in pixels of the canvas. Only quad and triangle lists are kept (the scene's own full-screen
  // passes are rectangles).
  void Add(uint64_t key, uint64_t vertex_shader, uint32_t primitive, const float* positions, uint32_t vertex_count);
  // Draws the frame's motion into `motion` (RG16F, its pixels, from this frame's position to the last's) and `depth`
  // (R32_FLOAT, 0 at the near plane), both covering the canvas (`canvas_width` x `canvas_height`), after `adjust` (the
  // profile's, when given) saw the frame's draws; then the frame's draws become the last frame's. Nothing to draw:
  // false.
  bool Draw(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITexture* motion,
            Diligent::ITexture* depth, uint32_t canvas_width, uint32_t canvas_height,
            void (*adjust)(OverlayDraw* draws, size_t count));
  // A frame without a view: the last frame's draws forgotten.
  void Clear();

  // Draws kept and matched with the last frame's, since the last call (for the log).
  struct Stats {
    uint32_t draws = 0, matched = 0;
  };
  Stats TakeStats() {
    Stats stats = stats_;
    stats_ = {};
    return stats;
  }

 private:
  struct Entry {
    uint64_t key, vertex_shader;
    uint32_t primitive, offset, count;  // positions[offset..] and previous[offset..] (x, y pairs)
    float center[2];
    bool matched;
  };
  struct Frame {
    std::vector<Entry> entries;
    std::vector<float> positions, previous;
    std::unordered_map<uint64_t, std::vector<uint32_t>> by_key;
    void Clear() {
      entries.clear();
      positions.clear();
      previous.clear();
      by_key.clear();
    }
  };
  bool CreatePipeline(Diligent::IRenderDevice* device);

  Frame frames_[2];
  uint32_t current_ = 0;
  std::vector<OverlayDraw> draws_;
  std::vector<float> vertices_;  // this frame's triangles: canvas x, y and the motion, per vertex
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer_;
  uint64_t buffer_size_ = 0;
  bool failed_ = false;
  Stats stats_;
};

}  // namespace native_x_renderer
