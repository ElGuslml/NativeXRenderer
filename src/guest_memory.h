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

#pragma once

#include <rex/graphics/shared_memory.h>

#include <cstdint>
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
  // The guest memory it mirrors, for reading guest data on the CPU.
  using rex::graphics::SharedMemory::memory;

  Diligent::IBuffer* buffer() const { return buffer_; }
  // The views the shaders bind: read-only (no memexport) and read-write.
  Diligent::IBufferView* view() const { return srv_; }
  Diligent::IBufferView* uav() const { return uav_; }

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

  Diligent::IRenderDevice* device_;
  Diligent::IDeviceContext* context_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer_;
  Diligent::IBufferView* srv_ = nullptr;
  Diligent::IBufferView* uav_ = nullptr;
  // Per page: the hash of the data last copied into the mirror, 0 if none.
  std::vector<uint64_t> page_hashes_;
  std::vector<uint8_t> snapshot_;
  uint64_t uploaded_bytes_ = 0, unchanged_bytes_ = 0;
  uint32_t copies_ = 0;
  bool in_read_state_ = false;
};

}  // namespace native_x_renderer
