// NativeXRenderer - macOS: the game's window as DiligentCore takes it (src/window_mac.h).
//
// On Windows the renderer draws on the window's HWND. Here it adds its own Metal view to the game's window: an
// NSView hosting a CAMetalLayer that fills the window's content view (as SDL's Metal view does for the SDK's
// presenter, which has let the window go by then). DiligentCore's MacOSNativeWindow takes the NSView and draws on its
// layer (VK_EXT_metal_surface, through KosmicKrisp or MoltenVK).

#include "window_mac.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <dispatch/dispatch.h>

@interface NXRMetalView : NSView
@end

@implementation NXRMetalView
- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.wantsLayer = YES;
    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  }
  return self;
}
- (CALayer*)makeBackingLayer {
  return [CAMetalLayer layer];
}
- (BOOL)wantsUpdateLayer {
  return YES;
}
// Clicks and keys go to the window's other views (SDL's), as they did before this one was added.
- (NSView*)hitTest:(NSPoint)point {
  (void)point;
  return nil;
}
// The layer's pixels follow the screen's scale (a Retina display, another monitor).
- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  if (self.window) self.layer.contentsScale = self.window.backingScaleFactor;
}
@end

namespace native_x_renderer::mac {
namespace {

NXRMetalView* g_view = nil;

// The game's window: the app's main or key window, else its first visible one (the game has one).
NSWindow* GameWindow() {
  NSWindow* window = NSApp.mainWindow ?: NSApp.keyWindow;
  if (window) return window;
  for (NSWindow* w in NSApp.windows) {
    if (w.visible && w.contentView) return w;
  }
  return nil;
}

template <typename F>
void OnMainThread(F&& f) {
  if ([NSThread isMainThread]) {
    f();
  } else {
    F* function = &f;
    dispatch_sync(dispatch_get_main_queue(), ^{
      (*function)();
    });
  }
}

}  // namespace

void* AttachMetalView(rex::ui::Window* window) {
  (void)window;
  void* result = nullptr;
  OnMainThread([&] {
    NSWindow* ns_window = GameWindow();
    NSView* content = ns_window.contentView;
    if (!content) return;
    if (g_view) [g_view removeFromSuperview];
    g_view = [[NXRMetalView alloc] initWithFrame:content.bounds];
    g_view.layer.contentsScale = ns_window.backingScaleFactor;
    [content addSubview:g_view positioned:NSWindowAbove relativeTo:nil];
    result = (__bridge void*)g_view;
  });
  return result;
}

void ReleaseMetalView() {
  OnMainThread([] {
    [g_view removeFromSuperview];
    g_view = nil;
  });
}

bool DrawableSize(void* ns_view, uint32_t& width, uint32_t& height) {
  if (!ns_view) return false;
  NSSize size = NSZeroSize;
  // Read on any thread: bounds and scale are plain values; the renderer thread asks each frame.
  NSView* view = (__bridge NSView*)ns_view;
  size = [view convertSizeToBacking:view.bounds.size];
  width = uint32_t(size.width + 0.5);
  height = uint32_t(size.height + 0.5);
  return width && height;
}

}  // namespace native_x_renderer::mac
