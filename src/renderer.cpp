// XenosNative - the renderer.
//
// With --xenos_native=true (read once, at startup), XenosNative draws the game instead of the SDK's
// Xenos emulation:
//   - the SDK's command processor keeps executing the game's command buffer for synchronization only
//     (xenos_native/sdk_patches/0002: fences, interrupts, swaps, queries with fake sample counts), and
//     draws nothing on the host;
//   - the SDK's presenter is detached from the game's window, and XenosNative presents there through
//     its own DiligentCore (Vulkan) swap chain. The SDK's ImGui overlays are not shown on this path yet.
//
// Every D3D call is handled on the render thread, after the original ran, in the game's order, from the
// call's arguments and the state tracker (state.h). Phase 2 builds this up step by step (docs/plan.md):
// render targets, clears, resolves and presenting here; draws in draw.cpp. Textures from guest memory are
// not loaded yet (placeholders, textures.h).

#include "renderer.h"

#include "blit.h"
#include "device.h"
#include "draw.h"
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

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <memory>

REXCVAR_DEFINE_BOOL(xenos_native, false, "XenosNative",
                    "Draw the game with XenosNative instead of the SDK's Xenos emulation (read at startup; "
                    "Phase 2, in development)");

namespace xenos_native::renderer {
namespace {

using namespace internal;
using namespace Diligent;

constexpr uint32_t kRbCopyDestInfo = 0x231B;

const Profile* g_profile = nullptr;
std::unique_ptr<Device> g_device;
RenderTargets g_targets;
ResolvedTextures g_resolved;
Blitter g_blit;
OcclusionQueries g_queries;  // the guest's occlusion queries (D32)
bool g_queries_on = false;
std::unique_ptr<DrawPath> g_draw;  // created on the first draw (it needs the tracker's register file)
bool g_draw_failed = false;
bool g_tiling = false;  // between BeginTiling and EndTiling
uint32_t g_frames_presented = 0;

// The arguments of the outermost call in progress (the renderer acts when it returns).
struct Arguments {
  Call call{};
  std::array<uint32_t, 8> r{};  // r3..r10
  double f1 = 0;
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
      desc.Name = "XenosNative frame time";
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

void Draw(const StateTracker& tracker, const uint8_t* base, uint32_t device) {
  if (!g_draw) {
    if (g_draw_failed) return;
    auto* kernel = rex::system::kernel_state();
    auto draw = std::make_unique<DrawPath>();
    if (!kernel || !kernel->memory() ||
        !draw->Initialize(*g_device, tracker.mutable_registers(), *kernel->memory(), g_resolved)) {
      REXLOG_ERROR("[xenos_native] could not set up the draw path; draws are skipped");
      g_draw_failed = true;
      return;
    }
    g_draw = std::move(draw);
  }
  std::array<HostTarget*, 5> targets;
  for (uint32_t i = 0; i < 5; ++i) targets[i] = g_targets.Get(BoundSurface(base, device, i));
  g_draw->Draw(tracker, base, targets, g_tiling);
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
      REXLOG_WARN("[xenos_native] a partial clear (flags {:#x}) was skipped: not supported yet", flags);
    }
    return;
  }
  auto* context = g_device->context();
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(flags & (1u << i))) continue;
    HostTarget* target = g_targets.Get(BoundSurface(base, device, i));
    if (!target || !color) continue;
    float c[4] = {GuestFloat(base, color), GuestFloat(base, color + 4), GuestFloat(base, color + 8),
                  GuestFloat(base, color + 12)};
    context->SetRenderTargets(1, &target->view, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearRenderTarget(target->view, c, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    if (g_draw) g_draw->TargetsChanged();
  }
  if (flags & 0x30) {
    HostTarget* target = g_targets.Get(BoundSurface(base, device, 4));
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
  HostTarget* source = g_targets.Get(surface);
  if (!source || !texture || level) {
    ++g_stats.skipped_resolves;
    return;
  }
  // The destination texture's fetch constant (+28): dword 1 base and format, dword 2 size, dword 5 the
  // dimension. The header holds the CPU address; draws sample the GPU address the device puts in the fetch
  // constant.
  uint32_t fetch1 = Load32(base, texture + 32), fetch2 = Load32(base, texture + 36);
  uint32_t dimension = (Load32(base, texture + 48) >> 9) & 3;
  if (dimension != uint32_t(rex::graphics::xenos::DataDimension::k2DOrStacked)) {
    // 3D and cube destinations: not yet (their size is encoded differently, and they would need host
    // textures of their dimension).
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("[xenos_native] resolve to a texture of dimension {}: not supported yet", dimension);
    }
    ++g_stats.skipped_resolves;
    return;
  }
  uint32_t address = GpuAddress(fetch1 & 0xFFFFF000u), format = fetch1 & 0x3F;
  uint32_t width = (fetch2 & 0x1FFF) + 1, height = ((fetch2 >> 13) & 0x1FFF) + 1, slices = (fetch2 >> 26) + 1;
  ResolvedTexture* target = g_resolved.GetOrCreate(address, format, width, height, slices);
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
  // Clip to both textures.
  from.width = std::min({from.width, source->width - std::min(source->width, uint32_t(from.x)),
                         target->width - std::min(target->width, uint32_t(to.x))});
  from.height = std::min({from.height, source->height - std::min(source->height, uint32_t(from.y)),
                          target->height - std::min(target->height, uint32_t(to.y))});
  to.width = from.width;
  to.height = from.height;

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
  int32_t exp_bias = int32_t(dest_info << 10) >> 26;  // bits 16-21, signed
  options.scale = std::ldexp(1.0f, exp_bias);

  Blitter::Source kind = source->depth ? (source->samples > 1 ? Blitter::Source::kDepthMsaa : Blitter::Source::kDepth)
                                       : (source->samples > 1 ? Blitter::Source::kColorMsaa : Blitter::Source::kColor);
  g_blit.Copy(g_device->context(), source->srv, kind, from, target_view, to, options);
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
    g_queries.Begin(g_device->context());
  } else if (flags & 1) {
    g_queries.End(g_device->context(), address);
  }
}

void PresentFrontBuffer(const uint8_t* base) {
  auto* context = g_device->context();
  auto* back_buffer = g_device->swap_chain()->GetCurrentBackBufferRTV();
  const float black[4] = {0, 0, 0, 1};
  context->SetRenderTargets(1, &back_buffer, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  if (g_draw) g_draw->TargetsChanged();
  context->ClearRenderTarget(back_buffer, black, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  if (!g_front_buffer) return;
  uint32_t fetch1 = Load32(base, g_front_buffer + 32);
  ResolvedTexture* front = g_resolved.FindNewest(GpuAddress(fetch1 & 0xFFFFF000u));
  if (!front) return;
  // Letterbox: keep the front buffer's aspect ratio in the window.
  const auto& desc = g_device->swap_chain()->GetDesc();
  float scale = std::min(float(desc.Width) / float(front->width), float(desc.Height) / float(front->height));
  uint32_t width = uint32_t(float(front->width) * scale), height = uint32_t(float(front->height) * scale);
  Blitter::Rect from{0, 0, front->width, front->height};
  Blitter::Rect to{int32_t((desc.Width - width) / 2), int32_t((desc.Height - height) / 2), width, height};
  // The front buffer is read through its fetch constant, swizzle included: a resolve with the red/blue
  // swap (RB_COPY_DEST_INFO) writes BGRA, and the front buffer's swizzle swaps it back.
  Blitter::Options options;
  options.swizzle = (Load32(base, g_front_buffer + 40) >> 1) & 0xFFF;
  g_blit.Copy(context, front->srv, Blitter::Source::kColorScaled, from, back_buffer, to, options);
}

}  // namespace

bool Initialize(const Profile& profile, rex::system::IGraphicsSystem* graphics, rex::ui::Window* window) {
  g_profile = &profile;
  if (!REXCVAR_GET(xenos_native)) {
    return false;
  }
  void* native_window = window ? window->GetNativeWindowHandle() : nullptr;
  if (!graphics || !native_window) {
    REXLOG_ERROR("[xenos_native] renderer: needs the graphics system and the game's window; off");
    return false;
  }

  // The window can hold one swap chain: take it from the SDK's presenter.
  window->SetPresenter(nullptr);
  auto device = std::make_unique<Device>();
  if (!device->Create(native_window) || !g_blit.Initialize(device->device())) {
    REXLOG_ERROR("[xenos_native] renderer: no host device; the Xenos path draws");
    return false;
  }
  if (!graphics->SetHostRendering(false)) {
    REXLOG_ERROR("[xenos_native] renderer: the graphics system cannot turn host rendering off "
                 "(SDK patch missing?)");
    return false;
  }
  g_targets.Initialize(device->device());
  g_resolved.Initialize(device->device());
  // Occlusion queries measured here and answered to the command processor (D32); without them it
  // reports its fake sample count.
  g_queries_on = g_queries.Initialize(device->device()) &&
                 graphics->SetOcclusionQueryResolver(&OcclusionQueries::Resolve, &g_queries);
  if (!g_queries_on) {
    REXLOG_WARN("[xenos_native] occlusion queries not measured (SDK patch 0003 missing, or no device support)");
  }
  g_timer.Initialize(device->device());
  g_timer.Begin(device->context());
  g_device = std::move(device);
  REXLOG_INFO("[xenos_native] renderer on: the SDK runs the command buffer for synchronization only");
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
  // The frame was submitted at Present: collect the occlusion query results that arrived.
  if (g_queries_on) g_queries.Poll();
  DrawPath::Stats draws = g_draw ? g_draw->TakeStats() : DrawPath::Stats{};
  g_creation.analyses.Merge(draws.analyses);
  g_creation.translations.Merge(draws.translations);
  g_creation.pipelines.Merge(draws.pipelines);
  g_creation.translating_draws += draws.translating;
  g_creation.compiling_draws += draws.compiling;
  if (g_frames_presented == 1 || g_frames_presented % 600 == 0) {
    REXLOG_INFO("[xenos_native] renderer: frame {}: {} clears, {} resolves ({} skipped); {} render targets, {} "
                "resolve targets",
                frame, g_stats.clears, g_stats.resolves, g_stats.skipped_resolves, g_targets.count(),
                g_resolved.count());
    FrameTimer::Result gpu = g_timer.Take();
    REXLOG_INFO("[xenos_native] renderer: frame {}: {} draw calls, {} drawn, {} without targets, {} failed, {} waiting for "
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
    if (g_draw) {
      REXLOG_INFO("[xenos_native] renderer: frame {}: since the start, {} guest texture loads ({} MB), {} skipped "
                  "(data unchanged), {} textures not created (unsupported format); {} guest textures on the host "
                  "({} MB)",
                  frame, g_draw->texture_loads(), g_draw->texture_loaded_bytes() >> 20, g_draw->texture_loads_unchanged(),
                  g_draw->textures_unsupported(), g_draw->texture_count(), g_draw->texture_host_bytes() >> 20);
    }
    if (g_pacing.frames) {
      double n = g_pacing.frames;
      REXLOG_INFO("[xenos_native] renderer: frame {}: frame interval {:.2f} ms average, {:.2f} ms max; renderer "
                  "CPU {:.2f} ms per frame; waiting for the GPU {:.2f} ms per frame, {:.2f} ms max",
                  frame, g_pacing.interval_ms / n, g_pacing.max_interval_ms, g_pacing.renderer_ms / n,
                  g_pacing.wait_ms / n, g_pacing.max_wait_ms);
    }
    REXLOG_INFO("[xenos_native] renderer: frame {}: created on the render thread: {} shader analyses ({:.1f} ms, "
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
      REXLOG_INFO("[xenos_native] renderer: frame {}: occlusion queries: {} begun, {} ended, {} results arrived "
                  "(most samples per query:{}); {} answered to the command processor, {} before any result",
                  frame, q.begun, q.ended, q.arrived, max.empty() ? " none" : max, q.answered, q.unanswered);
    }
    Device::FenceState fences = g_device->GetFenceState();
    REXLOG_INFO("[xenos_native] renderer: frame {}: frames presented {}, completed {}; DiligentCore queue fence "
                "next {}, completed {}",
                frame, fences.frames_presented, fences.frames_completed, fences.queue_next, fences.queue_completed);
    // DiligentCore ignores Vulkan errors in release builds: after VK_ERROR_DEVICE_LOST, waits return at once
    // (frames look complete) while the queue fence stops advancing (it only reads fence status).
    static bool reported_loss = false;
    if (!reported_loss && fences.queue_next - fences.queue_completed > 64) {
      reported_loss = true;
      REXLOG_ERROR("[xenos_native] DiligentCore's command queue is {} submissions behind while frames complete: "
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

}  // namespace xenos_native::renderer
