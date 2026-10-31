// NativeXRenderer - see guest_memory.h.

#include "guest_memory.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
REXCVAR_DEFINE_BOOL(native_x_renderer_compare_unwatched, true, "NativeXRenderer",
                    "Pages left unwatched (D97) are compared with a copy of what the mirror holds, kept on the CPU, "
                    "instead of by hash (D110)");

namespace native_x_renderer {

using namespace Diligent;

namespace {
// Times a page must be found written again, each within kWrittenAgainWithin frames of the last, to be left unwatched;
// frames unchanged before it is watched again (D97). Both are 60 FPS's, scaled by the frame rate (D109).
constexpr uint8_t kUnwatchAfter = 8;
// A ring buffer of the game's (about 18 MB in GH Metallica's songs) rewrites each page every 10 frames or so.
constexpr uint32_t kWrittenAgainWithin = 16;
constexpr uint32_t kWatchAgainAfter = 120;
// The unwatched pages' copy is committed in blocks of this size (D110).
constexpr uint32_t kCopyCommitSize = 0x10000;
}  // namespace

GuestMemory::GuestMemory(rex::memory::Memory& memory, IRenderDevice* device, IDeviceContext* context)
    : SharedMemory(memory), device_(device), context_(context) {}

GuestMemory::~GuestMemory() {
  stop_mapping_ = true;
  if (map_thread_.joinable()) map_thread_.join();
  if (copy_) VirtualFree(copy_, 0, MEM_RELEASE);
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
  // D110: address space only; the blocks the unwatched pages fall in are committed as they are first copied.
  copy_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kBufferSize, MEM_RESERVE, PAGE_NOACCESS));
  if (!copy_) {
    REXLOG_WARN("[native_x_renderer] could not reserve {} MB for the unwatched pages' copy: they are compared by hash",
                kBufferSize >> 20);
  }
  copy_committed_.assign(kBufferSize / kCopyCommitSize, false);
  copy_valid_.assign(kBufferSize >> page_size_log2(), false);
  compare_unwatched_ = copy_ && REXCVAR_GET(native_x_renderer_compare_unwatched);
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
    // The pages whose data is what the mirror holds are skipped; the others are copied in runs, the unwatched ones
    // through their copy on the CPU (D110). A CPU write while a page is checked invalidates it again, so a page
    // skipped here is checked again at its next use.
    uint32_t run_first = 0, run_count = 0;
    bool run_unwatched = false;
    auto upload_run = [&] {
      if (run_count && !(run_unwatched && UploadUnwatched(run_first, run_count))) Upload(run_first, run_count);
      run_count = 0;
    };
    for (uint32_t page = page_first; page < page_first + page_count; ++page) {
      PageWrites& writes = page_writes_[page];
      const bool unwatched = writes.watching == Watching::kUnwatched;
      if (unwatched && window_open_) {
        if (checked_window_[page] == window_) {  // checked in this window already (D98)
          upload_run();
          continue;
        }
        checked_window_[page] = window_;
      }
      const bool compared = unwatched && compare_unwatched_;
      bool unchanged;
      if (compared) {
        unchanged = UnwatchedUnchanged(page);
      } else {
        // A page never copied (or dropped) differs whatever its data: not hashed (D108).
        uint64_t hash = page_hashes_[page]
                            ? XXH3_64bits(memory().TranslatePhysical<const uint8_t*>(page << page_shift), page_size)
                            : 0;
        unchanged = hash && hash == page_hashes_[page];
      }
      if (unwatched) {
        unwatched_hashed_bytes_ += page_size;
        if (!unchanged) {
          writes.changed_frame = frame_;
        } else if (frame_ - writes.changed_frame >= watch_again_after_) {
          Watch(page);  // made valid at its next request
        }
      }
      if (unchanged) {
        unchanged_bytes_ += page_size;
        upload_run();
        continue;
      }
      if (run_count && compared != run_unwatched) upload_run();
      if (!run_count) {
        run_first = page;
        run_unwatched = compared;
      }
      ++run_count;
    }
    upload_run();
  }
  return true;
}

bool GuestMemory::UnwatchedUnchanged(uint32_t page) const {
  if (!copy_valid_[page]) return false;
  const uint32_t page_shift = page_size_log2();
  const uint32_t start = page << page_shift;
  return !std::memcmp(memory().TranslatePhysical<const uint8_t*>(start), copy_ + start, size_t(1) << page_shift);
}

bool GuestMemory::UploadUnwatched(uint32_t page_first, uint32_t page_count) {
  const uint32_t page_shift = page_size_log2();
  const uint32_t start = page_first << page_shift, length = page_count << page_shift;
  for (uint32_t block = start / kCopyCommitSize; block <= (start + length - 1) / kCopyCommitSize; ++block) {
    if (copy_committed_[block]) continue;
    if (!VirtualAlloc(copy_ + size_t(block) * kCopyCommitSize, kCopyCommitSize, MEM_COMMIT, PAGE_READWRITE)) {
      REXLOG_WARN("[native_x_renderer] could not commit memory for the unwatched pages' copy: they are compared by "
                  "hash from now on");
      VirtualFree(copy_, 0, MEM_RELEASE);
      copy_ = nullptr;
      copy_committed_bytes_ = 0;
      compare_unwatched_ = false;
      return false;
    }
    copy_committed_[block] = true;
    copy_committed_bytes_ += kCopyCommitSize;
  }
  // As Upload's snapshot: the CPU may write the pages at any time, so the copy is what the mirror receives.
  std::memcpy(copy_ + start, memory().TranslatePhysical(start), length);
  context_->UpdateBuffer(buffer_, start, length, copy_ + start, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  for (uint32_t page = page_first; page < page_first + page_count; ++page) {
    copy_valid_[page] = true;
    // No hash of what the mirror holds now: once watched again, the page is copied at its first request.
    page_hashes_[page] = 0;
  }
  uploaded_bytes_ += length;
  ++copies_;
  ++total_copies_;
  in_read_state_ = false;
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
  bool again = writes.validated_frame && frame_ + 1 - writes.validated_frame <= written_again_within_;
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
  copy_valid_[page] = false;  // the mirror's data there is copied by Upload from now on (D110)
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
  // D97's frame counts are 60 FPS's (D109): at 160 FPS GH5 rewrites most of its ring buffer's pages every 17 to 32
  // frames, which at 16 were never left unwatched. The frame's time, smoothed over about 64 frames, scales them (from
  // 1 to 4 times, 60 to 240 FPS).
  const auto now = std::chrono::steady_clock::now();
  if (last_frame_end_ != std::chrono::steady_clock::time_point{}) {
    const double ms = std::chrono::duration<double, std::milli>(now - last_frame_end_).count();
    if (ms > 0.0 && ms < 100.0) frame_ms_ += (ms - frame_ms_) / 64.0;
  }
  last_frame_end_ = now;
  const double scale = std::clamp(1000.0 / 60.0 / frame_ms_, 1.0, 4.0);
  written_again_within_ = uint32_t(std::lround(kWrittenAgainWithin * scale));
  watch_again_after_ = uint32_t(std::lround(kWatchAgainAfter * scale));
  compare_unwatched_ = copy_ && REXCVAR_GET(native_x_renderer_compare_unwatched);
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
    copy_valid_[page] = false;
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
    copy_valid_[page_first + i] = false;  // the unwatched pages' copy no longer holds what the mirror does (D110)
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
