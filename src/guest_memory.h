// NativeXRenderer - the GPU-side copy of guest physical memory (internal).
//
// The translated shaders read vertices (vfetch) from a 512 MB storage buffer that mirrors guest physical
// memory, and indexed draws use the same buffer as their index buffer: this is the SDK's "shared memory"
// design, and the class is the SDK's SharedMemory with a DiligentCore buffer underneath. SharedMemory keeps
// one valid bit per 4 KB page and protects the pages it uploaded; a CPU write to one of them (Unlock or
// not: Bink's planes too) clears the bit, and the next draw that needs the page uploads it again.
//
// The invalidation is coarser than the CPU's writes: a write invalidates the pages around it too, and
// POTCAWE writes next to its vertex, index and texture data every frame. So a page is copied only if its
// data differs from what was last copied into it (a hash per page, D35): at the dock about 2.5 % of the
// pages requested again had changed. Every copy ends DiligentCore's render pass.
//
// The SDK's own SharedMemory (in the Vulkan backend) is idle on this path: with host rendering off, the
// command processor requests no ranges. Both register with the memory's invalidation callbacks, which
// support several listeners.
//
// Watching costs more than hashing for the pages the game rewrites every frame (D97): each frame the renderer
// thread protected them again (VirtualProtect per run) and the game's first write to each faulted and unprotected
// up to 256 KB around it, whose pages were then requested and hashed again, unchanged. In GH Metallica's songs that
// was over half of the renderer thread's time (protections 26 %, hashing 19 %, the memory's lock held by the faults
// 10 %). A page found written again 8 times, each within 16 frames of the last, is left unwatched and invalid: every
// request hashes it, and copies it if it changed. After 120 frames unchanged it is watched again. Texture data and
// resolve destinations stay watched: the texture cache and the resolved textures learn of the game's writes there
// through the watches; and a write next to them no longer invalidates them too (MakeValid).
//
// An unwatched page is compared with a copy of what the mirror holds, kept on the CPU, instead of by hash (D110):
// most of them changed at each request (45 % in GH5's songs, 75 % in GH Metallica's), and a compare stops at the
// first difference where a hash reads the whole page; the copy uploaded needs no hash either.

#pragma once

#include <rex/graphics/shared_memory.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "Buffer.h"
#include "BufferView.h"
#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"

namespace native_x_renderer {

class GuestMemory final : public rex::graphics::SharedMemory {
 public:
  GuestMemory(rex::memory::Memory& memory, Diligent::IRenderDevice* device, Diligent::IDeviceContext* context);
  ~GuestMemory() override;

  bool Initialize();
  // Every page of the guest memory read once through the view the mirror copies from (TranslatePhysical), on a thread
  // of its own, so that the copies find them mapped (D108).
  void MapAhead();
  // The guest memory it mirrors, for reading guest data on the CPU.
  using rex::graphics::SharedMemory::memory;

  Diligent::IBuffer* buffer() const { return buffer_; }
  // The views the shaders bind: read-only (no memexport) and read-write.
  Diligent::IBufferView* view() const { return srv_; }
  Diligent::IBufferView* uav() const { return uav_; }

  // A resolve wrote [start, start + length) of guest memory, on the host only (D70). The pages are marked valid
  // and written by the GPU, as the SDK's RangeWrittenByGpu does for the Xenos path, without firing the watches:
  // they become write-watched, and the invalidation the mirror widens around a CPU write stops at them, so the
  // global watches report a resolve destination only when its own memory changes. The mirror keeps its data
  // there: a draw reading that memory as vertices reads what was last uploaded.
  // Its pages stay watched from then on (D97).
  void ResolveWritten(uint32_t start, uint32_t length);

  // Around the texture cache's requests (GuestTextures::RequestTextures): the pages they upload stay watched (D97).
  void BeginTextureRequests() { ++texture_requests_; }
  void EndTextureRequests() { --texture_requests_; }

  // Uploads ahead (D98): from BeginWindow to EndWindow the queued draws' data is as the window's request found it
  // (the game does not reuse it before the command processor's next signal), so an unwatched page that request, or
  // a draw's, hashed is not hashed again by the next draws' requests until EndWindow. An inline draw's restore
  // clears that for its pages.
  void BeginWindow() {
    ++window_;
    window_open_ = true;
  }
  void EndWindow() { window_open_ = false; }

  // Once a frame (DrawPath::EndFrame): the frames count the writes of pages rewritten every frame (D97).
  void EndFrame();
  // Pages left unwatched now, and bytes of them compared since the last call (D97, D110).
  uint32_t unwatched_pages() const { return unwatched_pages_; }
  uint64_t TakeUnwatchedHashedBytes() {
    uint64_t bytes = unwatched_hashed_bytes_;
    unwatched_hashed_bytes_ = 0;
    return bytes;
  }
  // The memory the unwatched pages' copy on the CPU takes (D110).
  uint64_t copy_committed_bytes() const { return copy_committed_bytes_; }

  // Writes `size` bytes at `address` into the mirror if guest memory no longer holds them: an inline draw's data
  // that the game reused since the draw was captured (D71). The pages are then invalidated, so that the next draw
  // reading them takes guest memory again. Returns whether it wrote.
  bool WriteIfChanged(Diligent::IDeviceContext* context, uint32_t address, const uint8_t* data, uint32_t size);

  // Before GPU work reads the mirror (draws, texture loads): after uploads, puts the buffer in one state
  // for every way it is read (storage buffer and index buffer), so binding it needs no further barrier.
  void PrepareForRead(Diligent::IDeviceContext* context);

  // Bytes uploaded since the last call.
  uint64_t TakeUploadedBytes() {
    uint64_t bytes = uploaded_bytes_;
    uploaded_bytes_ = 0;
    return bytes;
  }
  // Bytes requested again but not uploaded since the last call: their data had not changed (D35).
  uint64_t TakeUnchangedBytes() {
    uint64_t bytes = unchanged_bytes_;
    unchanged_bytes_ = 0;
    return bytes;
  }
  // Copies recorded so far (D98: whether a draw's requests copied).
  uint64_t copies() const { return total_copies_; }
  // Copies recorded since the last call (each one ends the render pass in progress).
  uint32_t TakeCopies() {
    uint32_t copies = copies_;
    copies_ = 0;
    return copies;
  }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  // Copies pages to the mirror, from a snapshot whose hashes it records.
  void Upload(uint32_t page_first, uint32_t page_count);
  // Copies unwatched pages to the mirror through their copy on the CPU (D110); false if it could not be committed.
  bool UploadUnwatched(uint32_t page_first, uint32_t page_count);
  // An unwatched page holds what the mirror does (D110).
  bool UnwatchedUnchanged(uint32_t page) const;
  // A requested page that is invalid: whether it stays unwatched (D97); counts its writes in a row.
  bool KeepUnwatched(uint32_t page);
  void Watch(uint32_t page);
  void MakeValid(uint32_t page_first, uint32_t page_count, bool always_watched);

  // Per page (D97): how it is watched, and the frames that tell when to change.
  enum class Watching : uint8_t { kWatched, kUnwatched, kAlways };
  struct PageWrites {
    uint32_t validated_frame = 0;  // frame + 1 when last made valid here, 0 if never
    uint32_t changed_frame = 0;    // while unwatched: the last frame its data changed
    uint8_t in_a_row = 0;          // times in a row it was found written again soon after being made valid
    Watching watching = Watching::kWatched;
  };
  std::vector<PageWrites> page_writes_;
  // D98: per page, the window in which an unwatched page was last hashed (0: none).
  std::vector<uint32_t> checked_window_;
  uint32_t window_ = 0;
  bool window_open_ = false;
  uint32_t frame_ = 0;
  // D97's frame counts at the current frame rate (D109), and what measures it.
  uint32_t written_again_within_ = 16, watch_again_after_ = 120;
  double frame_ms_ = 1000.0 / 60.0;
  std::chrono::steady_clock::time_point last_frame_end_;
  int texture_requests_ = 0;
  bool unwatch_ = true;
  uint32_t unwatched_pages_ = 0;
  uint64_t unwatched_hashed_bytes_ = 0;

  Diligent::IRenderDevice* device_;
  Diligent::IDeviceContext* context_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer_;
  Diligent::IBufferView* srv_ = nullptr;
  Diligent::IBufferView* uav_ = nullptr;
  // Per page: the hash of the data last copied into the mirror, 0 if none.
  std::vector<uint64_t> page_hashes_;
  std::vector<uint8_t> snapshot_;
  // D110: the unwatched pages' data as the mirror holds it, each at its own offset in an address range of the
  // mirror's size committed 64 KB at a time; per 64 KB whether committed, per page whether it holds the mirror's data.
  uint8_t* copy_ = nullptr;
  std::vector<bool> copy_committed_;
  std::vector<bool> copy_valid_;
  uint64_t copy_committed_bytes_ = 0;
  bool compare_unwatched_ = false;
  // MapAhead's thread, and the flag that stops it early (the destructor).
  std::thread map_thread_;
  std::atomic<bool> stop_mapping_{false};
  uint64_t uploaded_bytes_ = 0, unchanged_bytes_ = 0;
  uint32_t copies_ = 0;
  uint64_t total_copies_ = 0;
  bool in_read_state_ = false;
};

}  // namespace native_x_renderer
