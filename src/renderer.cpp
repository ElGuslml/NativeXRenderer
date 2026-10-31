// NativeXRenderer - the renderer.
//
// With --native_x_renderer=true (read once, at startup), NativeXRenderer draws the game instead of the SDK's
// Xenos emulation:
//   - the SDK's command processor keeps executing the game's command buffer for synchronization only
//     (the SDK's recomp change 27: fences, interrupts, swaps, queries with fake sample counts), and
//     draws nothing on the host;
//   - the SDK's presenter is detached from the game's window, and NativeXRenderer presents there through
//     its own DiligentCore (Vulkan) swap chain. The SDK's ImGui overlays (its dialogs) are drawn over the frame
//     there (overlay.h, D75).
//
// Every D3D call is captured on the game's render thread, after the original ran, in the game's order, from
// the call's arguments and the state tracker (state.h), into a command (commands.h); the renderer's own thread
// runs the commands (D71): render targets, clears, resolves and presenting here, draws in draw.cpp. With
// native_x_renderer_thread off, each command runs right away on the game's thread.

#include "renderer.h"

#include "aa.h"
#include "upscaler.h"
#include "blit.h"
#include "commands.h"
#include "device.h"
#include "draw.h"
#include "gamma.h"
#include "overlay.h"
#include "queries.h"
#include "internal.h"
#include "state.h"
#include "targets.h"

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/window.h>

#include "Query.h"
#include "RenderDeviceVk.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>

REXCVAR_DEFINE_BOOL(native_x_renderer, false, "NativeXRenderer",
                    "Draw the game with NativeXRenderer instead of the SDK's Xenos emulation (read at startup). Off "
                    "in the library; a game that is ready turns it on in its own configuration (POTCAWE: D56). "
                    "If it cannot start, the Xenos path draws");
REXCVAR_DEFINE_STRING(native_x_renderer_antialiasing, "smaa", "NativeXRenderer",
                      "Antialiasing (read at startup): smaa or fxaa on the finished scene, before the HUD (needs the "
                      "game's profile to name the scene's resolve); dlaa, NVIDIA DLSS at the render resolution before "
                      "the game's post-processing (needs the game's hooks to mark its views; SMAA where DLSS is not "
                      "available); or off. Experimental, not finished (D53): dlss_quality, dlss_balanced, "
                      "dlss_performance or dlss_ultra_performance, the 3D view drawn at a lower resolution and "
                      "upscaled by DLSS");
REXCVAR_DEFINE_BOOL(native_x_renderer_object_motion, true, "NativeXRenderer",
                    "DLSS (DLAA and the DLSS modes): motion vectors of their own for the objects that move on "
                    "their own, characters and animated props (read at startup); false: the camera's motion only");
REXCVAR_DEFINE_BOOL(native_x_renderer_pipeline_storage, true, "NativeXRenderer",
                    "Keep the shaders and pipelines used between runs, and prepare them at startup (read at startup)");
REXCVAR_DEFINE_BOOL(native_x_renderer_wait_offscreen_pipelines, true, "NativeXRenderer",
                    "A draw into a target not of the game's output size (post-processing, masks, shadows) waits for "
                    "its shaders and pipeline instead of being skipped (D101); false: skipped, as the scene's draws");
REXCVAR_DEFINE_BOOL(native_x_renderer_fill_screen, true, "NativeXRenderer",
                    "Fill an output that is not 16:9, with a render scale per axis, when the game's hooks widen its "
                    "cameras (read at startup); false: the 16:9 image with bars");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_aspect, 0.0, "NativeXRenderer",
                      "With native_x_renderer_fill_screen, the image's aspect ratio (read at startup, D104): 0 = the "
                      "output's, which it fills; another (2.3889 for 21:9, 3.5556 for 32:9) renders the game at that "
                      "ratio, with bars where the output is narrower or wider");
REXCVAR_DEFINE_BOOL(native_x_renderer_thread, true, "NativeXRenderer",
                    "Run the renderer on a thread of its own (read at startup, D71): the game's render thread only "
                    "captures what each D3D call needs, and the renderer's thread draws, through DiligentCore and the "
                    "driver. Off: all of it on the game's render thread, inside its D3D calls");
REXCVAR_DEFINE_BOOL(native_x_renderer_present_with_swaps, false, "NativeXRenderer",
                    "Show each frame when the command processor executes the game's swap for it, as the SDK's "
                    "presenters do, instead of when the game calls Present (read at startup, D86): the game's CPU "
                    "runs a frame ahead, and its delays (Kameo's Bink videos) then do not reach the screen. Needs "
                    "the renderer's own thread (native_x_renderer_thread)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_resolution_scale, 0.0, "NativeXRenderer",
                      "Render scale, host pixels per guest pixel (read at startup). 0: from the screen, the largest "
                      "at which the game's image fits the monitor or the window (at least 1)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_shadow_scale, 0.0, "NativeXRenderer",
                      "The shadow maps' scale, host pixels per guest pixel on both axes (read at startup, D92): the "
                      "depth passes with no color target that are not of the game's output size are drawn at least "
                      "at this scale (2: twice the game's resolution, 4: four times). 0, or below the render scale: "
                      "the render scale, as everything else");
REXCVAR_DEFINE_INT32(native_x_renderer_resolved_memory_mb, 384, "NativeXRenderer",
                     "Resolved textures unused for a minute that still hold the game's resolve are kept while all the "
                     "resolved textures take at most this many MB, as the console's memory keeps a resolve until "
                     "something writes there; above it the least recently used are freed (D96). 0: freed after a "
                     "minute unused, as before D96");
REXCVAR_DEFINE_INT32(native_x_renderer_upload_ahead, 256, "NativeXRenderer",
                     "Uploads ahead (D98): the vertex and index data of up to this many draws queued behind a draw are "
                     "uploaded with its own, up to the frame's end, so that their copies end the render pass once; 0: "
                     "each draw uploads its own");

namespace native_x_renderer::renderer {
namespace {

using namespace internal;
using namespace Diligent;

constexpr uint32_t kRbCopyDestInfo = 0x231B;
constexpr uint32_t kRbDepthClear = 0x231D;

const Profile* g_profile = nullptr;
std::unique_ptr<Device> g_device;
RenderTargets g_targets;
// The order of the writes to host targets: each draw or clear stamps the targets it writes (D95).
uint64_t g_target_writes = 0;
ResolvedTextures g_resolved;
Blitter g_blit;
// The SDK's dialogs over the presented frame (D75). Never destroyed: the runtime's ImGuiDrawer keeps a pointer.
Overlay& g_overlay = *new Overlay;
rex::ui::Window* g_window = nullptr;
DisplayGamma g_gamma;  // the guest's display gamma ramp, applied when presenting (D36)
Antialiasing g_aa;     // on the finished scene (D41)
bool g_scene_antialiased = false;  // this frame's scene copy antialiased already (Profile::scene_resolve_once, D94)
Upscaler g_upscaler;   // NVIDIA DLSS (Phase 5, D46)
OcclusionQueries g_queries;  // the guest's occlusion queries (D32)
bool g_queries_on = false;
RenderScale g_scale;  // host pixels per guest pixel (D37), per axis when filling the output (D43)
double g_aspect_factor = 1.0;  // g_scale.x / g_scale.y: how much wider the image is than the game's
bool g_aspect_box = false;     // the draws are a 2D layer kept at the game's aspect ratio (D43)
// DLSS (D47, D49): on when native_x_renderer_antialiasing is dlaa or a DLSS mode and DLSS is available. Between
// BeginView with a matrix and EndViewGeometry the draws into targets of the game's output size are the main
// 3D view's: jittered, and their color and depth targets kept for DLSS. With a DLSS mode those targets are
// the reduced ones (targets.h), at g_reduced_scale; EndViewGeometry brings them to the render scale.
bool g_dlss = false;
bool g_object_motion = false;  // D48, with DLSS
bool g_reduced = false;        // D49: the main 3D view at a lower scale
RenderScale g_reduced_scale;
// D92: the shadow maps' scale, higher than the render scale when native_x_renderer_shadow_scale asks for it.
RenderScale g_shadow_scale;
bool g_shadow_scaled = false;
bool g_scene_view = false;
std::array<float, 16> g_scene_view_projection{};
HostTarget* g_scene_color = nullptr;
HostTarget* g_scene_depth = nullptr;
GuestSurface g_scene_color_surface;
// The reduced targets drawn or cleared in this view (D49), with their surfaces.
std::vector<std::pair<GuestSurface, HostTarget*>> g_reduced_used;
std::filesystem::path g_cache_root;  // the runtime's cache directory (D42)
std::unique_ptr<DrawPath> g_draw;  // created on the first draw (it needs the tracker's register file)
bool g_draw_failed = false;
bool g_tiling = false;  // between BeginTiling and EndTiling
// The size of a tiled pass's image, from BeginTiling's tile rectangles: the GPU draws the pass once per tile
// into a surface of one tile's size, NativeXRenderer once into targets of every tile's (D60).
uint32_t g_tiled_width = 0, g_tiled_height = 0;
uint32_t g_frames_presented = 0;

// The renderer's thread (D71): the commands the game's thread captures, run in order.
CommandQueue g_queue;
// Presenting with the guest's swaps (D86): on; the swaps the command processor has executed (its observer, on
// its thread); the oldest swap a composed frame waits for (0: none); the last tag given at capture (the game's
// thread). The game's CPU runs a frame ahead of the command processor: a frame's swap executes after the next
// frame's Present has run, so up to two composed frames wait, each in an image of its own (three, one being
// composed), copied into the swap chain's back buffer when shown.
rex::system::IGraphicsSystem* g_graphics = nullptr;
bool g_present_with_swaps = false;
// Uploads ahead (D98): the draws to look ahead (native_x_renderer_upload_ahead, read once a frame), the registers
// the ranges come from as they will be at each queued command (the fetch constants and the index buffer's; the
// queued records applied to a copy, the rest of which is not read), the ranges gathered, and the command up to which
// the data has been requested.
int32_t g_upload_ahead = 0;
std::unique_ptr<rex::graphics::RegisterFile> g_ahead_registers;
std::vector<std::pair<uint32_t, uint32_t>> g_ahead_ranges;
uint64_t g_ahead_until = 0;
std::atomic<uint64_t> g_swaps_executed{0}, g_pending_swap{0};
uint64_t g_last_swap_tag = 0;
struct PendingFrame {
  uint64_t tag;
  uint32_t image;
  std::chrono::steady_clock::time_point since;
};
constexpr uint32_t kMaxPendingFrames = 2;
std::array<PendingFrame, kMaxPendingFrames> g_pending_frames{};
uint32_t g_pending_count = 0;
struct FrameImage {
  Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
  Diligent::ITextureView *rtv = nullptr, *srv = nullptr;
};
std::array<FrameImage, kMaxPendingFrames + 1> g_frame_images;
uint32_t g_next_frame_image = 0;
// A composed frame not shown within this long (the command processor stopped) is shown.
constexpr auto kSwapTimeout = std::chrono::milliseconds(100);
// Capturing a command: the game's render thread, or a game's hook (BeginView...) if it ran on another thread.
std::mutex g_capture_mutex;
// The renderer's copy of the GPU registers, on its thread: what the draw path reads. The tracker's records
// are applied to it (D73).
std::unique_ptr<rex::graphics::RegisterFile> g_registers;
uint32_t g_captured = 0;  // commands captured since the last frame end, on the game's thread

// The arguments of the outermost call in progress (the renderer acts when it returns).
struct Arguments {
  Call call{};
  std::array<uint32_t, 8> r{};  // r3..r10
  double f1 = 0;
  uint32_t lr = 0;  // the return address in the caller
  uint32_t outer_lr = 0;  // its caller's (D65), for a Resolve when the profile checks it
};
Arguments g_call;
uint32_t g_front_buffer = 0;  // the texture object Present was given this frame

// Per frame, for the log.
struct FrameStats {
  uint32_t clears = 0, resolves = 0, skipped_resolves = 0;
} g_stats;

// GPU time per frame: a duration query around each frame's commands (from one Present to the next), read
// when the query is reused, kQueries frames later. A query not finished by then is counted as late: the
// GPU is more than kQueries frames behind the CPU.
class FrameTimer {
 public:
  void Initialize(IRenderDevice* device) {
    if (device->GetDeviceInfo().Features.DurationQueries != DEVICE_FEATURE_STATE_ENABLED) return;
    for (auto& query : queries_) {
      QueryDesc desc;
      desc.Name = "NativeXRenderer frame time";
      desc.Type = QUERY_TYPE_DURATION;
      device->CreateQuery(desc, &query);
      if (!query) return;
    }
    enabled_ = true;
  }
  void Begin(IDeviceContext* context) {
    if (!enabled_) return;
    if (pending_[index_]) {
      QueryDataDuration data;
      if (queries_[index_]->GetData(&data, sizeof(data), true) && data.Frequency) {
        double ms = double(data.Duration) * 1000.0 / double(data.Frequency);
        total_ms_ += ms;
        max_ms_ = std::max(max_ms_, ms);
        ++frames_;
      } else {
        ++late_;
      }
      pending_[index_] = false;
    }
    context->BeginQuery(queries_[index_]);
    active_ = true;
  }
  void End(IDeviceContext* context) {
    if (!active_) return;
    context->EndQuery(queries_[index_]);
    pending_[index_] = true;
    index_ = (index_ + 1) % kQueries;
    active_ = false;
  }
  struct Result {
    double average_ms = 0, max_ms = 0;  // 0 if no frame was measured
    uint32_t frames = 0, late = 0;
  };
  // Since the last call.
  Result Take() {
    Result result{frames_ ? total_ms_ / frames_ : 0.0, max_ms_, frames_, late_};
    total_ms_ = max_ms_ = 0;
    frames_ = late_ = 0;
    return result;
  }

 private:
  static constexpr uint32_t kQueries = 32;
  std::array<RefCntAutoPtr<IQuery>, kQueries> queries_;
  std::array<bool, kQueries> pending_{};
  uint32_t index_ = 0;
  bool enabled_ = false, active_ = false;
  double total_ms_ = 0, max_ms_ = 0;
  uint32_t frames_ = 0, late_ = 0;
} g_timer;

// Frame pacing on the CPU side, for the log: time spent in the renderer (on the render thread, inside the
// game's D3D calls), time waiting for the GPU (Device::Present), interval between presents.
struct Pacing {
  using Clock = std::chrono::steady_clock;
  double renderer_ms = 0, wait_ms = 0, max_wait_ms = 0, interval_ms = 0, max_interval_ms = 0;
  uint32_t frames = 0;
  Clock::time_point last_present{};
  // With native_x_renderer_present_with_swaps (D86): frames shown at their swap; shown earlier because two
  // newer frames were waiting, or because the swap did not come in time; not shown (a newer one's swap had
  // executed too).
  uint32_t at_swap = 0, before_swap = 0, timed_out = 0, skipped = 0;

  static double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
} g_pacing;

// Host objects created since the last log line: shader analyses and pipelines on the render thread,
// translations on worker threads (D34).
struct Creation {
  CreationTime analyses, translations, pipelines;
  uint64_t translating_draws = 0;  // skipped while a shader was translated (D34)
  uint64_t compiling_draws = 0;    // skipped while their pipeline compiled (D28)
  // Draws into offscreen targets that waited for their shaders and pipeline instead (D101), and how long.
  uint64_t waited_draws = 0;
  double waited_ms = 0.0, max_waited_ms = 0.0;
} g_creation;

// Buffer copies (each ends the render pass) and guest memory uploads per frame, since the last log line.
struct Uploads {
  uint64_t frames = 0, copies = 0, max_copies = 0, uploaded_bytes = 0, unchanged_bytes = 0;
  uint64_t unwatched_hashed_bytes = 0, unwatched_pages = 0;  // D97
  uint64_t unwatched_copy_bytes = 0;                         // D110
  uint64_t ahead_batches = 0, ahead_draws = 0;               // D98
} g_uploads;

// Object motion (D48) since the last log line: main view draws matched with the last frame's, those that
// moved, and their motion draws.
struct MotionCounts {
  uint64_t matched = 0, moved = 0, drawn = 0, max_drawn = 0;
} g_motion;

class RendererTime {
 public:
  RendererTime() : start_(Pacing::Clock::now()) {}
  ~RendererTime() { g_pacing.renderer_ms += Pacing::Ms(Pacing::Clock::now() - start_); }

 private:
  Pacing::Clock::time_point start_;
};

// A surface the device has bound (render targets 0-3, 4 the depth), as its object says: read on the game's
// thread when a call is captured.
GuestSurface ReadBoundSurface(const uint8_t* base, uint32_t device, uint32_t index) {
  const DeviceLayout& d = g_profile->device;
  const ObjectLayout& o = g_profile->objects;
  return index < 4 ? ReadSurface(base, Load32(base, device + d.render_targets + 4 * index), false, o)
                   : ReadSurface(base, Load32(base, device + d.depth_stencil), true, o);
}

// A bound surface as NativeXRenderer draws it: in a tiled pass, at the size of every tile (D60).
GuestSurface Tiled(GuestSurface s) {
  if (g_tiling && s) {
    s.width = std::max(s.width, g_tiled_width);
    s.height = std::max(s.height, g_tiled_height);
  }
  return s;
}

float GuestFloat(const uint8_t* base, uint32_t address) { return LoadFloat(base, address); }

// <cache root>/native_x_renderer/<title id>: the stored shaders and pipelines of this game (D42).
std::filesystem::path StoragePath(uint32_t title_id) {
  if (g_cache_root.empty() || !REXCVAR_GET(native_x_renderer_pipeline_storage)) return {};
  return g_cache_root / "native_x_renderer" / fmt::format("{:08X}", title_id);
}

// The host target a draw, a clear or a resolve uses for a guest surface: in the main 3D view, while DLSS
// draws it at a lower scale, the reduced one for the targets of the game's output size (D49); with a shadow scale,
// the shadow map's for a depth surface bound with no color surface (a draw's or a clear's binding) that is not of
// the game's output size, and from then on for that surface, except in a draw with a color surface, which gets the
// target at the render scale, of the same scale as the color's (D92). A clear of a shadow map gets its target
// whatever is bound: the XDK's ClearF clears depth through an aliased color surface (POTCAWE's).
enum class Binding { kUnknown, kDepthOnly, kWithColor };

Binding BindingOf(const Command& c) {
  bool color = c.surfaces[0] || c.surfaces[1] || c.surfaces[2] || c.surfaces[3];
  return color ? Binding::kWithColor : Binding::kDepthOnly;
}

HostTarget* TargetFor(const GuestSurface& surface, Binding binding = Binding::kUnknown) {
  bool output_size =
      surface && surface.width == g_profile->output_width && surface.height == g_profile->output_height;
  bool reduced = g_reduced && g_scene_view && output_size;
  bool shadow = false;
  if (g_shadow_scaled && surface && surface.depth && !output_size && binding != Binding::kWithColor) {
    shadow = g_targets.IsShadow(surface);
    if (!shadow && binding == Binding::kDepthOnly) {
      g_targets.MarkShadow(surface);
      shadow = true;
      REXLOG_INFO("[native_x_renderer] shadow map: the {}x{} depth surface at EDRAM base {} (pitch {}), drawn with no "
                  "color target, at the shadow scale {:.4f} (D92)",
                  surface.width, surface.height, surface.edram_base(), surface.pitch(), g_shadow_scale.x);
    }
  }
  using Kind = RenderTargets::Kind;
  HostTarget* target = g_targets.Get(surface, reduced ? Kind::kReduced : shadow ? Kind::kShadow : Kind::kRender);
  auto same = [&](const auto& used) { return used.second == target; };
  if (reduced && target && std::none_of(g_reduced_used.begin(), g_reduced_used.end(), same)) {
    g_reduced_used.emplace_back(surface, target);
  }
  return target;
}

// The reduced targets of the view that ended, at the render scale for what follows (D49): the color DLSS
// upscaled (`upscaled`) is there already; the others are scaled, color filtered and depth nearest.
void PromoteReduced(const HostTarget* upscaled) {
  if (g_reduced_used.empty()) return;
  IDeviceContext* context = g_device->context();
  for (const auto& [surface, reduced] : g_reduced_used) {
    if (reduced == upscaled) continue;
    HostTarget* full = g_targets.Get(surface);
    if (!full || reduced->samples > 1) continue;
    Blitter::Rect from{0, 0, reduced->width, reduced->height}, to{0, 0, full->width, full->height};
    g_blit.Copy(context, reduced->srv, reduced->depth ? Blitter::Source::kDepthToDepth : Blitter::Source::kColorScaled,
                from, full->view, to);
    full->written = ++g_target_writes;
  }
  g_reduced_used.clear();
  if (g_draw) g_draw->TargetsChanged();
}

// Called by a draw whose own requests copied, ending the render pass (D98): the draws queued behind it, up to the
// frame's end, have their vertex and index ranges requested now, at once, their copies recorded with the draw's. The
// game's thread has written their data before queueing them, and does not reuse it before the command processor's
// next signal, which waits for this thread; each draw still requests its own ranges when it runs, finding them
// uploaded. A resolve in between needs no stop: it writes the mirror on the GPU after these copies, and the pages a
// resolve made valid are not uploaded again. A draw that copies nothing costs nothing here: in games that upload
// little (GRAW, POTCAWE) a window at every draw, of the 2 or 3 draws queued behind it, cost more than it saved.
void UploadAhead() {
  if (g_upload_ahead <= 0 || !g_queue.threaded() || !g_draw) return;
  uint64_t index = g_queue.running_index();
  if (index + 1 < g_ahead_until) return;  // the next draws are in this window already
  const Command* next = g_queue.Peek(1);
  if (!next) return;
  if (!g_ahead_registers) g_ahead_registers = std::make_unique<rex::graphics::RegisterFile>();
  uint32_t* ahead_values = g_ahead_registers->values;
  const uint32_t* values = g_registers->values;
  constexpr uint32_t kFetch = rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, kFetchCount = 32 * 6;
  std::memcpy(ahead_values + kFetch, values + kFetch, kFetchCount * sizeof(uint32_t));
  ahead_values[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR] = values[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR];
  ahead_values[rex::graphics::XE_GPU_REG_VGT_DMA_BASE] = values[rex::graphics::XE_GPU_REG_VGT_DMA_BASE];
  g_ahead_ranges.clear();
  uint32_t draws = 0, ahead = 1;
  for (; next && draws < uint32_t(g_upload_ahead); next = g_queue.Peek(++ahead)) {
    if (next->type == Command::Type::kPresent || next->type == Command::Type::kFrameEnd) break;
    ApplyRecord(next->registers.data(), next->registers.size(), ahead_values);
    if (next->type == Command::Type::kDraw && next->programs[0]) {
      g_draw->AddDrawRanges(*next->programs[0], *g_ahead_registers, g_ahead_ranges);
      ++draws;
    }
  }
  g_ahead_until = index + ahead;
  g_draw->UploadAhead(g_ahead_ranges, draws);
}

void ExecuteDraw(const Command& c) {
  RendererTime time;
  if (!g_draw) {
    if (g_draw_failed) return;
    auto* kernel = rex::system::kernel_state();
    auto draw = std::make_unique<DrawPath>();
    if (!kernel || !kernel->memory() ||
        !draw->Initialize(*g_device, *g_registers, *kernel->memory(), g_resolved, g_scale,
                          StoragePath(kernel->title_id()))) {
      REXLOG_ERROR("[native_x_renderer] could not set up the draw path; draws are skipped");
      g_draw_failed = true;
      return;
    }
    g_draw = std::move(draw);
    g_draw->SetUploadAhead(UploadAhead);
    g_draw->SetTextureSizeConstants(g_profile->texture_size_constants, g_profile->texture_size_constant_count);
  }
  std::array<HostTarget*, 5> targets;
  const HostTarget* first_target = nullptr;
  for (uint32_t i = 0; i < 5; ++i) {
    targets[i] = TargetFor(Tiled(c.surfaces[i]), BindingOf(c));
    if (targets[i]) targets[i]->written = ++g_target_writes;
    if (targets[i] && !first_target) first_target = targets[i];
  }
  // D101: a draw into a target of another size than the game's output (the first color target, else depth) waits for
  // its pipeline. Such passes may be drawn once and resolved: GH Metallica's post-processing mask, drawn at the
  // career's first song load, showed garbage for the session when its draw was skipped on a first run.
  g_draw->SetWaitForPipelines(REXCVAR_GET(native_x_renderer_wait_offscreen_pipelines) && first_target &&
                              (first_target->guest_width != g_profile->output_width ||
                               first_target->guest_height != g_profile->output_height));
  g_draw->SetAspectBox(g_aspect_box);
  bool scene = g_scene_view && g_dlss;
  g_draw->SetJitter(scene, g_upscaler.jitter_x(), g_upscaler.jitter_y(), g_profile->output_width,
                    g_profile->output_height);
  g_draw->SetObjectMotion(g_object_motion);
  std::array<DrawPath::Copy, 2> copies;
  for (uint32_t i = 0; i < c.copy_count; ++i) {
    copies[i] = {c.copies[i].address, c.data.data() + c.copies[i].offset, c.copies[i].size};
  }
  g_draw->Draw(c.programs[0] ? *c.programs[0] : StateTracker::kNoProgram,
               c.programs[1] ? *c.programs[1] : StateTracker::kNoProgram, std::span(copies.data(), c.copy_count),
               targets, g_tiling);
  if (scene && targets[0] && targets[0]->guest_width == g_profile->output_width &&
      targets[0]->guest_height == g_profile->output_height) {
    g_scene_color = targets[0];
    g_scene_color_surface = Tiled(c.surfaces[0]);
    if (targets[4]) g_scene_depth = targets[4];
  }
}

// ClearF(dev, flags, rect, color*, Z (f1), stencil): bits 0-3 clear RT0-3, 0x10 depth, 0x20 stencil. The
// targets are the ones bound when ClearF ran, and the color what it pointed to then.
// A rectangle of guest pixels on a host target at its scale (the render scale, D37; the reduced one in an upscaled
// view, D49).
Blitter::Rect HostRect(const RenderScale& scale, const Blitter::Rect& r) {
  int32_t x0 = scale.HostX(r.x), y0 = scale.HostY(r.y);
  return Blitter::Rect{x0, y0, uint32_t(scale.HostX(int64_t(r.x) + r.width) - x0),
                       uint32_t(scale.HostY(int64_t(r.y) + r.height) - y0)};
}

// `r` (guest pixels) within the target, and whether it covers it whole.
bool ClipToTarget(const HostTarget& target, Blitter::Rect& r) {
  int32_t x1 = std::clamp(r.x, 0, int32_t(target.guest_width));
  int32_t y1 = std::clamp(r.y, 0, int32_t(target.guest_height));
  int32_t x2 = std::clamp(int32_t(int64_t(r.x) + r.width), x1, int32_t(target.guest_width));
  int32_t y2 = std::clamp(int32_t(int64_t(r.y) + r.height), y1, int32_t(target.guest_height));
  r = {x1, y1, uint32_t(x2 - x1), uint32_t(y2 - y1)};
  return x1 == 0 && y1 == 0 && r.width == target.guest_width && r.height == target.guest_height;
}

// The blitter's fills are single-sampled (D80): a multisampled target's part is not cleared, logged once.
bool FillableTarget(const HostTarget& target) {
  if (target.samples <= 1) return true;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_WARN("[native_x_renderer] a clear of part of a {}x{} target with {} samples was skipped: not supported",
                target.guest_width, target.guest_height, target.samples);
  }
  return false;
}

// Clears a color target's rectangle (guest pixels, within it): the whole target through DiligentCore, a part of it
// filled by the blitter (D80).
void ClearColorRect(HostTarget* target, Blitter::Rect r, const float color[4]) {
  auto* context = g_device->context();
  if (ClipToTarget(*target, r)) {
    context->SetRenderTargets(1, &target->view, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearRenderTarget(target->view, color, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  } else if (r.width && r.height && FillableTarget(*target)) {
    g_blit.FillColor(context, target->view, HostRect(target->scale, r), color);
  }
  target->written = ++g_target_writes;
  if (g_draw) g_draw->TargetsChanged();
}

// The same for a depth-stencil target's depth and/or stencil.
void ClearDepthStencilRect(HostTarget* target, Blitter::Rect r, bool depth, float z, bool stencil, uint8_t s) {
  auto* context = g_device->context();
  if (ClipToTarget(*target, r)) {
    CLEAR_DEPTH_STENCIL_FLAGS clear_flags = CLEAR_DEPTH_FLAG_NONE;
    if (depth) clear_flags |= CLEAR_DEPTH_FLAG;
    if (stencil) clear_flags |= CLEAR_STENCIL_FLAG;
    context->SetRenderTargets(0, nullptr, target->view, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearDepthStencil(target->view, clear_flags, z, s, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  } else if (r.width && r.height && FillableTarget(*target)) {
    g_blit.FillDepthStencil(context, target->view, HostRect(target->scale, r), depth, z, stencil, s);
  }
  target->written = ++g_target_writes;
  if (g_draw) g_draw->TargetsChanged();
}

void ExecuteClear(const Command& c) {
  RendererTime time;
  uint32_t flags = c.r[1], stencil = c.r[5];
  // A rectangle (x1, y1, x2, y2): a target it covers is cleared whole (D78; Kameo clears its scene with the
  // target's own rectangle), a part of one is filled (D80).
  auto rect = [&](const HostTarget* target) {
    if (!c.has_rect) return Blitter::Rect{0, 0, target->guest_width, target->guest_height};
    return Blitter::Rect{c.rect[0], c.rect[1], uint32_t(std::max(0, c.rect[2] - c.rect[0])),
                         uint32_t(std::max(0, c.rect[3] - c.rect[1]))};
  };
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(flags & (1u << i))) continue;
    HostTarget* target = TargetFor(Tiled(c.surfaces[i]));
    if (!target || !c.has_color) continue;
    ClearColorRect(target, rect(target), c.color.data());
  }
  if (flags & 0x30) {
    Binding binding = BindingOf(c) == Binding::kDepthOnly ? Binding::kDepthOnly : Binding::kUnknown;
    HostTarget* target = TargetFor(Tiled(c.surfaces[4]), binding);
    if (target) {
      ClearDepthStencilRect(target, rect(target), (flags & 0x10) != 0, float(c.f1), (flags & 0x20) != 0,
                            uint8_t(stencil));
    }
  }
  ++g_stats.clears;
}

// The bytes a resolve destination covers in guest memory from its address: the base level, every slice or face,
// laid out as its fetch constant says (dword 0 the pitch and tiling, dword 5 the packed mips). Resolves write
// level 0 only (below).
uint32_t ResolveGuestBytes(uint32_t fetch0, uint32_t fetch5, uint32_t format, uint32_t width, uint32_t height,
                           uint32_t slices, bool cube) {
  namespace rg = rex::graphics;
  rg::texture_util::TextureGuestLayout layout = rg::texture_util::GetGuestTextureLayout(
      cube ? rg::xenos::DataDimension::kCube : rg::xenos::DataDimension::k2DOrStacked, (fetch0 >> 22) & 0x1FF, width,
      height, cube ? 6 : slices, (fetch0 >> 31) != 0, rg::xenos::TextureFormat(format), ((fetch5 >> 11) & 1) != 0,
      true, 0);
  return layout.base.level_data_extent_bytes;
}

// The clears a resolve does after its copy (D80), over the rectangle it copied (guest pixels of the source):
// 0x100 its color source with the call's color, 0x200 the depth-stencil surface with Z (f1) and RB_DEPTH_CLEAR's
// stencil, as the EDRAM copy does them (the SDK's resolve: the color one only when the source is a color target).
void ResolveClears(const Command& c, HostTarget* source, const Blitter::Rect& copied) {
  uint32_t flags = c.r[1];
  if ((flags & 0x100) && c.has_color && source && !source->depth && (flags & 7) < 4) {
    ClearColorRect(source, copied, c.color.data());
  }
  if (flags & 0x200) {
    HostTarget* depth = (flags & 7) == 4 ? source : TargetFor(Tiled(c.surfaces[4]));
    if (depth && depth->depth) {
      ClearDepthStencilRect(depth, copied, true, float(c.f1), true, uint8_t(c.clear_stencil));
    }
  }
}

// Resolve(dev, flags, srcRect*, destTexture, destPoint*, level, slice, clearColor*, Z (f1), stencil, ...).
// flags: 0-3 = RT0-3, 4 = depth; 0x10-0x80 = one sample (fragment 0-3) instead of the average; 0x100, 0x200:
// clear the color, the depth and stencil afterwards (ResolveClears).
void ExecuteResolve(const Command& c) {
  RendererTime time;
  uint32_t flags = c.r[1], level = c.r[5], slice = c.r[6];
  GuestSurface surface = Tiled(c.surfaces[0]);
  HostTarget* source = TargetFor(surface);
  // The rectangle the copy reads, then narrowed to what it copied: the clears' rectangle, which they get however
  // the copy goes.
  Blitter::Rect copied{0, 0, surface.width, surface.height};
  if (c.has_source_rect) {
    copied = {c.source_rect[0], c.source_rect[1], uint32_t(std::max(0, c.source_rect[2] - c.source_rect[0])),
              uint32_t(std::max(0, c.source_rect[3] - c.source_rect[1]))};
  }
  struct Clears {
    const Command& c;
    HostTarget*& source;  // the target read, which a multisampled surface's alias can replace (D95)
    const Blitter::Rect& copied;
    ~Clears() {
      if (c.r[1] & 0x300) ResolveClears(c, source, copied);
    }
  } clears{c, source, copied};
  if (!source || !c.has_texture || level) {
    ++g_stats.skipped_resolves;
    return;
  }
  // The destination texture's fetch constant: dword 1 base and format, dword 2 size, dword 5 the dimension. The
  // header holds the CPU address; draws sample the GPU address the device puts in the fetch constant.
  uint32_t fetch1 = c.fetch[1], fetch2 = c.fetch[2];
  uint32_t dimension = (c.fetch[5] >> 9) & 3;
  // A cube's faces are the slices of a host cube texture, `slice` the face (D51); its size is encoded as a
  // 2D texture's, with a stack depth of 6.
  bool cube = dimension == uint32_t(rex::graphics::xenos::DataDimension::kCube);
  if (dimension != uint32_t(rex::graphics::xenos::DataDimension::k2DOrStacked) && !cube) {
    // 1D and 3D destinations: not yet (a 3D texture's size is encoded differently, and it would need a host
    // texture of its dimension).
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("[native_x_renderer] resolve to a texture of dimension {}: not supported yet", dimension);
    }
    ++g_stats.skipped_resolves;
    return;
  }
  uint32_t address = GpuAddress(fetch1 & 0xFFFFF000u), format = fetch1 & 0x3F;
  uint32_t width = (fetch2 & 0x1FFF) + 1, height = ((fetch2 >> 13) & 0x1FFF) + 1, slices = (fetch2 >> 26) + 1;
  // From a shadow map, into a texture at its scale (D92).
  ResolvedTexture* target = g_resolved.GetOrCreate(address, format, width, height, slices, cube, source->shadow);
  ITextureView* target_view = target ? target->SliceView(slice) : nullptr;
  if (!target_view) {
    ++g_stats.skipped_resolves;
    return;
  }

  Blitter::Rect from{0, 0, surface.width, surface.height};
  if (c.has_source_rect) {
    int32_t x1 = c.source_rect[0], y1 = c.source_rect[1], x2 = c.source_rect[2], y2 = c.source_rect[3];
    from = {x1, y1, uint32_t(std::max(0, x2 - x1)), uint32_t(std::max(0, y2 - y1))};
  }
  Blitter::Rect to{0, 0, from.width, from.height};
  if (c.has_dest_point) {
    to.x = c.dest_point[0];
    to.y = c.dest_point[1];
  }
  // Clip to both textures, in guest pixels.
  from.width = std::min({from.width, source->guest_width - std::min(source->guest_width, uint32_t(from.x)),
                         target->guest_width - std::min(target->guest_width, uint32_t(to.x))});
  from.height = std::min({from.height, source->guest_height - std::min(source->guest_height, uint32_t(from.y)),
                          target->guest_height - std::min(target->guest_height, uint32_t(to.y))});
  to.width = from.width;
  to.height = from.height;
  copied = from;
  // A multisampled color surface whose EDRAM was last written through a single-sampled one (D95): the copy reads
  // that target, its pixels averaged by the filtered copy at half the size (2x2 for 4x MSAA, 1x2 for 2x) as the
  // resolve averages the samples, and the resolve's clears go there too.
  uint32_t samples_x = 1, samples_y = 1;
  HostTarget* alias = g_targets.FindMsaaAlias(surface, samples_x, samples_y);
  bool aliased = alias && alias != source && alias->written > source->written;
  if (aliased) {
    Blitter::Rect a{from.x * int32_t(samples_x), from.y * int32_t(samples_y), from.width * samples_x,
                    from.height * samples_y};
    ClipToTarget(*alias, a);
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("[native_x_renderer] a resolve through a {}x{} surface with {}x MSAA reads the {}x{} target drawn "
                  "at the same EDRAM (base {}): its samples averaged (D95)",
                  surface.width, surface.height, surface.samples(), alias->guest_width, alias->guest_height,
                  surface.edram_base());
    }
    if (((flags >> 4) & 0xF) != 0) {
      static bool logged_fragment = false;
      if (!logged_fragment) {
        logged_fragment = true;
        REXLOG_WARN("[native_x_renderer] a resolve of one sample through an aliased MSAA surface: averaged instead");
      }
    }
    source = alias;
    copied = from = a;
  }
  // Then on the host: the source at its scale (the render scale, D37; the reduced one in an upscaled view,
  // D49; a shadow map's, D92), the destination at its own (the render scale, or the shadow maps'). At one scale,
  // with a fractional scale the two rectangles' rounded sizes can differ by a pixel: the copy takes the smaller.
  // From a reduced target the copy scales.
  from = HostRect(source->scale, from);
  to = HostRect(target->scale, to);
  bool scaled = source->reduced || aliased;
  if (scaled && source->samples > 1) {
    ++g_stats.skipped_resolves;  // reduced targets have no MSAA (NativeXRenderer draws without it)
    return;
  }
  from.width = std::min(from.width, source->width - std::min(source->width, uint32_t(from.x)));
  from.height = std::min(from.height, source->height - std::min(source->height, uint32_t(from.y)));
  to.width = std::min(to.width, target->width - std::min(target->width, uint32_t(to.x)));
  to.height = std::min(to.height, target->height - std::min(target->height, uint32_t(to.y)));
  if (!scaled) {
    from.width = to.width = std::min(from.width, to.width);
    from.height = to.height = std::min(from.height, to.height);
  }

  // What the copy itself does, from the RB_COPY_DEST_INFO the call wrote.
  uint32_t dest_info = c.dest_info;
  Blitter::Options options;
  options.samples = source->samples;
  if (uint32_t fragments = (flags >> 4) & 0xF) {
    options.sample = int32_t(std::countr_zero(fragments));
  }
  options.swap_red_blue = (dest_info >> 24) & 1;
  // A 2_10_10_10_FLOAT target is RGBA16F on the host, which keeps what its format would not: an alpha above
  // 1 that a draw wrote, for one (POTCAWE's journal, D50). The copy takes the guest's range, as the EDRAM's
  // packing does on the Xenos path.
  namespace xenos = rex::graphics::xenos;
  options.float_7e3 =
      !source->depth && xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(surface.format())) ==
                            xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT;
  int32_t exp_bias = int32_t(dest_info << 10) >> 26;  // bits 16-21, signed
  options.scale = std::ldexp(1.0f, exp_bias);

  Blitter::Source kind = source->depth ? (source->samples > 1 ? Blitter::Source::kDepthMsaa : Blitter::Source::kDepth)
                                       : (source->samples > 1 ? Blitter::Source::kColorMsaa : Blitter::Source::kColor);
  if (scaled) kind = source->depth ? Blitter::Source::kDepthScaled : Blitter::Source::kColorScaled;
  g_blit.Copy(g_device->context(), source->srv, kind, from, target_view, to, options);
  // A depth resolve that a fetch reads as k_8_8_8_8 (for the stencil): the words too (D64).
  if (source->depth && target->read_packed && !scaled && !cube && source->stencil_srv) {
    if (ITextureView* packed = target->PackedSliceView(g_device->device(), slice)) {
      Blitter::Options packed_options;
      packed_options.stencil = source->stencil_srv;
      packed_options.depth_float24 = source->depth_float24;
      g_blit.Copy(g_device->context(), source->srv, Blitter::Source::kDepthStencilPacked, from, packed, to,
                  packed_options);
      target->packed_written = true;
    }
  }
  // The copy of the finished scene (the profile's scene_resolve_caller): antialiased in place (D41).
  // It is of the game's output size (D65).
  if (g_aa.mode() != Antialiasing::Mode::kOff && c.scene_caller && slice == 0 &&
      target->guest_width == g_profile->output_width &&
      target->guest_height == g_profile->output_height &&
      !(g_profile->scene_resolve_once && g_scene_antialiased)) {
    g_aa.Apply(g_device->context(), target->texture, target_view);
    g_scene_antialiased = true;
  }
  if (g_draw) g_draw->TargetsChanged();
  g_resolved.Touch(target, ResolveGuestBytes(c.fetch[0], c.fetch[5], format, width, height, slices, cube));
  ++g_stats.resolves;
}

// BeginTiling(dev, flags, count, tileRects*, clearColor*, Z (f1), stencil). The tile rectangles are
// D3DRECTs (x1, y1, x2, y2) in the coordinates of the whole image. The GPU replays the pass once per tile
// into surfaces of one tile's size, and the game resolves each tile's rectangle (POTCAWE, GRAW). NativeXRenderer
// draws the pass once: the surfaces bound in it get host targets of the size the tiles cover together, and
// each tile's resolve copies its own rectangle from there (D60).
void ExecuteBeginTiling(const Command& c) {
  RendererTime time;
  g_tiling = true;
  g_tiled_width = c.width;
  g_tiled_height = c.height;
  static uint32_t logged_width = 0, logged_height = 0;
  if (g_tiled_width != logged_width || g_tiled_height != logged_height) {
    logged_width = g_tiled_width;
    logged_height = g_tiled_height;
    REXLOG_INFO("[native_x_renderer] a tiled pass of {} tiles: drawn once, into targets of {}x{}", c.count,
                g_tiled_width, g_tiled_height);
  }
}

// EndTiling(dev, resolveFlags, resolveRects*, destTexture, clearColor*, Z (f1), stencil, params): a game can
// let it resolve the tiles. POTCAWE and GRAW resolve each tile themselves and pass no destination.
void ExecuteEndTiling(const Command& c) {
  RendererTime time;
  g_tiling = false;
  static bool logged = false;
  if (c.on && !logged) {
    logged = true;
    REXLOG_WARN("[native_x_renderer] EndTiling with a resolve destination: not supported yet");
  }
}

// Query Issue(query, flags): D3DISSUE_BEGIN (2) or D3DISSUE_END (1). An occlusion query's Issue points an
// EVENT_WRITE_ZPD at its sample counts (RB_SAMPLE_COUNT_ADDR: a normal packet with one tile, one
// bin-predicated packet per tile with several; the last is the last tile's block, which the end marker
// is in); other query types write none.
void ExecuteQuery(const Command& c) {
  RendererTime time;
  uint32_t flags = c.r[1];
  if (flags & 2) {
    // Samples are reported at the game's scale: from the scale of the targets the query's draws go to, the
    // reduced one in an upscaled view (D49).
    const RenderScale& scale = g_reduced && g_scene_view ? g_reduced_scale : g_scale;
    g_queries.SetSampleScale(double(scale.x) * double(scale.y));
    g_queries.Begin(g_device->context());
  } else if (flags & 1) {
    g_queries.End(g_device->context(), c.address);
  }
}

// The game's aspect box (D43) and its views (D47), in the order of its draws.
void ExecuteAspectBox(const Command& c) { g_aspect_box = c.on && g_aspect_factor != 1.0; }

void ExecuteBeginView(const Command& c) {
  g_scene_view = c.has_matrix;
  if (g_scene_view) g_scene_view_projection = c.matrix;
  g_scene_color = g_scene_depth = nullptr;
}

void ExecuteEndViewGeometry() {
  if (!g_scene_view) return;
  g_scene_view = false;
  RendererTime time;
  // DLSS submits the command buffer (upscaler.cpp), which an open occlusion query forbids.
  bool upscaled = false;
  if (g_dlss && g_scene_color && g_scene_depth && g_scene_depth->srv && !(g_queries_on && g_queries.open())) {
    // DLSS's result goes to the scene color at the render scale: the target itself, or for a reduced one the
    // surface's target at the render scale (D49).
    HostTarget* output = g_scene_color->reduced ? g_targets.Get(g_scene_color_surface) : g_scene_color;
    upscaled = output && g_upscaler.Apply(g_device->context(), g_scene_color->texture, g_scene_depth->texture,
                                          g_scene_depth->srv, output->texture, g_scene_view_projection,
                                          g_draw ? g_draw->jittered_z_min() : 0.0f,
                                          g_draw ? g_draw->jittered_z_max() : 1.0f, [](ITextureView* motion) {
                                            if (g_draw) g_draw->DrawObjectMotion(motion, g_scene_depth->view);
                                          });
    if (g_draw) g_draw->TargetsChanged();
  }
  PromoteReduced(upscaled ? g_scene_color : nullptr);
  g_scene_color = g_scene_depth = nullptr;
}

// Composes the frame into `back_buffer` (the swap chain's, or a frame image of its size, D86).
void PresentFrontBuffer(const Command& c, ITextureView* back_buffer) {
  auto* context = g_device->context();
  if (g_draw) g_draw->TargetsChanged();
  ResolvedTexture* front = nullptr;
  uint32_t fetch1 = 0;
  if (c.has_texture) {
    fetch1 = c.fetch[1];
    front = g_resolved.FindNewest(GpuAddress(fetch1 & 0xFFFFF000u));
    if (front) g_resolved.MarkUsed(front);
  }
  // The front buffer is read through its fetch constant, swizzle included: a resolve with the red/blue
  // swap (RB_COPY_DEST_INFO) writes BGRA, and the front buffer's swizzle swaps it back. Then the guest's
  // gamma ramp, at the front buffer's size (D36); the scaled copy below reads the result as it is.
  Blitter::Options options;
  ITextureView* source = nullptr;
  if (front) {
    options.swizzle = (c.fetch[3] >> 1) & 0xFFF;
    source = front->srv;
    if (ITextureView* gamma =
            g_gamma.Apply(context, g_blit, front->srv, front->width, front->height, fetch1 & 0x3F, options.swizzle)) {
      source = gamma;
      options.swizzle = Blitter::Options{}.swizzle;
    }
  }
  const float black[4] = {0, 0, 0, 1};
  context->SetRenderTargets(1, &back_buffer, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->ClearRenderTarget(back_buffer, black, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  if (!front) return;
  // Letterbox: keep the front buffer's aspect ratio in the window.
  const auto& desc = g_device->swap_chain()->GetDesc();
  float scale = std::min(float(desc.Width) / float(front->width), float(desc.Height) / float(front->height));
  uint32_t width = uint32_t(float(front->width) * scale), height = uint32_t(float(front->height) * scale);
  Blitter::Rect from{0, 0, front->width, front->height};
  Blitter::Rect to{int32_t((desc.Width - width) / 2), int32_t((desc.Height - height) / 2), width, height};
  g_blit.Copy(context, source, Blitter::Source::kColorScaled, from, back_buffer, to, options);
}

// Shows the composed frame: the swap chain's Present, the frame fence, the pacing. Present ends DiligentCore's
// command buffer: the draw path binds its state again (D86: this may come between the next frame's draws).
void FinishPresent() {
  g_timer.End(g_device->context());
  double wait_ms = g_device->Present();
  g_timer.Begin(g_device->context());
  if (g_draw) g_draw->TargetsChanged();
  g_pacing.wait_ms += wait_ms;
  g_pacing.renderer_ms -= wait_ms;
  g_pacing.max_wait_ms = std::max(g_pacing.max_wait_ms, wait_ms);
  auto now = Pacing::Clock::now();
  if (g_pacing.last_present != Pacing::Clock::time_point{}) {
    double interval = Pacing::Ms(now - g_pacing.last_present);
    g_pacing.interval_ms += interval;
    g_pacing.max_interval_ms = std::max(g_pacing.max_interval_ms, interval);
    ++g_pacing.frames;
  }
  g_pacing.last_present = now;
}

// The frame image to compose into (D86), created at the swap chain's size and format.
FrameImage* NextFrameImage() {
  FrameImage& image = g_frame_images[g_next_frame_image];
  g_next_frame_image = (g_next_frame_image + 1) % g_frame_images.size();
  const auto& desc = g_device->swap_chain()->GetDesc();
  if (!image.texture || image.texture->GetDesc().Width != desc.Width ||
      image.texture->GetDesc().Height != desc.Height) {
    TextureDesc texture;
    texture.Name = "NativeXRenderer frame image";
    texture.Type = RESOURCE_DIM_TEX_2D;
    texture.Width = desc.Width;
    texture.Height = desc.Height;
    texture.Format = desc.ColorBufferFormat;
    texture.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    image = {};
    g_device->device()->CreateTexture(texture, nullptr, &image.texture);
    if (!image.texture) return nullptr;
    image.rtv = image.texture->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    image.srv = image.texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  }
  return &image;
}

// Shows the waiting frame `index` (D86): its image copied into the back buffer, then FinishPresent. The frames
// before it are dropped.
void ShowPendingFrame(uint32_t index) {
  RendererTime time;
  FrameImage& image = g_frame_images[g_pending_frames[index].image];
  if (image.srv) {
    auto* context = g_device->context();
    auto* back_buffer = g_device->swap_chain()->GetCurrentBackBufferRTV();
    const auto& desc = g_device->swap_chain()->GetDesc();
    const auto& from_desc = image.texture->GetDesc();
    Blitter::Rect from{0, 0, from_desc.Width, from_desc.Height}, to{0, 0, desc.Width, desc.Height};
    g_blit.Copy(context, image.srv,
                from_desc.Width == desc.Width && from_desc.Height == desc.Height ? Blitter::Source::kColor
                                                                                 : Blitter::Source::kColorScaled,
                from, back_buffer, to);
  }
  FinishPresent();
  for (uint32_t i = index + 1; i < g_pending_count; ++i) g_pending_frames[i - index - 1] = g_pending_frames[i];
  g_pending_count -= index + 1;
  g_pending_swap.store(g_pending_count ? g_pending_frames[0].tag : 0, std::memory_order_relaxed);
}

// The waiting frames (D86): the newest whose swap has executed is shown (older ones are skipped); with `room`,
// the oldest is shown anyway if the queue is full; one that waited kSwapTimeout is shown. On the renderer's
// thread: before every command, at Present, and when the queue wakes it (the swap observer, or every few
// milliseconds while it waits).
void PresentPending(bool room = false) {
  if (!g_pending_count) return;
  uint64_t executed = g_swaps_executed.load(std::memory_order_acquire);
  int ready = -1;
  for (uint32_t i = 0; i < g_pending_count; ++i) {
    if (executed >= g_pending_frames[i].tag) ready = int(i);
  }
  if (ready >= 0) {
    ++g_pacing.at_swap;
    g_pacing.skipped += uint32_t(ready);
    ShowPendingFrame(uint32_t(ready));
  } else if (room && g_pending_count == kMaxPendingFrames) {
    ++g_pacing.before_swap;
    ShowPendingFrame(0);
  } else if (std::chrono::steady_clock::now() - g_pending_frames[0].since > kSwapTimeout) {
    ++g_pacing.timed_out;
    ShowPendingFrame(0);
  }
}

void ExecutePresent(const Command& c) {
  bool with_swap = g_present_with_swaps && c.swap_tag;
  if (with_swap) PresentPending(true);  // room for this frame
  RendererTime time;
  g_aspect_box = false;
  PromoteReduced(nullptr);  // a view without EndViewGeometry (D49)
  g_scene_view = false;
  g_scene_antialiased = false;
  if (g_dlss) g_upscaler.NextFrame();
  if (!g_device->UpdateSize()) return;
  const auto& desc = g_device->swap_chain()->GetDesc();
  // With the guest's swaps (D86): composed into a frame image now, shown when the command processor executes the
  // frame's swap, which comes after this command has run (the swap passes the signal gate).
  FrameImage* image = with_swap ? NextFrameImage() : nullptr;
  ITextureView* target = image ? image->rtv : g_device->swap_chain()->GetCurrentBackBufferRTV();
  PresentFrontBuffer(c, target);
  g_overlay.Present(g_device->context(), target, desc.Width, desc.Height, g_window);
  if (!image) {
    FinishPresent();
    return;
  }
  g_pending_frames[g_pending_count++] = {c.swap_tag, uint32_t(image - g_frame_images.data()),
                                         std::chrono::steady_clock::now()};
  g_pending_swap.store(g_pending_frames[0].tag, std::memory_order_relaxed);
  PresentPending();
}

// Runs a command on the renderer's thread (or right away without it).
void Execute(Command& c);

// ---------------------------------------------------------------------------------------------------
// Capturing, on the game's render thread (D71): what each call needs from guest memory and the tracker, read
// before the call returns to the game.

// One command: filled, then published when it goes out of scope.
class Capture {
 public:
  explicit Capture(Command::Type type) : lock_(g_capture_mutex), command(g_queue.Next()) {
    command.type = type;
    ++g_captured;
  }
  ~Capture() { g_queue.Publish(); }
  Capture(const Capture&) = delete;
  Capture& operator=(const Capture&) = delete;

 private:
  std::lock_guard<std::mutex> lock_;

 public:
  Command& command;
};


void CaptureClear(PPCContext& ctx, const uint8_t* base) {
  uint32_t device = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, color = ctx.r6.u32;
  Capture capture(Command::Type::kClear);
  Command& c = capture.command;
  c.r = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  c.f1 = ctx.f1.f64;
  c.has_rect = rect != 0;
  if (c.has_rect) {
    for (uint32_t i = 0; i < 4; ++i) c.rect[i] = int32_t(Load32(base, rect + 4 * i));
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (flags & (1u << i)) c.surfaces[i] = ReadBoundSurface(base, device, i);
  }
  if (flags & 0x30) c.surfaces[4] = ReadBoundSurface(base, device, 4);
  if (color) {
    c.has_color = true;
    for (uint32_t i = 0; i < 4; ++i) c.color[i] = GuestFloat(base, color + 4 * i);
  }
}

// An inline draw's vertices and indices are in the command buffer, which the library reuses on its own
// schedule: they are copied into the command. Its stream is vertex fetch constant 95 (fetch slot 31, rule 3 of
// xdk-d3d-reference.md); DMA indices are at VGT_DMA_BASE. The draw's own packets write both, and the draw
// initiator (rule 11): they are read from its scan, the register file being on the renderer's thread (D73).
void CaptureInlineData(const StateTracker& tracker, const uint8_t* base, Command& c) {
  namespace rg = rex::graphics;
  auto copy = [&](uint32_t address, uint32_t size) {
    constexpr uint32_t kMaxSize = 1u << 20;
    if (!size || size > kMaxSize || c.copy_count >= c.copies.size()) return;
    const uint8_t* source = base + (0xA0000000u | (address & 0x1FFFFFFFu));
    c.copies[c.copy_count++] = {address, uint32_t(c.data.size()), size};
    c.data.insert(c.data.end(), source, source + size);
  };
  uint32_t fetch0, fetch1;
  if (tracker.LastWrite(rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 31 * 6 + 4, fetch0) &&
      tracker.LastWrite(rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 31 * 6 + 5, fetch1) &&
      (fetch0 & 3) == uint32_t(rg::xenos::FetchConstantType::kVertex)) {
    copy(fetch0 & ~3u, ((fetch1 >> 2) & 0xFFFFFF) * 4);
  }
  uint32_t initiator, dma_base;
  if (tracker.LastWrite(rg::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator) && ((initiator >> 6) & 3) == 0 &&
      tracker.LastWrite(rg::XE_GPU_REG_VGT_DMA_BASE, dma_base)) {  // DMA indices: 16 or 32 bits each
    copy(dma_base, (initiator >> 16) * ((initiator >> 11) & 1 ? 4 : 2));
  }
}

void CaptureDraw(const TrackedCall& tracked, StateTracker& tracker, const uint8_t* base) {
  Capture capture(Command::Type::kDraw);
  Command& c = capture.command;
  c.call = tracked.call;
  tracker.TakeRecord(c.registers);
  for (uint32_t i = 0; i < 5; ++i) c.surfaces[i] = ReadBoundSurface(base, tracked.device, i);
  c.programs[0] = tracker.vertex_program_ptr();
  c.programs[1] = tracker.pixel_program_ptr();
  if ((Info(tracked.call).flags & kInlineEnd) || tracked.call == Call::kDrawVerticesUP) {
    CaptureInlineData(tracker, base, c);
  }
}

void CaptureResolve(const uint8_t* base, const Arguments& a, const PacketScan& scan) {
  uint32_t device = a.r[0], flags = a.r[1], source_rect = a.r[2], texture = a.r[3], dest_point = a.r[4];
  Capture capture(Command::Type::kResolve);
  Command& c = capture.command;
  c.r = a.r;
  c.f1 = a.f1;
  c.surfaces[0] = ReadBoundSurface(base, device, std::min(flags & 7u, 4u));
  if (texture) {
    // The destination texture's fetch constant (ObjectLayout::texture_fetch).
    c.has_texture = true;
    uint32_t fetch = texture + g_profile->objects.texture_fetch;
    for (uint32_t i = 0; i < 6; ++i) c.fetch[i] = Load32(base, fetch + 4 * i);
  }
  if (source_rect) {
    c.has_source_rect = true;
    for (uint32_t i = 0; i < 4; ++i) c.source_rect[i] = int32_t(Load32(base, source_rect + 4 * i));
  }
  if (dest_point) {
    c.has_dest_point = true;
    c.dest_point = {int32_t(Load32(base, dest_point)), int32_t(Load32(base, dest_point + 4))};
  }
  // The RB_COPY_DEST_INFO the call wrote: its last non-zero value (the library resets the copy registers
  // afterwards); the stencil a depth clear writes, from RB_DEPTH_CLEAR (D80).
  for (const auto& [reg, value] : scan.writes) {
    if (reg == kRbCopyDestInfo && value) c.dest_info = value;
    if (reg == kRbDepthClear) c.clear_stencil = value & 0xFF;
  }
  // The clears after the copy (D80): the color (a D3DVECTOR4 at r10) and the depth surface.
  if ((flags & 0x100) && a.r[7]) {
    c.has_color = true;
    for (uint32_t i = 0; i < 4; ++i) c.color[i] = GuestFloat(base, a.r[7] + 4 * i);
  }
  if (flags & 0x200) c.surfaces[4] = ReadBoundSurface(base, device, 4);
  c.scene_caller = a.lr == g_profile->scene_resolve_caller &&
                   (!g_profile->scene_resolve_outer_caller || a.outer_lr == g_profile->scene_resolve_outer_caller);
}

// BeginTiling(dev, flags, count, tileRects*, clearColor*, Z (f1), stencil): the size of the image the tiles
// cover together.
void CaptureBeginTiling(const uint8_t* base, const Arguments& a) {
  constexpr uint32_t kMaxTiles = 32;  // EDRAM holds 2048 of its own tiles; games use two or three
  constexpr int32_t kMaxSize = 8192;  // the largest guest surface
  uint32_t count = a.r[2], rects = a.r[3];
  Capture capture(Command::Type::kBeginTiling);
  Command& c = capture.command;
  c.count = count;
  for (uint32_t i = 0; rects && i < std::min(count, kMaxTiles); ++i) {
    int32_t x2 = int32_t(Load32(base, rects + 16 * i + 8)), y2 = int32_t(Load32(base, rects + 16 * i + 12));
    c.width = std::max(c.width, uint32_t(std::clamp(x2, 0, kMaxSize)));
    c.height = std::max(c.height, uint32_t(std::clamp(y2, 0, kMaxSize)));
  }
}

// Query Issue(query, flags): the address its EVENT_WRITE_ZPD points at (see ExecuteQuery).
void CaptureQuery(const Arguments& a, const PacketScan& scan) {
  constexpr uint32_t kRbSampleCountAddr = 0x2325;
  uint32_t address = 0;
  for (const auto* writes : {&scan.writes, &scan.predicated_writes}) {
    for (const auto& [reg, value] : *writes) {
      if (reg == kRbSampleCountAddr) address = value;
    }
  }
  if (!address) return;
  Capture capture(Command::Type::kQuery);
  capture.command.r = a.r;
  capture.command.address = address;
}

// Present: the front buffer's fetch constant. The tracker's record goes too, so that it stays short in
// frames without draws.
void CapturePresent(StateTracker& tracker, const uint8_t* base) {
  Capture capture(Command::Type::kPresent);
  Command& c = capture.command;
  // The swap that shows this frame (D86): the game's Present has called VdSwap by now; one that calls it later
  // gets the next number all the same.
  if (g_present_with_swaps) {
    uint64_t issued = 0, executed = 0;
    g_graphics->GetSwapCounts(issued, executed);
    c.swap_tag = std::max(issued, g_last_swap_tag + 1);
    g_last_swap_tag = c.swap_tag;
  }
  tracker.TakeRecord(c.registers);
  if (g_front_buffer) {
    c.has_texture = true;
    uint32_t fetch = g_front_buffer + g_profile->objects.texture_fetch;
    for (uint32_t i = 0; i < 6; ++i) c.fetch[i] = Load32(base, fetch + 4 * i);
  }
}

}  // namespace

bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window,
                const std::filesystem::path& cache_root) {
  g_profile = &profile;
  g_cache_root = cache_root;
  if (!REXCVAR_GET(native_x_renderer)) {
    return false;
  }
  // Before taking the window's swap chain: if the profile is incomplete, the Xenos path draws.
  if (!DeviceLayoutComplete(profile.device, "renderer")) {
    return false;
  }
  const ObjectLayout& o = profile.objects;
  if (o.texture_fetch == kUnknown || o.surface_info == kUnknown || o.surface_color_depth_info == kUnknown ||
      o.surface_size == kUnknown || !o.surface_width_minus_one.bits || !o.surface_height_minus_one.bits) {
    REXLOG_WARN("[native_x_renderer] renderer: the profile's object layout lacks the texture or surface fields; off");
    return false;
  }
  void* native_window = window ? window->GetNativeWindowHandle() : nullptr;
  if (!graphics || !native_window) {
    REXLOG_ERROR("[native_x_renderer] renderer: needs the graphics system and the game's window; off");
    return false;
  }

  // The window can hold one swap chain: take it from the SDK's presenter. If NativeXRenderer cannot start, the
  // presenter gets it back and the Xenos path draws (D56: NativeXRenderer can be a game's default).
  window->SetPresenter(nullptr);
  auto device = std::make_unique<Device>();
  auto fall_back = [&](const char* reason) {
    g_blit = Blitter();
    device.reset();
    window->SetPresenter(graphics->presenter());
    REXLOG_ERROR("[native_x_renderer] renderer: {}; the Xenos path draws", reason);
    return false;
  };
  if (!device->Create(native_window) || !g_blit.Initialize(device->device())) {
    return fall_back("no host device");
  }
  if (!graphics->SetHostRendering(false)) {
    return fall_back("the graphics system cannot turn host rendering off (SDK patch missing?)");
  }
  // The SDK's backend draws nothing now. Its shader storage holds the Xenos path's pipelines, which the
  // runtime creates before the game starts: GRAW waited more than 14 s for 400 of them (D60).
  if (rex::cvar::Query<bool>("store_shaders") && rex::cvar::SetFlagByName("store_shaders", "false")) {
    REXLOG_INFO("[native_x_renderer] store_shaders set to false: the SDK's shader storage is for the Xenos path");
  }
  // The render scale (D37): the largest at which the game's image fits the output, which is the swap chain
  // (the monitor in fullscreen, the window's client area otherwise), at least 1, at most what the largest
  // texture allows; or the native_x_renderer_resolution_scale override.
  // Filling the output (D43): when the game widens its cameras, the scale is per axis, so that the image
  // has the output's aspect ratio; the vertical scale is the one computed or forced, and the horizontal one
  // follows from the output's aspect ratio. native_x_renderer_aspect (D104) asks for another ratio than the
  // output's: the image has that one, as large as fits the output, and the presentation centers it with bars.
  {
    const auto& output = device->swap_chain()->GetDesc();
    uint32_t guest_width = std::max(profile.output_width, 1u), guest_height = std::max(profile.output_height, 1u);
    double guest_aspect = double(guest_width) / guest_height;
    double output_aspect = output.Height ? double(output.Width) / output.Height : guest_aspect;
    double asked_aspect = REXCVAR_GET(native_x_renderer_aspect);
    double image_aspect = asked_aspect > 0.0 ? asked_aspect : output_aspect;
    bool fill = profile.widens_cameras && REXCVAR_GET(native_x_renderer_fill_screen) &&
                std::abs(image_aspect / guest_aspect - 1.0) > 0.01;
    double fit = fill ? std::min(double(output.Height) / guest_height,
                                 double(output.Width) / (guest_width * image_aspect / guest_aspect))
                      : std::min(double(output.Width) / guest_width, double(output.Height) / guest_height);
    double forced = REXCVAR_GET(native_x_renderer_resolution_scale);
    uint32_t max_dimension = device->device()->GetAdapterInfo().Texture.MaxTexture2DDimension;
    double factor = fill ? image_aspect / guest_aspect : 1.0;
    double max_scale =
        max_dimension ? double(max_dimension) / std::max(guest_width * std::max(factor, 1.0), double(guest_height))
                      : 1.0;
    double highest = std::max(max_scale, 1.0);
    double scale = std::clamp(forced > 0.0 ? forced : fit, std::min(1.0 / std::min(factor, 1.0), highest), highest);
    // A taller output keeps the horizontal scale at least 1 (the vertical one grows instead).
    g_scale.y = float(scale);
    g_scale.x = float(scale * factor);
    g_aspect_factor = double(g_scale.x) / double(g_scale.y);
    REXLOG_INFO("[native_x_renderer] render scale {:.4f} x {:.4f} ({}{}): the game's {}x{} rendered at {}x{}, for a {}x{} "
                "output",
                g_scale.x, g_scale.y, forced > 0.0 ? "native_x_renderer_resolution_scale" : "from the output",
                fill ? fmt::format(", filling it: the game's cameras widened by {:.4f}", g_aspect_factor) : "",
                guest_width, guest_height, g_scale.HostWidth(guest_width), g_scale.HostHeight(guest_height),
                output.Width, output.Height);
  }
  g_targets.Initialize(device->device(), g_scale);
  g_resolved.Initialize(device->device(), g_scale);
  // The shadow maps' scale (D92): at least native_x_renderer_shadow_scale on both axes, when that is above the render
  // scale; at most what the largest texture allows for a 2048x2048 map.
  {
    double asked = REXCVAR_GET(native_x_renderer_shadow_scale);
    uint32_t max_dimension = device->device()->GetAdapterInfo().Texture.MaxTexture2DDimension;
    double highest = max_dimension ? double(max_dimension) / 2048.0 : 1.0;
    double shadow = std::min(asked, highest);
    g_shadow_scaled = shadow > double(std::max(g_scale.x, g_scale.y));
    if (g_shadow_scaled) {
      g_shadow_scale.x = g_shadow_scale.y = float(shadow);
      g_targets.SetShadowScale(g_shadow_scale);
      g_resolved.SetShadowScale(g_shadow_scale);
      REXLOG_INFO("[native_x_renderer] shadow maps at scale {:.4f} (native_x_renderer_shadow_scale; the render scale is "
                  "{:.4f} x {:.4f}): a 1024x1024 map is {}x{} on the host",
                  shadow, g_scale.x, g_scale.y, g_shadow_scale.HostWidth(1024), g_shadow_scale.HostHeight(1024));
    }
  }
  // Occlusion queries measured here and answered to the command processor (D32); without them it
  // reports its fake sample count. The SDK's occlusion_query_enable (on by default) decides, as on the Xenos
  // path: a game that turns it off keeps the fake count (D67). Unregistered (no GPU plugin): on.
  bool measure_queries = rex::cvar::GetFlagByName("occlusion_query_enable").empty() ||
                         rex::cvar::Query<bool>("occlusion_query_enable");
  g_queries_on = measure_queries && g_queries.Initialize(device->device()) &&
                 graphics->SetOcclusionQueryResolver(&OcclusionQueries::Resolve, &g_queries);
  // Samples are counted at the render scale and reported at the game's (D32, D37).
  g_queries.SetSampleScale(double(g_scale.x) * double(g_scale.y));
  if (!measure_queries) {
    REXLOG_INFO("[native_x_renderer] occlusion queries not measured (occlusion_query_enable = false): the command "
                "processor's fake sample count, as on the Xenos path");
  } else if (!g_queries_on) {
    REXLOG_WARN("[native_x_renderer] occlusion queries not measured (SDK patch 0003 missing, or no device support)");
  }
  // The guest's display gamma ramp (D36); without it the front buffer is presented as it is.
  g_gamma.Initialize(device->device(), graphics);
  // The SDK's dialogs (D75): without them a game that asks for text (GRAW's new game) waits on a black screen.
  g_window = window;
  if (!g_overlay.Initialize(device->device(), device->swap_chain()->GetDesc().ColorBufferFormat)) {
    REXLOG_WARN("[native_x_renderer] the SDK's dialogs cannot be drawn (no overlay pipeline)");
  }
  // Antialiasing (D41), where the profile names the finished scene's resolve.
  {
    std::string name = REXCVAR_GET(native_x_renderer_antialiasing);
    // DLSS on the main 3D view, before the game's post-processing, in place of the antialiasing of the
    // finished scene: DLAA at the render resolution (D47), or a DLSS mode from a lower one (D49). SMAA where
    // DLSS is not available.
    static constexpr struct {
      const char* name;
      Upscaler::Mode mode;
    } kDlssModes[] = {{"dlaa", Upscaler::Mode::kDlaa},
                      {"dlss_quality", Upscaler::Mode::kQuality},
                      {"dlss_balanced", Upscaler::Mode::kBalanced},
                      {"dlss_performance", Upscaler::Mode::kPerformance},
                      {"dlss_ultra_performance", Upscaler::Mode::kUltraPerformance}};
    for (const auto& entry : kDlssModes) {
      if (name != entry.name) continue;
      uint32_t guest_width = std::max(profile.output_width, 1u), guest_height = std::max(profile.output_height, 1u);
      uint32_t width = g_scale.HostWidth(guest_width), height = g_scale.HostHeight(guest_height);
      g_dlss = g_upscaler.Initialize(device->device(), width, height, entry.mode);
      name = g_dlss ? "off" : "smaa";
      g_object_motion = g_dlss && REXCVAR_GET(native_x_renderer_object_motion);
      if (g_dlss && g_upscaler.upscales()) {
        g_reduced = true;
        g_reduced_scale.x = float(g_upscaler.input_width()) / float(guest_width);
        g_reduced_scale.y = float(g_upscaler.input_height()) / float(guest_height);
        g_targets.SetReducedScale(g_reduced_scale);
      }
      std::string what = !g_dlss ? std::string("DLSS not available, SMAA instead")
                         : g_reduced ? fmt::format("{} (experimental, D53): the 3D view drawn at {}x{} (scale "
                                                   "{:.3f}x{:.3f}), upscaled to {}x{} by DLSS",
                                                   Upscaler::ModeName(g_upscaler.mode()),
                                                   g_upscaler.input_width(), g_upscaler.input_height(),
                                                   g_reduced_scale.x, g_reduced_scale.y, width, height)
                                     : std::string("DLAA (NVIDIA DLSS at the render resolution)");
      REXLOG_INFO("[native_x_renderer] antialiasing: {}{}", what,
                  g_dlss ? (g_object_motion ? ", with the motion of objects that move on their own"
                                            : ", with the camera's motion only")
                         : "");
    }
    Antialiasing::Mode mode = Antialiasing::ParseMode(name);
    if (mode != Antialiasing::Mode::kOff && !profile.scene_resolve_caller) {
      REXLOG_INFO("[native_x_renderer] antialiasing: off, the game's profile names no scene resolve");
      mode = Antialiasing::Mode::kOff;
    }
    g_aa.Initialize(device->device(), mode);
  }
  g_timer.Initialize(device->device());
  g_timer.Begin(device->context());
  g_device = std::move(device);
  g_registers = std::make_unique<rex::graphics::RegisterFile>();
  // The renderer's thread (D71), only where the command processor can wait for it before it signals the game
  // (SetSignalGate(nullptr) answers whether it can, and sets nothing).
  bool threaded = REXCVAR_GET(native_x_renderer_thread);
  if (threaded && !graphics->SetSignalGate(nullptr, nullptr)) {
    REXLOG_WARN("[native_x_renderer] the command processor has no signal gate (SDK patch 0006 missing): the renderer "
                "runs on the game's render thread");
    threaded = false;
  }
  // Presenting with the guest's swaps (D86): the renderer's thread, and the command processor's swap counts
  // (SDK patch 0008).
  g_graphics = graphics;
  if (REXCVAR_GET(native_x_renderer_present_with_swaps)) {
    uint64_t issued = 0, executed = 0;
    if (!threaded) {
      REXLOG_WARN("[native_x_renderer] native_x_renderer_present_with_swaps needs the renderer's own thread: frames "
                  "are shown when the game calls Present");
    } else if (!graphics->GetSwapCounts(issued, executed)) {
      REXLOG_WARN("[native_x_renderer] the command processor does not count the guest's swaps (SDK patch 0008 "
                  "missing): frames are shown when the game calls Present");
    } else {
      g_present_with_swaps = true;
      g_last_swap_tag = issued;
      g_swaps_executed.store(executed, std::memory_order_relaxed);
    }
  }
  CommandQueue::Poller poller = nullptr;
  if (g_present_with_swaps) poller = [] { PresentPending(); };
  g_queue.Start(threaded, &Execute, poller);
  if (threaded) {
    graphics->SetSignalGate([](void*) { g_queue.WaitIdle(); }, nullptr);
    std::atexit([] { g_queue.Stop(); });
  }
  if (g_present_with_swaps) {
    graphics->SetSwapObserver(
        [](void*, uint64_t executed) {
          g_swaps_executed.store(executed, std::memory_order_release);
          uint64_t pending = g_pending_swap.load(std::memory_order_relaxed);
          if (pending && executed >= pending) g_queue.Wake();
        },
        nullptr);
  }
  REXLOG_INFO("[native_x_renderer] renderer on: the SDK runs the command buffer for synchronization only; the "
              "renderer runs {}; frames are shown {}",
              threaded ? "on a thread of its own (D71)" : "on the game's render thread",
              g_present_with_swaps ? "when the command processor executes their swaps (D86)"
                                   : "when the game calls Present");
  return true;
}

void OnEnter(Call call, PPCContext& ctx, uint8_t* base, bool nested) {
  if (call == Call::kClearF) {
    // Captured right away (ClearF is also called from inside Clear): the bound targets are the ones to clear.
    CaptureClear(ctx, base);
    return;
  }
  if (nested) return;
  // The arguments of the calls the renderer acts on; the setters' are not needed (D72).
  switch (call) {
    case Call::kResolve:
    case Call::kPresent:
    case Call::kBeginTiling:
    case Call::kEndTiling:
    case Call::kQueryIssue:
      break;
    default:
      if (!(Info(call).flags & kDraw)) return;
      break;
  }
  g_call.call = call;
  g_call.r = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  g_call.f1 = ctx.f1.f64;
  g_call.lr = uint32_t(ctx.lr);
  g_call.outer_lr = call == Call::kResolve && g_profile->scene_resolve_outer_caller
                        ? OuterReturnAddress(base, ctx.r1.u32)
                        : 0;
  if (call == Call::kPresent) g_front_buffer = ctx.r4.u32;
}

void OnLeave(const TrackedCall& tracked, StateTracker& tracker, PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  if (tracked.call != g_call.call) return;
  if (tracked.draw) {
    CaptureDraw(tracked, tracker, base);
    return;
  }
  switch (tracked.call) {
    case Call::kBeginTiling:
      CaptureBeginTiling(base, g_call);
      break;
    case Call::kEndTiling: {
      Capture capture(Command::Type::kEndTiling);
      capture.command.on = g_call.r[3] != 0;
      break;
    }
    case Call::kResolve:
      CaptureResolve(base, g_call, *tracked.scan);
      break;
    case Call::kQueryIssue:
      if (g_queries_on) CaptureQuery(g_call, *tracked.scan);
      break;
    case Call::kPresent:
      CapturePresent(tracker, base);
      break;
    default:
      break;  // the call's state is in the tracker; nothing to do here
  }
}

void OnFrameEnd(uint32_t frame) {
  if (!g_device) return;
  Capture capture(Command::Type::kFrameEnd);
  capture.command.frame = frame;
  capture.command.captured = g_captured;
  g_captured = 0;
}

double AspectFactor() { return g_aspect_factor; }

void SetGameAspectBox(bool on) {
  Capture capture(Command::Type::kAspectBox);
  capture.command.on = on;
}

void BeginView(const float* scene_view_projection) {
  Capture capture(Command::Type::kBeginView);
  capture.command.has_matrix = scene_view_projection != nullptr;
  if (scene_view_projection) std::memcpy(capture.command.matrix.data(), scene_view_projection, 16 * sizeof(float));
}

void EndViewGeometry() { Capture capture(Command::Type::kEndViewGeometry); }

namespace {

void ExecuteFrameEnd(const Command& c) {
  uint32_t frame = c.frame;
  ++g_frames_presented;
  g_upload_ahead = REXCVAR_GET(native_x_renderer_upload_ahead);
  sdk::NextFrame();
  // Render targets and resolved textures unused for a minute are freed (D52): 3600 frames at 60 FPS, and as many
  // more as the frame rate since the last log is above that (D74; at 120 FPS 3600 frames were half a minute). The
  // resolved textures that still hold their resolve, only above native_x_renderer_resolved_memory_mb (D96).
  {
    uint64_t unused_frames = 3600;
    if (g_pacing.frames && g_pacing.interval_ms > 0) {
      unused_frames = std::max<uint64_t>(unused_frames, uint64_t(60000.0 * g_pacing.frames / g_pacing.interval_ms));
    }
    uint64_t keep_bytes = uint64_t(std::max(0, REXCVAR_GET(native_x_renderer_resolved_memory_mb))) << 20;
    Freed targets = g_targets.EndFrame(unused_frames), resolved = g_resolved.EndFrame(unused_frames, keep_bytes);
    if (targets.count || resolved.count) {
      if (g_draw) g_draw->TargetsChanged();
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: freed after a minute unused: {} render targets ({} MB), {} "
                  "resolved textures ({} MB); {} and {} left",
                  frame, targets.count, targets.bytes >> 20, resolved.count, resolved.bytes >> 20, g_targets.count(),
                  g_resolved.count());
    }
  }
  // The frame was submitted at Present: collect the occlusion query results that arrived.
  if (g_queries_on) g_queries.Poll();
  DrawPath::Stats draws = g_draw ? g_draw->TakeStats() : DrawPath::Stats{};
  g_creation.analyses.Merge(draws.analyses);
  g_creation.translations.Merge(draws.translations);
  g_creation.pipelines.Merge(draws.pipelines);
  g_creation.translating_draws += draws.translating;
  g_creation.compiling_draws += draws.compiling;
  g_creation.waited_draws += draws.waited;
  g_creation.waited_ms += draws.waited_ms;
  g_creation.max_waited_ms = std::max(g_creation.max_waited_ms, draws.max_waited_ms);
  ++g_uploads.frames;
  g_uploads.copies += draws.copies;
  g_uploads.max_copies = std::max<uint64_t>(g_uploads.max_copies, draws.copies);
  g_uploads.uploaded_bytes += draws.uploaded_bytes;
  g_uploads.unchanged_bytes += draws.unchanged_bytes;
  g_uploads.unwatched_hashed_bytes += draws.unwatched_hashed_bytes;
  g_uploads.unwatched_pages = draws.unwatched_pages;
  g_uploads.unwatched_copy_bytes = draws.unwatched_copy_bytes;
  g_uploads.ahead_batches += draws.ahead_batches;
  g_uploads.ahead_draws += draws.ahead_draws;
  g_motion.matched += draws.motion_matched;
  g_motion.moved += draws.motion_moved;
  g_motion.drawn += draws.motion_drawn;
  g_motion.max_drawn = std::max<uint64_t>(g_motion.max_drawn, draws.motion_drawn);
  if (g_frames_presented == 1 || g_frames_presented % 600 == 0) {
    REXLOG_INFO("[native_x_renderer] renderer: frame {}: {} clears, {} resolves ({} skipped); {} render targets, {} "
                "resolve targets ({} invalidated by guest memory changes, {} kept with their memory unchanged; {} fetches in "
                "600 frames read guest memory a resolve had written)",
                frame, g_stats.clears, g_stats.resolves, g_stats.skipped_resolves, g_targets.count(),
                g_resolved.count(), g_resolved.TakeInvalidated(), g_resolved.TakeKept(), g_resolved.TakeGoneReads());
    FrameTimer::Result gpu = g_timer.Take();
    REXLOG_INFO("[native_x_renderer] renderer: frame {}: {} draw calls, {} drawn, {} without targets, {} failed, {} waiting for "
                "their pipeline; "
                "{} KB uploaded in {} copies; textures: {} resolved, {} guest, {} placeholders (and {} for the "
                "other dimension); {} texture and sampler bindings set, shader resources committed in {} draws; {} "
                "shaders, {} pipelines; GPU {:.2f} ms average, {:.2f} ms max over {} frames ({} more than 32 frames "
                "late)",
                frame, draws.draws, draws.drawn, draws.no_targets, draws.failed, draws.compiling,
                draws.uploaded_bytes >> 10,
                draws.copies, draws.textures_resolved, draws.textures_guest, draws.textures_placeholder,
                draws.textures_other_dimension, draws.bindings_set, draws.committed,
                g_draw ? g_draw->shader_count() : 0, g_draw ? g_draw->pipeline_count() : 0, gpu.average_ms,
                gpu.max_ms, gpu.frames, gpu.late);
    if (g_aa.mode() != Antialiasing::Mode::kOff) {
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: antialiasing ({}) applied {} times in {} frames", frame,
                  Antialiasing::ModeName(g_aa.mode()), g_aa.TakeApplied(), g_uploads.frames);
    }
    if (g_dlss) {
      uint32_t applied = g_upscaler.TakeApplied();
      double n = std::max(double(applied), 1.0);
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: {} applied {} times in {} frames; object motion (D48), "
                  "per application: {:.0f} draws matched with the last frame's, {:.0f} moved, {:.0f} motion draws "
                  "({} max)",
                  frame, Upscaler::ModeName(g_upscaler.mode()), applied, g_uploads.frames, g_motion.matched / n,
                  g_motion.moved / n, g_motion.drawn / n, g_motion.max_drawn);
      g_motion = {};
    }
    if (g_uploads.frames) {
      double n = double(g_uploads.frames);
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: per frame, {:.1f} buffer copies average, {} max; guest memory "
                  "{:.0f} KB copied, {:.0f} KB requested again but unchanged (not copied); {} pages rewritten every "
                  "frame left unwatched (D97), {:.0f} KB of them compared per frame (their copy on the CPU {} MB, "
                  "D110); uploads ahead (D98): {:.1f} batches per frame, {:.1f} draws each",
                  frame, g_uploads.copies / n, g_uploads.max_copies, g_uploads.uploaded_bytes / n / 1024,
                  g_uploads.unchanged_bytes / n / 1024, g_uploads.unwatched_pages,
                  g_uploads.unwatched_hashed_bytes / n / 1024, g_uploads.unwatched_copy_bytes >> 20,
                  g_uploads.ahead_batches / n,
                  g_uploads.ahead_batches ? double(g_uploads.ahead_draws) / g_uploads.ahead_batches : 0.0);
      g_uploads = {};
    }
    if (g_draw) {
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: since the start, {} guest texture loads ({} MB), {} skipped "
                  "(data unchanged), {} textures not created (unsupported format); {} guest textures on the host "
                  "({} MB)",
                  frame, g_draw->texture_loads(), g_draw->texture_loaded_bytes() >> 20, g_draw->texture_loads_unchanged(),
                  g_draw->textures_unsupported(), g_draw->texture_count(), g_draw->texture_host_bytes() >> 20);
    }
    // Device memory (D55): what DiligentCore's memory manager has in use and in its pages, next to what
    // NativeXRenderer knows it holds. Used below allocated is memory kept in partly empty pages; used above
    // the parts is memory held by something else (released resources still referenced, among others).
    if (RefCntAutoPtr<IRenderDeviceVk> vulkan{g_device->device(), IID_RenderDeviceVk}) {
      Uint64 used = 0, allocated = 0, host_used = 0, host_allocated = 0;
      Uint32 pages = 0;
      vulkan->GetMemoryUsage(&used, &allocated, &host_used, &host_allocated, &pages);
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: device memory {} MB used in {} MB of pages ({} pages), "
                  "host-visible {} MB in {} MB; of it: guest textures {} MB, render targets {} MB ({}), resolved "
                  "textures {} MB ({})",
                  frame, used >> 20, allocated >> 20, pages, host_used >> 20, host_allocated >> 20,
                  g_draw ? g_draw->texture_host_bytes() >> 20 : 0, g_targets.bytes() >> 20, g_targets.count(),
                  g_resolved.bytes() >> 20, g_resolved.count());
    }
    if (g_pacing.frames) {
      double n = g_pacing.frames;
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: frame interval {:.2f} ms average, {:.2f} ms max; renderer "
                  "CPU {:.2f} ms per frame; waiting for the GPU {:.2f} ms per frame, {:.2f} ms max",
                  frame, g_pacing.interval_ms / n, g_pacing.max_interval_ms, g_pacing.renderer_ms / n,
                  g_pacing.wait_ms / n, g_pacing.max_wait_ms);
      if (g_present_with_swaps) {
        REXLOG_INFO("[native_x_renderer] renderer: frame {}: shown with the guest's swaps (D86): {} at their swap, {} "
                    "before it (two newer frames waiting), {} after waiting {} ms for it; {} skipped (a newer one's "
                    "swap had executed too)",
                    frame, g_pacing.at_swap, g_pacing.before_swap, g_pacing.timed_out, kSwapTimeout.count(),
                    g_pacing.skipped);
      }
      if (g_queue.threaded()) {
        CommandQueue::Waits producer = g_queue.TakeProducerWaits(), gate = g_queue.TakeGateWaits();
        REXLOG_INFO("[native_x_renderer] renderer: frame {}: on its own thread (D71): {} commands in the last frame; "
                    "per frame, the game's thread waited {:.2f} ms for room in the queue ({:.1f} times), the command "
                    "processor {:.2f} ms at its signals ({:.1f} times); {} inline draws' data restored",
                    frame, c.captured, producer.ms / n, producer.count / n, gate.ms / n, gate.count / n,
                    draws.inline_restored);
      }
    }
    REXLOG_INFO("[native_x_renderer] renderer: frame {}: created on the render thread: {} shader analyses ({:.1f} ms, "
                "max {:.1f}), {} pipelines ({:.1f} ms, max {:.1f}); on worker threads: {} translations ({:.1f} ms, "
                "max {:.1f}); draws skipped: {} while a shader was translated, {} while their pipeline compiled; "
                "offscreen draws that waited for theirs (D101): {} ({:.1f} ms, max {:.1f})",
                frame, g_creation.analyses.count, g_creation.analyses.ms, g_creation.analyses.max_ms,
                g_creation.pipelines.count, g_creation.pipelines.ms, g_creation.pipelines.max_ms,
                g_creation.translations.count, g_creation.translations.ms, g_creation.translations.max_ms,
                g_creation.translating_draws, g_creation.compiling_draws, g_creation.waited_draws,
                g_creation.waited_ms, g_creation.max_waited_ms);
    g_creation = {};
    if (g_queries_on) {
      OcclusionQueries::Stats q = g_queries.TakeStats();
      // The first few addresses (GRAW has hundreds).
      std::string max;
      size_t listed = 0;
      for (const auto& [address, samples] : q.max_samples) {
        if (listed++ == 8) break;
        max += fmt::format(" {:#x}: {}", address, samples);
      }
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: occlusion queries: {} begun ({} most in a frame, {} over the "
                  "frame's budget), {} ended, {} results arrived for {} addresses (most samples per query:{}{}); {} "
                  "answered to the command processor, {} before any result",
                  frame, q.begun, q.most_in_a_frame, q.over_budget, q.ended, q.arrived, q.max_samples.size(),
                  max.empty() ? " none" : max, q.max_samples.size() > 8 ? " ..." : "", q.answered, q.unanswered);
    }
    Device::FenceState fences = g_device->GetFenceState();
    REXLOG_INFO("[native_x_renderer] renderer: frame {}: frames presented {}, completed {}; DiligentCore queue fence "
                "next {}, completed {}",
                frame, fences.frames_presented, fences.frames_completed, fences.queue_next, fences.queue_completed);
    // DiligentCore ignores Vulkan errors in release builds: after VK_ERROR_DEVICE_LOST, waits return at once
    // (frames look complete) while the queue fence stops advancing (it only reads fence status).
    static bool reported_loss = false;
    if (!reported_loss && fences.queue_next - fences.queue_completed > 64) {
      reported_loss = true;
      REXLOG_ERROR("[native_x_renderer] DiligentCore's command queue is {} submissions behind while frames complete: "
                   "the Vulkan device is probably lost (DiligentCore does not report it in release builds)",
                   fences.queue_next - fences.queue_completed);
    }
    auto last_present = g_pacing.last_present;
    g_pacing = {};
    g_pacing.last_present = last_present;
  }
  if (g_draw) g_draw->EndFrame();
  g_stats = {};
}

void Execute(Command& c) {
  if (g_pending_count) PresentPending();
  // A command past the last uploads-ahead window (D98): its data is no longer known to be as the window found it.
  if (g_draw && g_queue.running_index() >= g_ahead_until) g_draw->EndAheadWindow();
  ApplyRecord(c.registers.data(), c.registers.size(), g_registers->values);
  switch (c.type) {
    case Command::Type::kDraw:
      ExecuteDraw(c);
      break;
    case Command::Type::kClear:
      ExecuteClear(c);
      break;
    case Command::Type::kResolve:
      ExecuteResolve(c);
      break;
    case Command::Type::kBeginTiling:
      ExecuteBeginTiling(c);
      break;
    case Command::Type::kEndTiling:
      ExecuteEndTiling(c);
      break;
    case Command::Type::kQuery:
      ExecuteQuery(c);
      break;
    case Command::Type::kPresent:
      ExecutePresent(c);
      break;
    case Command::Type::kFrameEnd:
      ExecuteFrameEnd(c);
      break;
    case Command::Type::kBeginView:
      ExecuteBeginView(c);
      break;
    case Command::Type::kEndViewGeometry:
      ExecuteEndViewGeometry();
      break;
    case Command::Type::kAspectBox:
      ExecuteAspectBox(c);
      break;
    case Command::Type::kNone:
      break;
  }
}

}  // namespace

}  // namespace native_x_renderer::renderer
