// NativeXRenderer - the record stage's queue (see record_queue.h).

#include "record_queue.h"

#include <algorithm>
#include <cstring>

namespace native_x_renderer {

RecordQueue::~RecordQueue() { Flush(); }

void* RecordQueue::Allocate(size_t bytes, size_t alignment) {
  while (true) {
    if (chunk_ < chunks_.size()) {
      Chunk& chunk = chunks_[chunk_];
      size_t start = (chunk.used + alignment - 1) & ~(alignment - 1);
      if (start + bytes <= chunk.size) {
        chunk.used = start + bytes;
        return chunk.data.get() + start;
      }
      ++chunk_;
      continue;
    }
    Chunk chunk;
    chunk.size = std::max(kChunkSize, bytes + alignment);
    chunk.data = std::make_unique<uint8_t[]>(chunk.size);
    chunks_.push_back(std::move(chunk));
  }
}

void* RecordQueue::Alloc(size_t bytes, size_t alignment) {
  if (!delay_) {
    if (immediate_.size() < bytes + alignment) immediate_.resize(bytes + alignment);
    uintptr_t base = reinterpret_cast<uintptr_t>(immediate_.data());
    return reinterpret_cast<void*>((base + alignment - 1) & ~(uintptr_t(alignment) - 1));
  }
  return Allocate(bytes, alignment);
}

const void* RecordQueue::Copy(const void* data, size_t bytes) {
  void* to = Alloc(bytes);
  std::memcpy(to, data, bytes);
  return to;
}

void RecordQueue::Flush() {
  if (flushing_) return;  // an operation that takes the context directly runs inside the flush already
  flushing_ = true;
  // Operations queued while running (none expected) run in the same pass.
  for (size_t i = 0; i < ops_.size(); ++i) ops_[i].run(ops_[i].op, context_);
  ops_.clear();
  for (Chunk& chunk : chunks_) chunk.used = 0;
  chunk_ = 0;
  flushing_ = false;
}

}  // namespace native_x_renderer
