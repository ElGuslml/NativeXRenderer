// NativeXRenderer - see guest_memory.h.

#include "guest_memory.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <xxhash.h>

REXCVAR_DEFINE_BOOL(native_x_renderer_unwatch_rewritten, true, "NativeXRenderer",
                    "Guest memory pages the game rewrites every frame are compared by hash at each use instead of "
                    "being watched for writes (D97)");
REXCVAR_DEFINE_BOOL(native_x_renderer_map_guest_memory, true, "NativeXRenderer",
                    "Every guest memory page read once on a thread of its own when the draw path starts, so that the "
                    "renderer's copies of new data (a screen's textures) do not map each page as they read it (D108)");

namespace native_x_renderer {

using namespace Diligent;

namespace {
// Times a page must be found written again, each within kWrittenAgainWithin frames of the last, to be left unwatched;
// frames unchanged before it is watched again (D97).
constexpr uint8_t kUnwatchAfter = 8;
// A ring buffer of the game's (about 18 MB in GH Metallica's songs) rewrites each page every 10 frames or so.
constexpr uint32_t kWrittenAgainWithin = 16;
constexpr uint32_t kWatchAgainAfter = 120;
}  // namespace

GuestMemory::GuestMemory(rex::memory::Memory& memory, IRenderDevice* device, IDeviceContext* context)
    : SharedMemory(memory), device_(device), context_(context) {}

GuestMemory::~GuestMemory() {
  stop_mapping_ = true;
  if (map_thread_.joinable()) map_thread_.join();
}

bool GuestMemory::Initialize() {
  InitializeCommon();
  page_hashes_.assign(kBufferSize >> page_size_log2(), 0);
  page_writes_.assign(kBufferSize >> page_size_log2(), PageWrites{});
  checked_window_.assign(kBufferSize >> page_size_log2(), 0);
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
  if (REXCVAR_GET(native_x_renderer_map_guest_memory)) MapAhead();
  return true;
}

void GuestMemory::MapAhead() {
  // The first read of a page through this view costs about 1.1 us (the system maps it there, though the game wrote
  // it through another view): GH5's venue textures, 37.6 MB uploaded in one frame, spent 10.9 ms on it. Read here
  // once, 64 KB at a time, with ReadProcessMemory: a page the SDK keeps without access (a range whose data comes
  // from the GPU) fails the read instead of raising the exception its handlers would take for the game's access;
  // a 64 KB block that fails is read page by page. Below normal priority, about 130 ms for 512 MB.
  const uint8_t* base = memory().TranslatePhysical<const uint8_t*>(0);
  map_thread_ = std::thread([this, base] {
    SetThreadDescription(GetCurrentThread(), L"NativeXRenderer guest memory mapping");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto start = std::chrono::steady_clock::now();
    std::vector<uint8_t> block(0x10000);
    const HANDLE process = GetCurrentProcess();
    uint32_t unreadable = 0;
    for (uint32_t offset = 0; offset < kBufferSize && !stop_mapping_; offset += uint32_t(block.size())) {
      SIZE_T read = 0;
      if (ReadProcessMemory(process, base + offset, block.data(), block.size(), &read)) continue;
      for (uint32_t page = 0; page < block.size(); page += 4096) {
        if (!ReadProcessMemory(process, base + offset + page, block.data(), 1, &read)) ++unreadable;
      }
    }
    REXLOG_INFO("[native_x_renderer] guest memory mapped ahead in {:.0f} ms ({} pages without access) (D108)",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                unreadable);
  });
}

bool GuestMemory::UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  const uint32_t page_shift = page_size_log2();
  const uint32_t page_size = UINT32_C(1) << page_shift;
  for (const auto& [page_first, page_count] : upload_page_ranges) {
    // Valid before the data is read, as the SDK does: a CPU write from now on invalidates the pages again. The
    // pages rewritten every frame are left invalid instead, unwatched (D97): every request hashes them.
    uint32_t valid_first = 0, valid_count = 0;
    bool valid_always = false;
    for (uint32_t page = page_first; page < page_first + page_count; ++page) {
      bool unwatched = KeepUnwatched(page);
      bool always = page_writes_[page].watching == Watching::kAlways;
      if (valid_count && (unwatched || always != valid_always)) {
        MakeValid(valid_first, valid_count, valid_always);
        valid_count = 0;
      }
      if (unwatched) continue;
      if (!valid_count) {
        valid_first = page;
        valid_always = always;
      }
      ++valid_count;
    }
    if (valid_count) MakeValid(valid_first, valid_count, valid_always);
    // The pages whose data is what the mirror holds are skipped; the others are copied in runs. A CPU write
    // while a page is hashed invalidates it again, so a page skipped here is checked again at its next use.
    uint32_t run_first = 0, run_count = 0;
    for (uint32_t page = page_first; page < page_first + page_count; ++page) {
      PageWrites& writes = page_writes_[page];
      if (writes.watching == Watching::kUnwatched && window_open_) {
        if (checked_window_[page] == window_) {  // hashed in this window already (D98)
          if (run_count) Upload(run_first, run_count);
          run_count = 0;
          continue;
        }
        checked_window_[page] = window_;
      }
      // A page never copied (or dropped) differs whatever its data: not hashed (D108).
      uint64_t hash = page_hashes_[page]
                          ? XXH3_64bits(memory().TranslatePhysical<const uint8_t*>(page << page_shift), page_size)
                          : 0;
      bool unchanged = hash && hash == page_hashes_[page];
      if (writes.watching == Watching::kUnwatched) {
        unwatched_hashed_bytes_ += page_size;
        if (!unchanged) {
          writes.changed_frame = frame_;
        } else if (frame_ - writes.changed_frame >= kWatchAgainAfter) {
          Watch(page);  // made valid at its next request
        }
      }
      if (unchanged) {
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

bool GuestMemory::KeepUnwatched(uint32_t page) {
  PageWrites& writes = page_writes_[page];
  if (writes.watching == Watching::kAlways) return false;
  if (texture_requests_ > 0 || !unwatch_) {
    if (writes.watching == Watching::kUnwatched) {
      // The game's writes while it was unwatched told no watch: told now, for a texture there.
      Watch(page);
      MemoryInvalidationCallback(page << page_size_log2(), UINT32_C(1) << page_size_log2(), true);
    }
    if (texture_requests_ > 0) writes.watching = Watching::kAlways;
    return false;
  }
  if (writes.watching == Watching::kUnwatched) return true;
  // Invalid again within a few frames of being made valid: written again.
  bool again = writes.validated_frame && frame_ + 1 - writes.validated_frame <= kWrittenAgainWithin;
  writes.in_a_row = again ? uint8_t(std::min(writes.in_a_row + 1, 255)) : 1;
  writes.validated_frame = frame_ + 1;
  if (writes.in_a_row < kUnwatchAfter) return false;
  writes.watching = Watching::kUnwatched;
  writes.changed_frame = frame_;
  ++unwatched_pages_;
  return true;
}

void GuestMemory::MakeValid(uint32_t page_first, uint32_t page_count, bool always_watched) {
  // The SDK widens the invalidation around a CPU write up to 256 KB (one fault for a buffer written from start to
  // end), but not across pages written by the GPU. Texture data and resolve destinations are marked so, though they
  // hold what guest memory does: the game rewrites single pages of a few small textures every frame, and their
  // widened invalidations made hundreds of texture pages around them be protected again and hashed every frame
  // (D97). The flag has no other use in the SDK.
  MakeRangeValid(page_first << page_size_log2(), page_count << page_size_log2(), always_watched);
}

void GuestMemory::Watch(uint32_t page) {
  PageWrites& writes = page_writes_[page];
  if (writes.watching == Watching::kUnwatched) --unwatched_pages_;
  writes.watching = Watching::kWatched;
  writes.in_a_row = 0;
  writes.validated_frame = 0;
}

void GuestMemory::ResolveWritten(uint32_t start, uint32_t length) {
  if (length && start < kBufferSize) {
    const uint32_t page_shift = page_size_log2();
    uint32_t last = std::min(start + (length - 1), kBufferSize - 1);
    for (uint32_t page = start >> page_shift; page <= last >> page_shift; ++page) {
      Watch(page);
      page_writes_[page].watching = Watching::kAlways;
    }
  }
  MakeRangeValid(start, length, true);
}

void GuestMemory::EndFrame() {
  ++frame_;
  unwatch_ = REXCVAR_GET(native_x_renderer_unwatch_rewritten);
}

bool GuestMemory::WriteIfChanged(IDeviceContext* context, uint32_t address, const uint8_t* data, uint32_t size) {
  if (!size || address >= kBufferSize || size > kBufferSize - address) return false;
  if (!std::memcmp(memory().TranslatePhysical(address), data, size)) return false;
  context->UpdateBuffer(buffer_, address, size, data, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  uploaded_bytes_ += size;
  ++copies_;
  ++total_copies_;
  in_read_state_ = false;
  // The mirror holds the old data there now: the next request copies the pages from guest memory again, hashes
  // or not.
  const uint32_t page_shift = page_size_log2();
  for (uint32_t page = address >> page_shift; page <= (address + size - 1) >> page_shift; ++page) {
    page_hashes_[page] = 0;
    checked_window_[page] = 0;
  }
  MemoryInvalidationCallback(address, size, true);
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
  ++total_copies_;
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
