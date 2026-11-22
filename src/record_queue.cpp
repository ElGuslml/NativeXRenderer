// NativeXRenderer - the record stage's queue (see record_queue.h).

#include "record_queue.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace native_x_renderer {

thread_local bool RecordQueue::on_record_thread_ = false;

RecordQueue::RecordQueue(Diligent::IDeviceContext* context)
    : context_(context), current_(std::make_unique<Batch>()) {}

RecordQueue::~RecordQueue() {
  SetThreaded(false);
  Flush();
}

void RecordQueue::Batch::RunAll(Diligent::IDeviceContext* context) {
  // Operations queued while running (none expected) run in the same pass.
  for (size_t i = 0; i < ops.size(); ++i) ops[i].run(ops[i].op, context);
  ops.clear();
  for (Chunk& c : chunks) c.used = 0;
  chunk = 0;
}

void* RecordQueue::Allocate(size_t bytes, size_t alignment) {
  Batch& batch = *current_;
  while (true) {
    if (batch.chunk < batch.chunks.size()) {
      Chunk& chunk = batch.chunks[batch.chunk];
      size_t start = (chunk.used + alignment - 1) & ~(alignment - 1);
      if (start + bytes <= chunk.size) {
        chunk.used = start + bytes;
        return chunk.data.get() + start;
      }
      ++batch.chunk;
      continue;
    }
    Chunk chunk;
    chunk.size = std::max(kChunkSize, bytes + alignment);
    chunk.data = std::make_unique<uint8_t[]>(chunk.size);
    batch.chunks.push_back(std::move(chunk));
  }
}

void* RecordQueue::Alloc(size_t bytes, size_t alignment) {
  if (immediate()) {
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

void RecordQueue::SetDelay(uint32_t commands) {
  if (commands == delay_) return;
  Flush();
  delay_ = commands;
}

void RecordQueue::SetThreaded(bool on) {
  if (on == threaded()) return;
  Flush();
  if (on) {
    stop_ = false;
    thread_ = std::thread([this] { ThreadMain(); });
  } else {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    work_.notify_all();
    thread_.join();
  }
}

void RecordQueue::RunCurrent() {
  if (flushing_) return;  // an operation that takes the context directly runs inside the flush already
  flushing_ = true;
  current_->RunAll(context_);
  flushing_ = false;
}

void RecordQueue::Submit() {
  if (current_->ops.empty()) return;
  std::unique_ptr<Batch> next;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    // At most kMaxInFlight batches ahead of the record thread.
    if (submitted_.size() >= kMaxInFlight) {
      ++stats_.full_waits;
      done_.wait(lock, [&] { return submitted_.size() < kMaxInFlight; });
    }
    stats_.operations += current_->ops.size();
    ++stats_.batches;
    submitted_.push_back(std::move(current_));
    if (!free_.empty()) {
      next = std::move(free_.back());
      free_.pop_back();
    }
  }
  work_.notify_one();
  current_ = next ? std::move(next) : std::make_unique<Batch>();
}

void RecordQueue::WaitIdle() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (submitted_.empty() && !running_) return;
  ++stats_.waits;
  const auto start = std::chrono::steady_clock::now();
  done_.wait(lock, [&] { return submitted_.empty() && !running_; });
  stats_.wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void RecordQueue::Flush() {
  if (on_record_thread_) return;
  if (threaded()) {
    Submit();
    WaitIdle();
    return;
  }
  RunCurrent();
}

void RecordQueue::HandOver() {
  if (on_record_thread_ || !threaded()) return;
  commands_ = 0;
  Submit();
}

void RecordQueue::EndCommand() {
  if (threaded()) {
    if (++commands_ >= kSubmitCommands || current_->ops.size() >= kSubmitOperations) {
      commands_ = 0;
      Submit();
    }
    return;
  }
  if (delay_ && ++commands_ >= delay_) {
    commands_ = 0;
    Flush();
  }
}

void RecordQueue::ThreadMain() {
#ifdef _WIN32
  SetThreadDescription(GetCurrentThread(), L"NativeXRenderer record");
#endif
  on_record_thread_ = true;
  while (true) {
    std::unique_ptr<Batch> batch;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (submitted_.empty() && !stop_) ++stats_.sleeps;  // D181
      work_.wait(lock, [&] { return stop_ || !submitted_.empty(); });
      if (submitted_.empty()) return;  // stop_, nothing left
      batch = std::move(submitted_.front());
      submitted_.pop_front();
      running_ = true;
    }
    batch->RunAll(context_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      free_.push_back(std::move(batch));
      running_ = false;
    }
    done_.notify_all();
  }
}

uint64_t RecordQueue::ThreadCycles() {
#ifdef _WIN32
  ULONG64 cycles = 0;
  if (thread_.joinable() && QueryThreadCycleTime(static_cast<HANDLE>(thread_.native_handle()), &cycles)) {
    return uint64_t(cycles);
  }
#endif
  return 0;
}

RecordQueue::Stats RecordQueue::TakeStats() {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats stats = stats_;
  stats_ = {};
  return stats;
}

}  // namespace native_x_renderer
