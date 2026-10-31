// NativeXRenderer - macOS: the game's window as DiligentCore takes it (src/window_mac.mm; internal).

#pragma once

#include <cstdint>

namespace rex::ui {
class Window;
}

namespace native_x_renderer::mac {

// A Metal view on the game's window (the SDK's CAMetalLayer surface, made once its presenter has let the window go),
// as the NSView DiligentCore's MacOSNativeWindow takes; null if it cannot be made. Kept until ReleaseMetalView.
void* AttachMetalView(rex::ui::Window* window);
void ReleaseMetalView();

// The view's size in pixels (its points times the screen's scale), as the swap chain should be; false while it has
// no area.
bool DrawableSize(void* ns_view, uint32_t& width, uint32_t& height);

}  // namespace native_x_renderer::mac
