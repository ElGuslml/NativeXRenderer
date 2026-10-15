// NativeXRenderer - the renderer.
//
// With --native_x_renderer=true (read once, at startup), NativeXRenderer draws the game instead of the SDK's
// Xenos emulation:
//   - the SDK's command processor keeps executing the game's command buffer for synchronization only
//     (native_x_renderer/sdk_patches/0002: fences, interrupts, swaps, queries with fake sample counts), and
//     draws nothing on the host;
//   - the SDK's presenter is detached from the game's window, and NativeXRenderer presents there through
//     its own DiligentCore (Vulkan) swap chain. The SDK's ImGui overlays are not shown on this path yet.
//
// Every D3D call is handled on the render thread, after the original ran, in the game's order, from the
// call's arguments and the state tracker (state.h). Phase 2 builds this up step by step (docs/plan.md):
// render targets, clears, resolves and presenting here; draws in draw.cpp. Textures from guest memory are
// not loaded yet (placeholders, textures.h).

#include "renderer.h"

#include "aa.h"
#include "upscaler.h"
#include "blit.h"
#include "device.h"
#include "draw.h"
#include "gamma.h"
#include "queries.h"
#include "internal.h"
#include "state.h"
#include "targets.h"

#include <rex/cvar.h>
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
#include <cstring>
#include <memory>

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
REXCVAR_DEFINE_BOOL(native_x_renderer_fill_screen, true, "NativeXRenderer",
                    "Fill an output that is not 16:9, with a render scale per axis, when the game's hooks widen its "
                    "cameras (read at startup); false: the 16:9 image with bars");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_resolution_scale, 0.0, "NativeXRenderer",
                      "Render scale, host pixels per guest pixel (read at startup). 0: from the screen, the largest "
                      "at which the game's image fits the monitor or the window (at least 1)");

namespace native_x_renderer::renderer {
namespace {

using namespace internal;
using namespace Diligent;

constexpr uint32_t kRbCopyDestInfo = 0x231B;

const Profile* g_profile = nullptr;
std::unique_ptr<Device> g_device;
RenderTargets g_targets;
ResolvedTextures g_resolved;
Blitter g_blit;
DisplayGamma g_gamma;  // the guest's display gamma ramp, applied when presenting (D36)
Antialiasing g_aa;     // on the finished scene (D41)
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
uint32_t g_frames_presented = 0;

// The arguments of the outermost call in progress (the renderer acts when it returns).
struct Arguments {
  Call call{};
  std::array<uint32_t, 8> r{};  // r3..r10
  double f1 = 0;
  uint32_t lr = 0;  // the return address in the caller
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

  static double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
} g_pacing;

// Host objects created since the last log line: shader analyses and pipelines on the render thread,
// translations on worker threads (D34).
struct Creation {
  CreationTime analyses, translations, pipelines;
  uint64_t translating_draws = 0;  // skipped while a shader was translated (D34)
  uint64_t compiling_draws = 0;    // skipped while their pipeline compiled (D28)
} g_creation;

// Buffer copies (each ends the render pass) and guest memory uploads per frame, since the last log line.
struct Uploads {
  uint64_t frames = 0, copies = 0, max_copies = 0, uploaded_bytes = 0, unchanged_bytes = 0;
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

GuestSurface BoundSurface(const uint8_t* base, uint32_t device, uint32_t index) {
  const DeviceLayout& d = g_profile->device;
  if (index < 4) return ReadSurface(base, Load32(base, device + d.render_targets + 4 * index), false);
  return ReadSurface(base, Load32(base, device + d.depth_stencil), true);
}

float GuestFloat(const uint8_t* base, uint32_t address) { return LoadFloat(base, address); }

// <cache root>/native_x_renderer/<title id>: the stored shaders and pipelines of this game (D42).
std::filesystem::path StoragePath(uint32_t title_id) {
  if (g_cache_root.empty() || !REXCVAR_GET(native_x_renderer_pipeline_storage)) return {};
  return g_cache_root / "native_x_renderer" / fmt::format("{:08X}", title_id);
}

// The host target a draw, a clear or a resolve uses for a guest surface: in the main 3D view, while DLSS
// draws it at a lower scale, the reduced one for the targets of the game's output size (D49).
HostTarget* TargetFor(const GuestSurface& surface) {
  bool reduced = g_reduced && g_scene_view && surface && surface.width == g_profile->output_width &&
                 surface.height == g_profile->output_height;
  HostTarget* target = g_targets.Get(surface, reduced);
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
  }
  g_reduced_used.clear();
  if (g_draw) g_draw->TargetsChanged();
}

void Draw(const StateTracker& tracker, const uint8_t* base, uint32_t device) {
  if (!g_draw) {
    if (g_draw_failed) return;
    auto* kernel = rex::system::kernel_state();
    auto draw = std::make_unique<DrawPath>();
    if (!kernel || !kernel->memory() ||
        !draw->Initialize(*g_device, tracker.mutable_registers(), *kernel->memory(), g_resolved, g_scale,
                          StoragePath(kernel->title_id()))) {
      REXLOG_ERROR("[native_x_renderer] could not set up the draw path; draws are skipped");
      g_draw_failed = true;
      return;
    }
    g_draw = std::move(draw);
  }
  std::array<HostTarget*, 5> targets;
  for (uint32_t i = 0; i < 5; ++i) targets[i] = TargetFor(BoundSurface(base, device, i));
  g_draw->SetAspectBox(g_aspect_box);
  bool scene = g_scene_view && g_dlss;
  g_draw->SetJitter(scene, g_upscaler.jitter_x(), g_upscaler.jitter_y(), g_profile->output_width,
                    g_profile->output_height);
  g_draw->SetObjectMotion(g_object_motion);
  g_draw->Draw(tracker, base, targets, g_tiling);
  if (scene && targets[0] && targets[0]->guest_width == g_profile->output_width &&
      targets[0]->guest_height == g_profile->output_height) {
    g_scene_color = targets[0];
    g_scene_color_surface = BoundSurface(base, device, 0);
    if (targets[4]) g_scene_depth = targets[4];
  }
}

// ClearF(dev, flags, rect, color*, Z (f1), stencil): bits 0-3 clear RT0-3, 0x10 depth, 0x20 stencil.
void Clear(const uint8_t* base, const Arguments& a) {
  uint32_t device = a.r[0], flags = a.r[1], rect = a.r[2], color = a.r[3], stencil = a.r[5];
  if (rect) {
    // Partial clears (a rectangle): not yet. POTCAWE's menus and dock have none; logged once if a scene
    // does.
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("[native_x_renderer] a partial clear (flags {:#x}) was skipped: not supported yet", flags);
    }
    return;
  }
  auto* context = g_device->context();
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(flags & (1u << i))) continue;
    HostTarget* target = TargetFor(BoundSurface(base, device, i));
    if (!target || !color) continue;
    float c[4] = {GuestFloat(base, color), GuestFloat(base, color + 4), GuestFloat(base, color + 8),
                  GuestFloat(base, color + 12)};
    context->SetRenderTargets(1, &target->view, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearRenderTarget(target->view, c, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    if (g_draw) g_draw->TargetsChanged();
  }
  if (flags & 0x30) {
    HostTarget* target = TargetFor(BoundSurface(base, device, 4));
    if (target) {
      CLEAR_DEPTH_STENCIL_FLAGS clear_flags = CLEAR_DEPTH_FLAG_NONE;
      if (flags & 0x10) clear_flags |= CLEAR_DEPTH_FLAG;
      if (flags & 0x20) clear_flags |= CLEAR_STENCIL_FLAG;
      context->SetRenderTargets(0, nullptr, target->view, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      context->ClearDepthStencil(target->view, clear_flags, float(a.f1), uint8_t(stencil),
                                 RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      if (g_draw) g_draw->TargetsChanged();
    }
  }
  ++g_stats.clears;
}

// Resolve(dev, flags, srcRect*, destTexture, destPoint*, level, slice, clearColor*, Z (f1), ...).
// flags: 0-3 = RT0-3, 4 = depth; 0x10-0x80 = one sample (fragment 0-3) instead of the average.
void Resolve(const uint8_t* base, const Arguments& a, const PacketScan& scan) {
  uint32_t device = a.r[0], flags = a.r[1], source_rect = a.r[2], texture = a.r[3], dest_point = a.r[4];
  uint32_t level = a.r[5], slice = a.r[6];
  GuestSurface surface = BoundSurface(base, device, std::min(flags & 7u, 4u));
  HostTarget* source = TargetFor(surface);
  if (!source || !texture || level) {
    ++g_stats.skipped_resolves;
    return;
  }
  // The destination texture's fetch constant (+28): dword 1 base and format, dword 2 size, dword 5 the
  // dimension. The header holds the CPU address; draws sample the GPU address the device puts in the fetch
  // constant.
  uint32_t fetch1 = Load32(base, texture + 32), fetch2 = Load32(base, texture + 36);
  uint32_t dimension = (Load32(base, texture + 48) >> 9) & 3;
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
  ResolvedTexture* target = g_resolved.GetOrCreate(address, format, width, height, slices, cube);
  ITextureView* target_view = target ? target->SliceView(slice) : nullptr;
  if (!target_view) {
    ++g_stats.skipped_resolves;
    return;
  }

  Blitter::Rect from{0, 0, surface.width, surface.height};
  if (source_rect) {
    int32_t x1 = int32_t(Load32(base, source_rect)), y1 = int32_t(Load32(base, source_rect + 4));
    int32_t x2 = int32_t(Load32(base, source_rect + 8)), y2 = int32_t(Load32(base, source_rect + 12));
    from = {x1, y1, uint32_t(std::max(0, x2 - x1)), uint32_t(std::max(0, y2 - y1))};
  }
  Blitter::Rect to{0, 0, from.width, from.height};
  if (dest_point) {
    to.x = int32_t(Load32(base, dest_point));
    to.y = int32_t(Load32(base, dest_point + 4));
  }
  // Clip to both textures, in guest pixels.
  from.width = std::min({from.width, source->guest_width - std::min(source->guest_width, uint32_t(from.x)),
                         target->guest_width - std::min(target->guest_width, uint32_t(to.x))});
  from.height = std::min({from.height, source->guest_height - std::min(source->guest_height, uint32_t(from.y)),
                          target->guest_height - std::min(target->guest_height, uint32_t(to.y))});
  to.width = from.width;
  to.height = from.height;
  // Then on the host: the source at its scale (the render scale, D37; the reduced one in an upscaled view,
  // D49), the destination at the render scale. At one scale, with a fractional scale the two rectangles'
  // rounded sizes can differ by a pixel: the copy takes the smaller. From a reduced target the copy scales.
  auto host_rect = [](const RenderScale& scale, const Blitter::Rect& r) {
    int32_t x0 = scale.HostX(r.x), y0 = scale.HostY(r.y);
    return Blitter::Rect{x0, y0, uint32_t(scale.HostX(int64_t(r.x) + r.width) - x0),
                         uint32_t(scale.HostY(int64_t(r.y) + r.height) - y0)};
  };
  from = host_rect(source->scale, from);
  to = host_rect(g_scale, to);
  bool scaled = source->reduced;
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

  // What the copy itself does, from the RB_COPY_DEST_INFO the call wrote (its last non-zero value: the
  // library resets the copy registers afterwards).
  uint32_t dest_info = 0;
  for (const auto& [reg, value] : scan.writes) {
    if (reg == kRbCopyDestInfo && value) dest_info = value;
  }
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
  // The copy of the finished scene (the profile's scene_resolve_caller): antialiased in place (D41).
  if (g_aa.mode() != Antialiasing::Mode::kOff && g_call.lr == g_profile->scene_resolve_caller && slice == 0) {
    g_aa.Apply(g_device->context(), target->texture, target_view);
  }
  if (g_draw) g_draw->TargetsChanged();
  g_resolved.Touch(target);
  ++g_stats.resolves;
}

// Query Issue(query, flags): D3DISSUE_BEGIN (2) or D3DISSUE_END (1). An occlusion query's Issue points an
// EVENT_WRITE_ZPD at its sample counts (RB_SAMPLE_COUNT_ADDR: a normal packet with one tile, one
// bin-predicated packet per tile with several; the last is the last tile's block, which the end marker
// is in); other query types write none.
void IssueQuery(const Arguments& a, const PacketScan& scan) {
  constexpr uint32_t kRbSampleCountAddr = 0x2325;
  uint32_t address = 0;
  for (const auto* writes : {&scan.writes, &scan.predicated_writes}) {
    for (const auto& [reg, value] : *writes) {
      if (reg == kRbSampleCountAddr) address = value;
    }
  }
  if (!address) return;
  uint32_t flags = a.r[1];
  if (flags & 2) {
    // Samples are reported at the game's scale: from the scale of the targets the query's draws go to, the
    // reduced one in an upscaled view (D49).
    const RenderScale& scale = g_reduced && g_scene_view ? g_reduced_scale : g_scale;
    g_queries.SetSampleScale(double(scale.x) * double(scale.y));
    g_queries.Begin(g_device->context());
  } else if (flags & 1) {
    g_queries.End(g_device->context(), address);
  }
}

}  // namespace

double AspectFactor() { return g_aspect_factor; }

void SetGameAspectBox(bool on) { g_aspect_box = on && g_aspect_factor != 1.0; }

void BeginView(const float* scene_view_projection) {
  g_scene_view = scene_view_projection != nullptr;
  if (g_scene_view) std::memcpy(g_scene_view_projection.data(), scene_view_projection, 16 * sizeof(float));
  g_scene_color = g_scene_depth = nullptr;
}

void EndViewGeometry() {
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

namespace {

void PresentFrontBuffer(const uint8_t* base) {
  auto* context = g_device->context();
  auto* back_buffer = g_device->swap_chain()->GetCurrentBackBufferRTV();
  if (g_draw) g_draw->TargetsChanged();
  ResolvedTexture* front = nullptr;
  uint32_t fetch1 = 0;
  if (g_front_buffer) {
    fetch1 = Load32(base, g_front_buffer + 32);
    front = g_resolved.FindNewest(GpuAddress(fetch1 & 0xFFFFF000u));
    if (front) g_resolved.MarkUsed(front);
  }
  // The front buffer is read through its fetch constant, swizzle included: a resolve with the red/blue
  // swap (RB_COPY_DEST_INFO) writes BGRA, and the front buffer's swizzle swaps it back. Then the guest's
  // gamma ramp, at the front buffer's size (D36); the scaled copy below reads the result as it is.
  Blitter::Options options;
  ITextureView* source = nullptr;
  if (front) {
    options.swizzle = (Load32(base, g_front_buffer + 40) >> 1) & 0xFFF;
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

}  // namespace

bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window,
                const std::filesystem::path& cache_root) {
  g_profile = &profile;
  g_cache_root = cache_root;
  if (!REXCVAR_GET(native_x_renderer)) {
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
  // The render scale (D37): the largest at which the game's image fits the output, which is the swap chain
  // (the monitor in fullscreen, the window's client area otherwise), at least 1, at most what the largest
  // texture allows; or the native_x_renderer_resolution_scale override.
  // Filling the output (D43): when the game widens its cameras, the scale is per axis, so that the image
  // has the output's aspect ratio; the vertical scale is the one computed or forced, and the horizontal one
  // follows from the output's aspect ratio.
  {
    const auto& output = device->swap_chain()->GetDesc();
    uint32_t guest_width = std::max(profile.output_width, 1u), guest_height = std::max(profile.output_height, 1u);
    double guest_aspect = double(guest_width) / guest_height;
    double output_aspect = output.Height ? double(output.Width) / output.Height : guest_aspect;
    bool fill = profile.widens_cameras && REXCVAR_GET(native_x_renderer_fill_screen) &&
                std::abs(output_aspect / guest_aspect - 1.0) > 0.01;
    double fit = fill ? double(output.Height) / guest_height
                      : std::min(double(output.Width) / guest_width, double(output.Height) / guest_height);
    double forced = REXCVAR_GET(native_x_renderer_resolution_scale);
    uint32_t max_dimension = device->device()->GetAdapterInfo().Texture.MaxTexture2DDimension;
    double factor = fill ? output_aspect / guest_aspect : 1.0;
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
  // Occlusion queries measured here and answered to the command processor (D32); without them it
  // reports its fake sample count.
  g_queries_on = g_queries.Initialize(device->device()) &&
                 graphics->SetOcclusionQueryResolver(&OcclusionQueries::Resolve, &g_queries);
  // Samples are counted at the render scale and reported at the game's (D32, D37).
  g_queries.SetSampleScale(double(g_scale.x) * double(g_scale.y));
  if (!g_queries_on) {
    REXLOG_WARN("[native_x_renderer] occlusion queries not measured (SDK patch 0003 missing, or no device support)");
  }
  // The guest's display gamma ramp (D36); without it the front buffer is presented as it is.
  g_gamma.Initialize(device->device(), graphics);
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
  REXLOG_INFO("[native_x_renderer] renderer on: the SDK runs the command buffer for synchronization only");
  return true;
}

void OnEnter(Call call, PPCContext& ctx, uint8_t* base, bool nested) {
  RendererTime time;
  if (call == Call::kClearF) {
    // Done right away (ClearF is also called from inside Clear): the bound targets are the ones to clear.
    Arguments a;
    a.r = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
    a.f1 = ctx.f1.f64;
    Clear(base, a);
    return;
  }
  if (nested) return;
  g_call.call = call;
  g_call.r = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  g_call.f1 = ctx.f1.f64;
  g_call.lr = uint32_t(ctx.lr);
  if (call == Call::kPresent) g_front_buffer = ctx.r4.u32;
}

void OnLeave(const TrackedCall& tracked, const StateTracker& tracker, PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  if (tracked.call != g_call.call) return;
  RendererTime time;
  if (tracked.draw) {
    Draw(tracker, base, tracked.device);
    return;
  }
  switch (tracked.call) {
    case Call::kBeginTiling:
      g_tiling = true;
      break;
    case Call::kEndTiling:
      g_tiling = false;
      break;
    case Call::kResolve:
      Resolve(base, g_call, *tracked.scan);
      break;
    case Call::kQueryIssue:
      if (g_queries_on) IssueQuery(g_call, *tracked.scan);
      break;
    case Call::kPresent:
      g_aspect_box = false;
      PromoteReduced(nullptr);  // a view without EndViewGeometry (D49)
      g_scene_view = false;
      if (g_dlss) g_upscaler.NextFrame();
      if (g_device->UpdateSize()) {
        PresentFrontBuffer(base);
        g_timer.End(g_device->context());
        double wait_ms = g_device->Present();
        g_timer.Begin(g_device->context());
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
      break;
    default:
      break;
  }
}

void OnFrameEnd(uint32_t frame) {
  if (!g_device) return;
  ++g_frames_presented;
  // Render targets and resolved textures unused for a minute (at 60 FPS) are freed (D52).
  {
    constexpr uint64_t kUnusedFrames = 3600;
    Freed targets = g_targets.EndFrame(kUnusedFrames), resolved = g_resolved.EndFrame(kUnusedFrames);
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
  ++g_uploads.frames;
  g_uploads.copies += draws.copies;
  g_uploads.max_copies = std::max<uint64_t>(g_uploads.max_copies, draws.copies);
  g_uploads.uploaded_bytes += draws.uploaded_bytes;
  g_uploads.unchanged_bytes += draws.unchanged_bytes;
  g_motion.matched += draws.motion_matched;
  g_motion.moved += draws.motion_moved;
  g_motion.drawn += draws.motion_drawn;
  g_motion.max_drawn = std::max<uint64_t>(g_motion.max_drawn, draws.motion_drawn);
  if (g_frames_presented == 1 || g_frames_presented % 600 == 0) {
    REXLOG_INFO("[native_x_renderer] renderer: frame {}: {} clears, {} resolves ({} skipped); {} render targets, {} "
                "resolve targets",
                frame, g_stats.clears, g_stats.resolves, g_stats.skipped_resolves, g_targets.count(),
                g_resolved.count());
    FrameTimer::Result gpu = g_timer.Take();
    REXLOG_INFO("[native_x_renderer] renderer: frame {}: {} draw calls, {} drawn, {} without targets, {} failed, {} waiting for "
                "their pipeline; "
                "{} KB uploaded in {} copies; textures: {} resolved, {} guest, {} placeholders (and {} for the "
                "other dimension); {} shaders, {} pipelines; GPU {:.2f} ms average, {:.2f} ms max over {} frames "
                "({} more than 32 frames late)",
                frame, draws.draws, draws.drawn, draws.no_targets, draws.failed, draws.compiling,
                draws.uploaded_bytes >> 10,
                draws.copies, draws.textures_resolved, draws.textures_guest, draws.textures_placeholder,
                draws.textures_other_dimension,
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
                  "{:.0f} KB copied, {:.0f} KB requested again but unchanged (not copied)",
                  frame, g_uploads.copies / n, g_uploads.max_copies, g_uploads.uploaded_bytes / n / 1024,
                  g_uploads.unchanged_bytes / n / 1024);
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
    }
    REXLOG_INFO("[native_x_renderer] renderer: frame {}: created on the render thread: {} shader analyses ({:.1f} ms, "
                "max {:.1f}), {} pipelines ({:.1f} ms, max {:.1f}); on worker threads: {} translations ({:.1f} ms, "
                "max {:.1f}); draws skipped: {} while a shader was translated, {} while their pipeline compiled",
                frame, g_creation.analyses.count, g_creation.analyses.ms, g_creation.analyses.max_ms,
                g_creation.pipelines.count, g_creation.pipelines.ms, g_creation.pipelines.max_ms,
                g_creation.translations.count, g_creation.translations.ms, g_creation.translations.max_ms,
                g_creation.translating_draws, g_creation.compiling_draws);
    g_creation = {};
    if (g_queries_on) {
      OcclusionQueries::Stats q = g_queries.TakeStats();
      std::string max;
      for (const auto& [address, samples] : q.max_samples) max += fmt::format(" {:#x}: {}", address, samples);
      REXLOG_INFO("[native_x_renderer] renderer: frame {}: occlusion queries: {} begun, {} ended, {} results arrived "
                  "(most samples per query:{}); {} answered to the command processor, {} before any result",
                  frame, q.begun, q.ended, q.arrived, max.empty() ? " none" : max, q.answered, q.unanswered);
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

}  // namespace native_x_renderer::renderer
