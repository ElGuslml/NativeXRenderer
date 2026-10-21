// NativeXRenderer - the guest's occlusion queries, measured on the host (internal, D32).
//
// The XDK's Query Issue(BEGIN) and Issue(END) bracket the draws whose visible samples the game wants to
// know (POTCAWE's sun flare: the visible fraction of a quad drawn at the sun). Each Issue writes an
// EVENT_WRITE_ZPD pointing at the query's sample counts in memory (RB_SAMPLE_COUNT_ADDR); the GPU writes
// the counts there, and Query GetData reads them.
//
// Here:
//   - Issue(BEGIN) begins a DiligentCore occlusion query, and Issue(END) ends it and keys it by the end
//     event's sample count address. Both happen outside a render pass, as Vulkan requires of a query
//     that contains whole render passes.
//   - Once per frame, the queries whose results arrived give the latest sample count for their address.
//   - The SDK's command processor, which runs the packets for synchronization only, asks for the count
//     when it executes the end event (IGraphicsSystem::SetOcclusionQueryResolver, the SDK's recomp change 28), on
//     its thread. The host GPU is up to two frames behind, so the answer is the latest count that
//     arrived for that address: the game sees its query one or two frames late, which a flare's
//     strength does not show (and games read last frame's query anyway). Before the first result the
//     answer is 0 samples.
// Samples are counted at the host resolution and reported at the guest's (divided by the area scale).
//
// DiligentCore takes a slot of its query pool at each begin and gets the slots back when the command
// buffer is submitted (at Present): a frame can begin as many queries as the pool has slots, minus those
// the queries kept for reuse hold. GRAW begins hundreds in a frame (D62). Past the frame's budget a guest
// query gets no host query, and the command processor answers it with its address's latest count.

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "Query.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"

namespace native_x_renderer {

class OcclusionQueries {
 public:
  // DiligentCore's occlusion query pool (EngineVkCreateInfo::QueryPoolSizes, device.cpp; its default is 128).
  static constexpr uint32_t kPoolSize = 4096;

  // False if the device has no occlusion queries (the fake sample count stays).
  bool Initialize(Diligent::IRenderDevice* device);

  // On the render thread, after the guest's Issue call. sample_count_address: the physical address the
  // end event points at.
  void Begin(Diligent::IDeviceContext* context);
  void End(Diligent::IDeviceContext* context, uint32_t sample_count_address);
  // A query is begun and not ended: the command buffer must not be submitted (Vulkan ends queries in the
  // command buffer they began in).
  bool open() const { return active_ != nullptr; }
  // Once per frame, after the frame was submitted: collects the counts that arrived.
  void Poll();

  // The command processor's question (IGraphicsSystem::SetOcclusionQueryResolver), on its thread.
  static bool Resolve(void* user, uint32_t sample_count_address, uint64_t* samples);

  // Host samples per guest sample (the scale of the targets drawn, x times y), for the queries begun from now
  // on: the render scale, or the reduced one in an upscaled view (D49).
  void SetSampleScale(double scale) { sample_scale_ = scale; }

  // Since the last call, for the log.
  struct Stats {
    uint32_t begun = 0, ended = 0, arrived = 0, answered = 0, unanswered = 0;
    uint32_t over_budget = 0;  // guest queries that got no host query: the frame's budget was spent
    uint32_t most_in_a_frame = 0;  // the most host queries begun in one frame
    // The highest count that arrived per sample count address.
    std::unordered_map<uint32_t, uint64_t> max_samples;
  };
  Stats TakeStats();

 private:
  // Ends DiligentCore's render pass, if one is open: a barrier on a small buffer (a barrier always ends
  // it).
  void EndRenderPass(Diligent::IDeviceContext* context);
  Diligent::RefCntAutoPtr<Diligent::IQuery> NewQuery();

  struct Pending {
    Diligent::RefCntAutoPtr<Diligent::IQuery> query;
    uint32_t address;
    double sample_scale;  // the one when it began
  };

  Diligent::IRenderDevice* device_ = nullptr;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> breaker_;
  Diligent::RESOURCE_STATE breaker_state_ = Diligent::RESOURCE_STATE_UNDEFINED;
  Diligent::RefCntAutoPtr<Diligent::IQuery> active_;
  std::vector<Diligent::RefCntAutoPtr<Diligent::IQuery>> free_;
  std::deque<Pending> pending_;
  uint32_t begun_this_frame_ = 0;  // pool slots taken since the last submission
  double sample_scale_ = 1.0;
  double active_sample_scale_ = 1.0;

  // Shared with the command processor's thread.
  std::mutex mutex_;
  std::unordered_map<uint32_t, uint64_t> latest_;  // guest samples by sample count address
  Stats stats_;
};

}  // namespace native_x_renderer
