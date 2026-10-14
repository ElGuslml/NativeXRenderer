// XenosNative - primitive processing: the SDK's PrimitiveProcessor with DiligentCore index buffers
// (internal).
//
// The SDK's PrimitiveProcessor decides, from VGT_DRAW_INITIATOR and the index registers, how a guest draw
// becomes a host draw: the host primitive type, the host vertex shader type, and the index buffer. Guest
// index buffers are used in place, from the guest memory mirror (guest_memory.h), and the vertex shader
// swaps their endianness. What the host cannot draw directly is converted: triangle fans, quad lists and
// line loops become lists and strips, points and rectangles are expanded in the vertex shader (no geometry
// shaders), and indices that use a primitive reset value other than the host's are rewritten.
//
// This class gives it the two buffers it needs: the built-in index buffer (fixed index patterns) and the
// buffer for the indices it converts each frame. Converted indices are written to a CPU arena and copied
// into a GPU buffer at the same offsets after each Process; both are reused from the start every frame
// (the copies are ordered after the previous frame's draws on the queue).

#pragma once

#include <rex/graphics/primitive_processor.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"

namespace xenos_native {

class GuestMemory;

class Primitives final : public rex::graphics::PrimitiveProcessor {
 public:
  Primitives(const rex::graphics::RegisterFile& registers, rex::memory::Memory& memory, GuestMemory& guest_memory,
             Diligent::IRenderDevice* device, Diligent::IDeviceContext* context);
  ~Primitives() override;

  bool Initialize();

  // Uploads what the last Process converted. Call before binding its index buffer. Returns whether it
  // recorded a copy.
  bool UploadConverted();
  // Between frames: forgets the frame's conversions.
  void EndFrame();

  // The index buffer and byte offset for a ProcessingResult's host_index_buffer_handle.
  Diligent::IBuffer* builtin_buffer() const { return builtin_; }
  static uint64_t BuiltinOffset(size_t handle) { return GetBuiltinIndexBufferOffsetBytes(handle); }
  Diligent::IBuffer* converted_buffer() const { return converted_; }
  uint64_t ConvertedOffset(size_t handle) const { return converted_offsets_[handle]; }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes, std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(rex::graphics::xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  static constexpr uint32_t kConvertedBufferSize = 32 << 20;

  Diligent::IRenderDevice* device_;
  Diligent::IDeviceContext* context_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> builtin_;
  Diligent::RefCntAutoPtr<Diligent::IBuffer> converted_;
  std::unique_ptr<uint8_t[]> arena_;
  uint32_t arena_used_ = 0;      // bytes allocated this frame
  uint32_t arena_uploaded_ = 0;  // bytes already copied to the GPU this frame
  std::vector<uint64_t> converted_offsets_;  // per handle, this frame
};

}  // namespace xenos_native
