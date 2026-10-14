// XenosNative - see queries.h.

#include "queries.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>

namespace xenos_native {

using namespace Diligent;

namespace {
// Pending queries beyond this are dropped (the oldest first): results that late are of no use.
constexpr size_t kMaxPending = 32;
}  // namespace

bool OcclusionQueries::Initialize(IRenderDevice* device) {
  device_ = device;
  if (device->GetDeviceInfo().Features.OcclusionQueries != DEVICE_FEATURE_STATE_ENABLED) {
    REXLOG_WARN("[xenos_native] the device has no occlusion queries: the guest's get the fake sample count");
    return false;
  }
  BufferDesc desc;
  desc.Name = "XenosNative render pass breaker";
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
  desc.Name = "XenosNative occlusion query";
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
    active_.Release();
  }
  active_ = NewQuery();
  if (!active_) return;
  EndRenderPass(context);
  context->BeginQuery(active_);
  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.begun;
}

void OcclusionQueries::End(IDeviceContext* context, uint32_t sample_count_address) {
  if (!active_) return;
  EndRenderPass(context);
  context->EndQuery(active_);
  pending_.push_back({std::move(active_), sample_count_address});
  active_.Release();
  if (pending_.size() > kMaxPending) pending_.pop_front();
  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.ended;
}

void OcclusionQueries::Poll() {
  // Results arrive in order; stop at the first that has not.
  while (!pending_.empty()) {
    Pending& front = pending_.front();
    QueryDataOcclusion data;
    if (!front.query->GetData(&data, sizeof(data), true)) break;
    uint64_t samples = sample_scale_ > 1.0 ? uint64_t(std::llround(double(data.NumSamples) / sample_scale_))
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

}  // namespace xenos_native
