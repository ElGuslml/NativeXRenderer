// XenosNative - see guest_memory.h.

#include "guest_memory.h"

#include <rex/logging.h>
#include <rex/system/xmemory.h>

namespace xenos_native {

using namespace Diligent;

GuestMemory::GuestMemory(rex::memory::Memory& memory, IRenderDevice* device, IDeviceContext* context)
    : SharedMemory(memory), device_(device), context_(context) {}

GuestMemory::~GuestMemory() = default;

bool GuestMemory::Initialize() {
  InitializeCommon();
  BufferDesc desc;
  desc.Name = "XenosNative guest memory";
  desc.Size = kBufferSize;
  desc.Usage = USAGE_DEFAULT;
  desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS | BIND_INDEX_BUFFER;
  desc.Mode = BUFFER_MODE_RAW;
  device_->CreateBuffer(desc, nullptr, &buffer_);
  if (!buffer_) {
    REXLOG_ERROR("[xenos_native] could not create the {} MB guest memory buffer", kBufferSize >> 20);
    return false;
  }
  srv_ = buffer_->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE);
  uav_ = buffer_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS);
  return true;
}

bool GuestMemory::UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  const uint32_t page_shift = page_size_log2();
  for (const auto& [page_first, page_count] : upload_page_ranges) {
    uint32_t start = page_first << page_shift, length = page_count << page_shift;
    // Valid before the copy, as the SDK does: a CPU write during the copy invalidates the pages again.
    MakeRangeValid(start, length, false);
    context_->UpdateBuffer(buffer_, start, length, memory().TranslatePhysical(start),
                           RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    uploaded_bytes_ += length;
    ++copies_;
    in_read_state_ = false;
  }
  return true;
}

void GuestMemory::PrepareForRead(IDeviceContext* context) {
  if (in_read_state_) return;
  StateTransitionDesc barrier{buffer_, RESOURCE_STATE_UNKNOWN,
                              RESOURCE_STATE_SHADER_RESOURCE | RESOURCE_STATE_INDEX_BUFFER,
                              STATE_TRANSITION_FLAG_UPDATE_STATE};
  context->TransitionResourceStates(1, &barrier);
  in_read_state_ = true;
}

}  // namespace xenos_native
