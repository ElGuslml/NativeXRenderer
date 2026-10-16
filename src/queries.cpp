// NativeXRenderer - see queries.h.

#include "queries.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>

namespace native_x_renderer {

using namespace Diligent;

namespace {
// Pending queries beyond this are dropped (the oldest first): results that late are of no use. A query
// holds its pool slot until it begins again, so the queries alive (at most this many, and the active one)
// and the slots taken in a frame must fit in the pool (D62).
constexpr size_t kMaxPending = 1024;
constexpr uint32_t kMaxBegunPerFrame = 2048;
static_assert(kMaxPending + 1 + kMaxBegunPerFrame <= OcclusionQueries::kPoolSize);
}  // namespace

bool OcclusionQueries::Initialize(IRenderDevice* device) {
  device_ = device;
  if (device->GetDeviceInfo().Features.OcclusionQueries != DEVICE_FEATURE_STATE_ENABLED) {
    REXLOG_WARN("[native_x_renderer] the device has no occlusion queries: the guest's get the fake sample count");
    return false;
  }
  BufferDesc desc;
  desc.Name = "NativeXRenderer render pass breaker";
  desc.Size = 16;
  desc.Usage = USAGE_DEFAULT;
  desc.BindFlags = BIND_UNIFORM_BUFFER;
  device->CreateBuffer(desc, nullptr, &breaker_);
  return breaker_ != nullptr;
}

void OcclusionQueries::EndRenderPass(IDeviceContext* context) {
  // A state change on the buffer, so DiligentCore records a barrier, which ends the render pass.
  RESOURCE_STATE next =
      breaker_state_ == RESOURCE_STATE_COPY_SOURCE ? RESOURCE_STATE_COPY_DEST : RESOURCE_STATE_COPY_SOURCE;
  StateTransitionDesc barrier{breaker_, breaker_state_, next, STATE_TRANSITION_FLAG_UPDATE_STATE};
  context->TransitionResourceStates(1, &barrier);
  breaker_state_ = next;
}

RefCntAutoPtr<IQuery> OcclusionQueries::NewQuery() {
  if (!free_.empty()) {
    RefCntAutoPtr<IQuery> query = std::move(free_.back());
    free_.pop_back();
    return query;
  }
  QueryDesc desc;
  desc.Name = "NativeXRenderer occlusion query";
  desc.Type = QUERY_TYPE_OCCLUSION;
  RefCntAutoPtr<IQuery> query;
  device_->CreateQuery(desc, &query);
  return query;
}

void OcclusionQueries::Begin(IDeviceContext* context) {
  if (active_) {
    // A begin without an end: that query is abandoned.
    EndRenderPass(context);
    context->EndQuery(active_);
    free_.push_back(std::move(active_));
  }
  if (begun_this_frame_ >= kMaxBegunPerFrame) {
    // No slot left for this frame: the guest's query is answered with its address's latest count.
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.over_budget;
    return;
  }
  active_ = NewQuery();
  if (!active_) return;
  EndRenderPass(context);
  context->BeginQuery(active_);
  ++begun_this_frame_;
  active_sample_scale_ = sample_scale_;
  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.begun;
}

void OcclusionQueries::End(IDeviceContext* context, uint32_t sample_count_address) {
  if (!active_) return;
  EndRenderPass(context);
  context->EndQuery(active_);
  pending_.push_back({std::move(active_), sample_count_address, active_sample_scale_});
  active_.Release();
  if (pending_.size() > kMaxPending) {
    free_.push_back(std::move(pending_.front().query));
    pending_.pop_front();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.ended;
}

void OcclusionQueries::Poll() {
  // The frame was submitted: DiligentCore has the slots its queries gave up back.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.most_in_a_frame = std::max(stats_.most_in_a_frame, begun_this_frame_);
  }
  begun_this_frame_ = 0;
  // Results arrive in order; stop at the first that has not.
  while (!pending_.empty()) {
    Pending& front = pending_.front();
    QueryDataOcclusion data;
    if (!front.query->GetData(&data, sizeof(data), true)) break;
    uint64_t samples = front.sample_scale != 1.0
                           ? uint64_t(std::llround(double(data.NumSamples) / front.sample_scale))
                           : data.NumSamples;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      latest_[front.address] = samples;
      ++stats_.arrived;
      uint64_t& max = stats_.max_samples[front.address];
      max = std::max(max, samples);
    }
    free_.push_back(std::move(front.query));
    pending_.pop_front();
  }
}

bool OcclusionQueries::Resolve(void* user, uint32_t sample_count_address, uint64_t* samples) {
  auto* self = static_cast<OcclusionQueries*>(user);
  std::lock_guard<std::mutex> lock(self->mutex_);
  auto it = self->latest_.find(sample_count_address);
  if (it == self->latest_.end()) {
    ++self->stats_.unanswered;
    *samples = 0;  // nothing measured yet for this query: not visible
    return true;
  }
  ++self->stats_.answered;
  *samples = it->second;
  return true;
}

OcclusionQueries::Stats OcclusionQueries::TakeStats() {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats stats = stats_;
  stats_ = {};
  return stats;
}

}  // namespace native_x_renderer
