// NativeXRenderer - the record stage's queue (internal; D153, D154).
//
// The renderer's thread does two kinds of work: it prepares each command from the game's state (registers, guest
// memory, the texture cache, pipelines, binding sets, constants) and it records DiligentCore context calls for it.
// The calls go through this queue as operations, in order (D153). Three modes:
// - at once (the default): an operation runs as a direct call would;
// - delayed (native_x_renderer_record_delay = N): the operations wait and run, in order, every N commands on the same
//   thread: what a record thread would see, deterministic, so that an operation that captured something by reference,
//   or used an object released before it ran, shows here first;
// - a record thread (native_x_renderer_record_thread, D154): the operations go in batches to a thread of their own,
//   which records them while the renderer's thread prepares the next commands.
//
// Data an operation needs later (constants, a snapshot of guest memory) is copied into the queue's arena (Alloc),
// valid until the operation has run. Direct() and Sync() wait until every queued operation has run (with the record
// thread: until it is idle), then the calling thread uses the context: one thread at a time ever does.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "DeviceContext.h"

namespace native_x_renderer {

class RecordQueue {
 public:
  explicit RecordQueue(Diligent::IDeviceContext* context);
  ~RecordQueue();
  RecordQueue(const RecordQueue&) = delete;
  RecordQueue& operator=(const RecordQueue&) = delete;

  // 0: every operation at once; N: queued and run every N commands (EndCommand), and at every Direct() or Flush().
  void SetDelay(uint32_t commands);
  // D154: a record thread runs the operations (started or stopped here, between commands).
  void SetThreaded(bool on);
  bool threaded() const { return thread_.joinable(); }
  // Whether an operation runs at once where it is pushed (no delay and no thread, or inside Sync).
  bool immediate() const { return (delay_ == 0 && !threaded()) || inline_ > 0; }

  // Queues `op(IDeviceContext*)`, or runs it at once.
  template <typename Op>
  void Push(Op&& op) {
    if (immediate() || on_record_thread_) {
      op(context_);
      return;
    }
    using Stored = std::decay_t<Op>;
    void* where = Allocate(sizeof(Stored), alignof(Stored));
    Stored* stored = new (where) Stored(std::forward<Op>(op));
    current_->ops.push_back({&Run<Stored>, stored});
  }

  // Memory for an operation's data, valid until it has run (in immediate mode, until the next Alloc).
  void* Alloc(size_t bytes, size_t alignment = 16);
  // Copies `bytes` of `data` into the arena.
  const void* Copy(const void* data, size_t bytes);

  // Runs every queued operation (with the record thread: hands them over and waits until it is idle).
  void Flush();
  // The end of a renderer command: delayed, every N of them Flush; with the record thread, the batch is handed over
  // every few commands.
  void EndCommand();
  // D165: the operations queued so far handed to the record thread now, without waiting for them to run (with the
  // record thread only). Called after Present's operation, so that the Present runs as soon as the frame is recorded,
  // not once the next frames have queued enough commands for EndCommand's batch.
  void HandOver();
  // The context, with every queued operation run first.
  Diligent::IDeviceContext* Direct() {
    Flush();
    return context_;
  }

  // Runs `op(context)` now, after every queued operation, and returns what it returns; what it queues itself runs at
  // once. For work whose result prepare needs (whether DLSS upscaled, a query open), or that calls back into prepare
  // (the motion draws inside the upscaler), and the frame's Present.
  template <typename Op>
  auto Sync(Op&& op) -> decltype(op(static_cast<Diligent::IDeviceContext*>(nullptr))) {
    Flush();
    struct Inline {
      uint32_t& depth;
      explicit Inline(uint32_t& d) : depth(d) { ++depth; }
      ~Inline() { --depth; }
    } scope(inline_);
    return op(context_);
  }

  // For the log (D154): batches handed to the record thread, and the waits for it, since the last call.
  struct Stats {
    uint64_t batches = 0, operations = 0, waits = 0, full_waits = 0;
    double wait_ms = 0.0;
  };
  Stats TakeStats();

 private:
  struct Entry {
    void (*run)(void* op, Diligent::IDeviceContext* context);  // runs and destroys
    void* op;
  };
  template <typename Op>
  static void Run(void* op, Diligent::IDeviceContext* context) {
    Op* typed = static_cast<Op*>(op);
    (*typed)(context);
    typed->~Op();
  }
  // A batch of operations and the arena they and their data live in (chunks of kChunkSize, or larger for a larger
  // block), reused once run.
  static constexpr size_t kChunkSize = size_t(1) << 20;
  struct Chunk {
    std::unique_ptr<uint8_t[]> data;
    size_t size = 0, used = 0;
  };
  struct Batch {
    std::vector<Entry> ops;
    std::vector<Chunk> chunks;
    size_t chunk = 0;
    void RunAll(Diligent::IDeviceContext* context);
  };
  void* Allocate(size_t bytes, size_t alignment);
  void RunCurrent();
  void Submit();
  void WaitIdle();
  void ThreadMain();

  Diligent::IDeviceContext* context_;
  uint32_t delay_ = 0, commands_ = 0, inline_ = 0;
  bool flushing_ = false;
  std::unique_ptr<Batch> current_;
  std::vector<uint8_t> immediate_;  // Alloc's memory in immediate mode

  // D154: the record thread and its batches. A batch every 32 commands or 384 operations: with 8 and 96 the renderer's
  // thread handed over ~100 batches a frame and waited for room ~5 times a frame at 480 FPS (2 % more time per draw).
  static constexpr uint32_t kSubmitCommands = 32, kSubmitOperations = 384, kMaxInFlight = 6;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable work_, done_;
  std::deque<std::unique_ptr<Batch>> submitted_;
  std::vector<std::unique_ptr<Batch>> free_;
  bool running_ = false, stop_ = false;
  static thread_local bool on_record_thread_;
  Stats stats_;
};

}  // namespace native_x_renderer
