// XenosNative - the GPU-side copy of guest physical memory (internal).
//
// The translated shaders read vertices (vfetch) from a 512 MB storage buffer that mirrors guest physical
// memory, and indexed draws use the same buffer as their index buffer: this is the SDK's "shared memory"
// design, and the class is the SDK's SharedMemory with a DiligentCore buffer underneath. SharedMemory keeps
// one valid bit per 4 KB page and protects the pages it uploaded; a CPU write to one of them (Unlock or
// not: Bink's planes too) clears the bit, and the next draw that needs the page uploads it again.
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

namespace xenos_native {

class GuestMemory final : public rex::graphics::SharedMemory {
 public:
  GuestMemory(rex::memory::Memory& memory, Diligent::IRenderDevice* device, Diligent::IDeviceContext* context);
  ~GuestMemory() override;

  bool Initialize();

  Diligent::IBuffer* buffer() const { return buffer_; }
  // The views the shaders bind: read-only (no memexport) and read-write.
  Diligent::IBufferView* view() const { return srv_; }
  Diligent::IBufferView* uav() const { return uav_; }

  // Bytes uploaded since the last call.
  uint64_t TakeUploadedBytes() {
    uint64_t bytes = uploaded_bytes_;
    uploaded_bytes_ = 0;
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
  Diligent::IRenderDevice* device_;
  Diligent::IDeviceContext* context_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer_;
  Diligent::IBufferView* srv_ = nullptr;
  Diligent::IBufferView* uav_ = nullptr;
  uint64_t uploaded_bytes_ = 0;
  uint32_t copies_ = 0;
};

}  // namespace xenos_native
