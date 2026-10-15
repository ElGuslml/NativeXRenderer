// NativeXRenderer - see primitives.h.

#include "primitives.h"

#include "guest_memory.h"

#include <rex/logging.h>

namespace native_x_renderer {

using namespace Diligent;
namespace rg = rex::graphics;

Primitives::Primitives(const rg::RegisterFile& registers, rex::memory::Memory& memory, GuestMemory& guest_memory,
                       IRenderDevice* device, IDeviceContext* context)
    : PrimitiveProcessor(registers, memory, guest_memory), device_(device), context_(context) {}

Primitives::~Primitives() { ShutdownCommon(); }

bool Primitives::Initialize() {
  // Full 32-bit indices (the device has fullDrawIndexUint32). DiligentCore has no triangle fans and no
  // geometry shaders here: fans, line loops and quad lists are converted, points and rectangles are
  // expanded in the vertex shader.
  if (!InitializeCommon(true, false, false, false, false, false)) return false;

  BufferDesc desc;
  desc.Name = "NativeXRenderer converted indices";
  desc.Size = kConvertedBufferSize;
  desc.Usage = USAGE_DEFAULT;
  desc.BindFlags = BIND_INDEX_BUFFER;
  device_->CreateBuffer(desc, nullptr, &converted_);
  arena_ = std::make_unique<uint8_t[]>(kConvertedBufferSize);
  return converted_ != nullptr;
}

bool Primitives::InitializeBuiltinIndexBuffer(size_t size_bytes, std::function<void(void*)> fill_callback) {
  std::vector<uint8_t> data(size_bytes);
  fill_callback(data.data());
  BufferDesc desc;
  desc.Name = "NativeXRenderer built-in indices";
  desc.Size = size_bytes;
  desc.Usage = USAGE_IMMUTABLE;
  desc.BindFlags = BIND_INDEX_BUFFER;
  BufferData initial{data.data(), size_bytes};
  device_->CreateBuffer(desc, &initial, &builtin_);
  return builtin_ != nullptr;
}

void* Primitives::RequestHostConvertedIndexBufferForCurrentFrame(rg::xenos::IndexFormat format,
                                                                 uint32_t index_count, bool coalign_for_simd,
                                                                 uint32_t coalignment_original_address,
                                                                 size_t& backend_handle_out) {
  uint32_t index_size = format == rg::xenos::IndexFormat::kInt16 ? 2 : 4;
  uint32_t start = (arena_used_ + 3) & ~3u;
  uint64_t size = uint64_t(index_size) * index_count + (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0);
  if (start + size > kConvertedBufferSize) {
    REXLOG_ERROR("[native_x_renderer] converted indices: the frame needs more than {} MB", kConvertedBufferSize >> 20);
    return nullptr;
  }
  uint8_t* mapping = arena_.get() + start;
  if (coalign_for_simd) mapping += GetSimdCoalignmentOffset(mapping, coalignment_original_address);
  arena_used_ = start + uint32_t(size);
  backend_handle_out = converted_offsets_.size();
  converted_offsets_.push_back(uint64_t(mapping - arena_.get()));
  return mapping;
}

bool Primitives::UploadConverted() {
  if (arena_used_ <= arena_uploaded_) return false;
  context_->UpdateBuffer(converted_, arena_uploaded_, arena_used_ - arena_uploaded_, arena_.get() + arena_uploaded_,
                         RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  arena_uploaded_ = arena_used_;
  return true;
}

void Primitives::EndFrame() {
  ClearPerFrameCache();
  arena_used_ = arena_uploaded_ = 0;
  converted_offsets_.clear();
}

}  // namespace native_x_renderer
