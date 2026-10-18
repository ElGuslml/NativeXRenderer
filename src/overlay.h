// NativeXRenderer - the SDK's ImGui overlays on NativeXRenderer's swap chain (internal).
//
// The SDK draws its dialogs (XamShowKeyboardUI's keyboard, message boxes, its debug overlay) with Dear ImGui
// through its ImGuiDrawer, which normally paints from the SDK's presenter. NativeXRenderer detaches that
// presenter from the window (it presents through its own swap chain), so the ImGuiDrawer gets this drawer instead
// (D75). Dear ImGui and the window's input stay on the UI thread, as in the SDK's own mode: there, ImGuiDrawer::Draw
// runs once per presented frame and this drawer records what it draws (vertices, indices, the draws and their
// textures). The renderer's thread draws the latest record over the frame before presenting it.
#pragma once

#include <rex/ui/immediate_drawer.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace native_x_renderer {

class Overlay final : public rex::ui::ImmediateDrawer {
 public:
  // The renderer's thread, at startup: the device and the swap chain's format.
  bool Initialize(Diligent::IRenderDevice* device, Diligent::TEXTURE_FORMAT target_format);
  // The renderer's thread, once per presented frame, before Present: asks the UI thread to draw the SDK's dialogs
  // (the first call also hands this drawer to the runtime's ImGuiDrawer), and draws the latest record, if a dialog
  // is open, over `target` (the swap chain's back buffer, `width` x `height`).
  void Present(Diligent::IDeviceContext* context, Diligent::ITextureView* target, uint32_t width, uint32_t height,
               rex::ui::Window* window);

  // rex::ui::ImmediateDrawer, on the UI thread (from ImGuiDrawer::Draw).
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                           rex::ui::ImmediateTextureFilter filter, bool is_repeated,
                                                           const uint8_t* data) override;
  void Begin(rex::ui::UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override;
  void Draw(const rex::ui::ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  // A texture's data, created on the UI thread; its host texture is made on the renderer's thread when first
  // drawn. Shared by the ImmediateTexture the SDK owns and by the records that draw with it.
  struct TextureEntry {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
  };
  struct Texture final : rex::ui::ImmediateTexture {
    Texture(uint32_t width, uint32_t height) : rex::ui::ImmediateTexture(width, height) {}
    std::shared_ptr<TextureEntry> data;
  };
  struct DrawCommand {
    uint32_t first_index = 0, index_count = 0;
    int32_t base_vertex = 0;
    std::shared_ptr<TextureEntry> texture;  // nullptr: color only
    bool scissor = false;
    float left = 0, top = 0, right = 0, bottom = 0;  // in the coordinate space
  };
  struct Frame {
    float width = 0, height = 0;  // the coordinate space
    std::vector<rex::ui::ImmediateVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<DrawCommand> draws;
  };

  // The renderer's thread.
  void Attach(rex::ui::Window* window);
  bool Prepare(TextureEntry& texture);
  void Render(Diligent::IDeviceContext* context, Diligent::ITextureView* target, uint32_t width, uint32_t height,
              const Frame& frame);

  Diligent::IRenderDevice* device_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::IPipelineState> pipeline_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> vertices_, indices_;
  uint32_t vertex_capacity_ = 0, index_capacity_ = 0;
  std::shared_ptr<TextureEntry> white_;  // for the draws without a texture
  rex::ui::ImGuiDrawer* imgui_ = nullptr;
  Frame drawing_;  // the renderer's copy of the latest record

  // The UI thread.
  Frame recording_;
  uint32_t batch_vertex_base_ = 0, batch_index_base_ = 0;

  // Between the two.
  std::mutex mutex_;
  Frame published_;            // guarded by mutex_
  bool has_published_ = false;  // guarded by mutex_
  std::atomic<bool> draw_requested_{false};
  std::atomic<bool> dialogs_open_{false};
};

}  // namespace native_x_renderer
