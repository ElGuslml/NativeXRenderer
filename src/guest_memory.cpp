// NativeXRenderer - see guest_memory.h.

#include "guest_memory.h"

#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <cstring>

#include <xxhash.h>

namespace native_x_renderer {

using namespace Diligent;

GuestMemory::GuestMemory(rex::memory::Memory& memory, IRenderDevice* device, IDeviceContext* context)
    : SharedMemory(memory), device_(device), context_(context) {}

GuestMemory::~GuestMemory() = default;

bool GuestMemory::Initialize() {
  InitializeCommon();
  page_hashes_.assign(kBufferSize >> page_size_log2(), 0);
  BufferDesc desc;
  desc.Name = "NativeXRenderer guest memory";
  desc.Size = kBufferSize;
  desc.Usage = USAGE_DEFAULT;
  desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS | BIND_INDEX_BUFFER;
  desc.Mode = BUFFER_MODE_RAW;
  device_->CreateBuffer(desc, nullptr, &buffer_);
  if (!buffer_) {
    REXLOG_ERROR("[native_x_renderer] could not create the {} MB guest memory buffer", kBufferSize >> 20);
    return false;
  }
  srv_ = buffer_->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE);
  uav_ = buffer_->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS);
  return true;
}

bool GuestMemory::UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  const uint32_t page_shift = page_size_log2();
  const uint32_t page_size = UINT32_C(1) << page_shift;
  for (const auto& [page_first, page_count] : upload_page_ranges) {
    // Valid before the data is read, as the SDK does: a CPU write from now on invalidates the pages again.
    MakeRangeValid(page_first << page_shift, page_count << page_shift, false);
    // The pages whose data is what the mirror holds are skipped; the others are copied in runs. A CPU write
    // while a page is hashed invalidates it again, so a page skipped here is checked again at its next use.
    uint32_t run_first = 0, run_count = 0;
    for (uint32_t page = page_first; page < page_first + page_count; ++page) {
      uint64_t hash = XXH3_64bits(memory().TranslatePhysical<const uint8_t*>(page << page_shift), page_size);
      if (hash && hash == page_hashes_[page]) {
        unchanged_bytes_ += page_size;
        if (run_count) Upload(run_first, run_count);
        run_count = 0;
        continue;
      }
      if (!run_count) run_first = page;
      ++run_count;
    }
    if (run_count) Upload(run_first, run_count);
  }
  return true;
}

void GuestMemory::Upload(uint32_t page_first, uint32_t page_count) {
  // The recorded hashes must be of the bytes the mirror receives, and the CPU may write the pages at any
  // time: copy them once, then hash and upload that copy.
  const uint32_t page_shift = page_size_log2();
  const uint32_t page_size = UINT32_C(1) << page_shift;
  uint32_t start = page_first << page_shift, length = page_count << page_shift;
  snapshot_.resize(length);
  std::memcpy(snapshot_.data(), memory().TranslatePhysical(start), length);
  for (uint32_t i = 0; i < page_count; ++i) {
    page_hashes_[page_first + i] = XXH3_64bits(snapshot_.data() + (size_t(i) << page_shift), page_size);
  }
  context_->UpdateBuffer(buffer_, start, length, snapshot_.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  uploaded_bytes_ += length;
  ++copies_;
  in_read_state_ = false;
}

void GuestMemory::PrepareForRead(IDeviceContext* context) {
  if (in_read_state_) return;
  StateTransitionDesc barrier{buffer_, RESOURCE_STATE_UNKNOWN,
                              RESOURCE_STATE_SHADER_RESOURCE | RESOURCE_STATE_INDEX_BUFFER,
                              STATE_TRANSITION_FLAG_UPDATE_STATE};
  context->TransitionResourceStates(1, &barrier);
  in_read_state_ = true;
}

}  // namespace native_x_renderer
