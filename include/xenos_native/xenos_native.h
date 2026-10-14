// XenosNative - native renderer for Xbox 360 games recompiled with the ReXGlue SDK.
//
// Replaces the drawing done through the game's XDK Direct3D library with host rendering on
// DiligentCore (Vulkan), keeping the SDK's Xenos emulation as the fallback. See xenos_native/README.md
// and xenos_native/docs/.
//
// Phase 1: the trace and the observer. The game reports its D3D entry points through hooks.h,
// describes its D3D device layout in a Profile, and calls Initialize() once before the guest starts.
// Nothing is drawn yet: the Xenos path draws.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system
namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace xenos_native {

// Bumped when the interface the game side uses changes.
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 4;

// One line for the game's startup log: version and what the library does at this stage.
const char* Describe();

// The D3D entry points a game can report (calls.inc).
enum class Call : uint8_t {
#define XENOS_NATIVE_CALL(id, arguments, flags) k##id,
#include <xenos_native/calls.inc>
#undef XENOS_NATIVE_CALL
  kCount
};
inline constexpr size_t kCallCount = size_t(Call::kCount);

const char* CallName(Call call);

// Where the D3D device structure keeps what XenosNative reads, as byte offsets from the device
// pointer (r3 of every device call). The values depend on the XDK version the game was built with:
// see docs/xdk-d3d-reference.md. kUnknown means "not known for this game".
inline constexpr uint32_t kUnknown = 0xFFFFFFFFu;

struct DeviceLayout {
  // Six 64-bit dirty masks: VS constants, PS constants, register groups, fetch constants, misc,
  // tiling-dependent state.
  uint32_t dirty_masks = kUnknown;
  uint32_t command_buffer_write = kUnknown;  // u32: address of the last dword written (pre-increment)
  uint32_t fetch_constants = kUnknown;       // 32 slots x 6 dwords (register 0x4800)
  uint32_t vs_float_constants = kUnknown;    // 256 vec4 (0x4000)
  uint32_t ps_float_constants = kUnknown;    // 256 vec4 (0x4400)
  uint32_t bool_loop_constants = kUnknown;   // 0x4900

  // The register shadow, in contiguous groups of registers.
  struct RegisterGroup {
    uint32_t offset = 0;
    uint16_t first_register = 0;
    uint16_t count = 0;
  };
  std::array<RegisterGroup, 8> register_groups{};

  uint32_t fence_counter = kUnknown;     // u32: fence of the command buffer segment being written
  uint32_t write_back_block = kUnknown;  // u32 pointer: [+0] last fence the GPU reached
  uint32_t flags = kUnknown;             // u32: tiling and predication flag bytes

  uint32_t vertex_declaration = kUnknown;
  uint32_t index_buffer = kUnknown;
  uint32_t render_targets = kUnknown;  // 4 surface pointers
  uint32_t depth_stencil = kUnknown;
  uint32_t vertex_buffers = kUnknown;  // one pointer per stream
  uint32_t textures = kUnknown;        // one pointer per sampler
  uint32_t viewport = kUnknown;        // x, y, width, height, min z, max z (floats)
  uint32_t pixel_shader = kUnknown;
  uint32_t vertex_shader = kUnknown;
  uint32_t predication_mask = kUnknown;
  uint32_t tile_count = kUnknown;
  uint32_t tile_rects = kUnknown;  // x1, y1, x2, y2 per tile
};

// Everything XenosNative needs to know about one game's D3D library.
struct Profile {
  const char* game = "";
  const char* xdk_version = "";  // as read from the XEX (for example "2.0.5426")
  DeviceLayout device;
  // The game's output resolution (its front buffer): the image the render scale fits to the screen.
  uint32_t output_width = 1280, output_height = 720;
  // The return address of the Resolve call that copies the finished scene: after the game's
  // post-processing, before its HUD is composited. XenosNative's antialiasing (FXAA or SMAA) is applied to
  // that copy. 0: no antialiasing.
  uint32_t scene_resolve_caller = 0;
  // The game's hooks widen its 16:9 cameras by AspectFactor() (D43): XenosNative then fills an output of any
  // aspect ratio, with a separate render scale per axis, instead of showing the 16:9 image with bars.
  bool widens_cameras = false;
};

// Call once, before the guest starts (for example in ReXApp::OnPostSetup). Reads the cvars, logs the
// configuration and turns the hooks on if a feature needs them. The profile must outlive the run.
// graphics: the runtime's graphics system (the observer registers a draw observer there; the renderer
// turns its host rendering off). window: the game's window (the renderer presents there). Call it from
// the UI thread: the renderer detaches the SDK's presenter from the window.
// cache_root: the runtime's cache directory (ReXApp: runtime()->cache_root()), where the renderer keeps the
// shaders and pipelines between runs; empty for none.
void Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window,
                const std::filesystem::path& cache_root = {});

// Whether XenosNative draws the game (the xenos_native cvar, and the renderer started), as of Initialize.
// For game settings that XenosNative does not support yet (for example POTCAWE's 2x MSAA, rendered in two
// EDRAM tiles).
bool Renders();

// How much wider the rendered image is than the game's (output aspect ratio over the game's), when
// XenosNative fills an output that is not 16:9 (Profile::widens_cameras, D43): above 1 for a wider output,
// below 1 for a taller one. The game's camera hooks widen its 16:9 views by it. 1 otherwise. Fixed at
// Initialize.
double AspectFactor();

// Whether the draws from now on are a 2D layer laid out for the game's aspect ratio (a HUD, menus, a video
// behind them). While XenosNative fills an output of another aspect ratio (AspectFactor() != 1, D43), those
// draws go into a box of the game's aspect ratio in the middle of their render target instead of being
// stretched with the 3D view. Set by the game's hooks around its 2D views; off again at every Present.
void SetGameAspectBox(bool on);

}  // namespace xenos_native
