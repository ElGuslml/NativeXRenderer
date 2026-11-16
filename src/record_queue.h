// NativeXRenderer - the record stage's queue (internal; GH Warriors of Rock's local_docs/31, step 3).
//
// The renderer's thread does two kinds of work: it prepares each command from the game's state (registers, guest
// memory, the texture cache, pipelines, binding sets, constants) and it records DiligentCore context calls for it.
// The calls go through this queue as operations, in order. With no delay (the default) an operation runs at once,
// as a direct call would. With a delay of N commands the operations wait and run, in order, every N commands on the
// same thread: what a record thread would see, deterministic, so that an operation that captured something by
// reference, or used an object released before it ran, shows here first.
//
// Data an operation needs later (constants, a snapshot of guest memory) is copied into the queue's arena (Alloc),
// valid until the operations queued so far have run. Direct() runs the queued operations and returns the context, for
// a call that needs its result at once (a map, a query) or code not converted yet.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "DeviceContext.h"

namespace native_x_renderer {

class RecordQueue {
 public:
  explicit RecordQueue(Diligent::IDeviceContext* context) : context_(context) {}
  ~RecordQueue();
  RecordQueue(const RecordQueue&) = delete;
  RecordQueue& operator=(const RecordQueue&) = delete;

  // 0: every operation at once; N: queued and run every N commands (EndCommand), and at every Direct() or Flush().
  void SetDelay(uint32_t commands) {
    Flush();
    delay_ = commands;
  }
  bool immediate() const { return delay_ == 0 || inline_ > 0; }

  // Queues `op(IDeviceContext*)`, or runs it at once (no delay, or inside Sync).
  template <typename Op>
  void Push(Op&& op) {
    if (!delay_ || inline_) {
      op(context_);
      return;
    }
    using Stored = std::decay_t<Op>;
    void* where = Allocate(sizeof(Stored), alignof(Stored));
    Stored* stored = new (where) Stored(std::forward<Op>(op));
    ops_.push_back({&Run<Stored>, stored});
  }

  // Memory for an operation's data, valid until it has run (in immediate mode, until the next Alloc).
  void* Alloc(size_t bytes, size_t alignment = 16);
  // Copies `bytes` of `data` into the arena.
  const void* Copy(const void* data, size_t bytes);

  // Runs the queued operations, in order.
  void Flush();
  // The end of a renderer command: every N of them, Flush.
  void EndCommand() {
    if (delay_ && ++commands_ >= delay_) {
      commands_ = 0;
      Flush();
    }
  }
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
  void* Allocate(size_t bytes, size_t alignment);

  Diligent::IDeviceContext* context_;
  uint32_t delay_ = 0, commands_ = 0, inline_ = 0;
  bool flushing_ = false;
  std::vector<Entry> ops_;
  // The arena: chunks of kChunkSize (or larger for a larger block), reused after each Flush.
  static constexpr size_t kChunkSize = size_t(1) << 20;
  struct Chunk {
    std::unique_ptr<uint8_t[]> data;
    size_t size = 0, used = 0;
  };
  std::vector<Chunk> chunks_;
  size_t chunk_ = 0;
  std::vector<uint8_t> immediate_;  // Alloc's memory in immediate mode
};

}  // namespace native_x_renderer
