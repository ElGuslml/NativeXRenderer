// NativeXRenderer - the renderer's commands, and the queue to the thread that runs them (internal, D71).
//
// The renderer works in two steps. On the game's render thread, inside its D3D calls, it captures what a call
// needs from guest memory and from the state tracker into a command: the register changes since the last
// command, the programs, the surfaces bound, the call's arguments and the values they point to. A thread of
// its own runs the commands in order: the draw path, clears, resolves, Present, DiligentCore and the driver.
// With native_x_renderer_thread off, each command runs as soon as it is captured, on the game's thread.
//
// What the renderer's thread still reads from guest memory, later than the game submitted it: vertex buffers,
// index buffers and textures. The game reuses such memory only after the GPU has signalled that it is done
// with it, and the command processor waits at every signal until the queue is empty (the SDK's recomp change 31), so
// the game never sees the GPU further along than the renderer. Inline draws keep their vertices and indices
// in the command buffer, which the library reuses on its own schedule: they are copied into the command.
//
// The queue holds kSlots commands, which keep their memory from one use to the next. The game's thread waits
// when it is full: the renderer's thread is at most kSlots calls behind.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "state.h"
#include "targets.h"

namespace native_x_renderer {

struct Command {
  enum class Type : uint8_t {
    kNone,
    kDraw,
    kClear,
    kResolve,
    kBeginTiling,
    kEndTiling,
    kQuery,
    kPresent,
    kFrameEnd,
    kBeginView,
    kEndViewGeometry,
    kAspectBox,
  };
  Type type = Type::kNone;
  Call call{};
  // Applied to the renderer's register file before the command runs: the tracker's record (RegisterRecord, D73).
  std::vector<uint32_t> registers;
  // The call's arguments (r3..r10, f1).
  std::array<uint32_t, 8> r{};
  double f1 = 0;
  // Draw: the surfaces bound (render targets 0-3, depth), as their objects say; clear: the same when ClearF
  // ran; resolve: [0] the source, [4] the depth when the resolve clears it (D80).
  std::array<GuestSurface, 5> surfaces{};

  // Draw: the programs it runs (the tracker keeps every program it loaded, D72), and an inline draw's vertices and
  // indices, copied from the command buffer.
  const StateTracker::Program* programs[2] = {};
  struct Copy {
    uint32_t address = 0;  // GPU address
    uint32_t offset = 0;   // in `data`
    uint32_t size = 0;
  };
  std::array<Copy, 2> copies{};
  uint32_t copy_count = 0;
  std::vector<uint8_t> data;

  // Clear: its color (when given), and its rectangle when given (x1, y1, x2, y2; D78: a rectangle that covers a
  // target clears it whole; D80: a smaller one is filled). Resolve: the color it clears its source with afterwards.
  bool has_color = false, has_rect = false;
  std::array<float, 4> color{};
  std::array<int32_t, 4> rect{};

  // Resolve and Present: the texture's fetch constant (when there is a texture). Resolve: the source rectangle
  // and the destination point (when given), RB_COPY_DEST_INFO, and whether its callers are the scene resolve's
  // (the profile's scene_resolve_caller, D41, D65).
  bool has_texture = false, has_source_rect = false, has_dest_point = false, scene_caller = false;
  std::array<uint32_t, 6> fetch{};
  std::array<int32_t, 4> source_rect{};
  std::array<int32_t, 2> dest_point{};
  uint32_t dest_info = 0;
  // Resolve: the stencil it clears the depth with afterwards (RB_DEPTH_CLEAR bits 0-7; the depth is f1, D80).
  uint32_t clear_stencil = 0;

  // BeginTiling: the tile count and the size of the whole image; EndTiling: a resolve destination given;
  // query: the sample count address; frame end: the frame number.
  uint32_t count = 0, width = 0, height = 0, address = 0, frame = 0;
  // BeginView: the scene's view-projection matrix (when given); the aspect box: on.
  bool has_matrix = false, on = false;
  std::array<float, 16> matrix{};

  // The game's thread, since the last frame end (frame end commands only): commands captured.
  uint32_t captured = 0;
  // Present, with native_x_renderer_present_with_swaps: the number of the guest's swap that shows this frame
  // (the swaps VdSwap had issued when the game's Present returned, D86); 0: shown at once.
  uint64_t swap_tag = 0;

  // Before reuse: the containers keep their capacity.
  void Reset() {
    type = Type::kNone;
    registers.clear();
    programs[0] = programs[1] = nullptr;
    copy_count = 0;
    data.clear();
    has_color = has_rect = has_texture = has_source_rect = has_dest_point = scene_caller = false;
    clear_stencil = 0;
    has_matrix = on = false;
    swap_tag = 0;
  }
};

class CommandQueue {
 public:
  using Executor = void (*)(Command& command);
  // Called on the renderer's thread whenever it wakes with no command to run (Wake, or every few milliseconds
  // while it waits): work that does not come from a command (D86).
  using Poller = void (*)();
  static constexpr uint32_t kSlots = 4096;

  CommandQueue();
  ~CommandQueue();

  // `threaded`: a thread of its own runs the commands; otherwise each runs when it is published. `poll`: see
  // Poller (optional, threaded only).
  void Start(bool threaded, Executor execute, Poller poll = nullptr);
  // From any thread: the renderer's thread runs the poller soon, even with no command to run (D86).
  void Wake();
  // Stops the thread after the command it is running (at exit), waiting for it up to a second.
  void Stop();
  bool threaded() const { return threaded_; }

  // The producer, the game's render thread: the next command to fill (reset), then Publish. Waits while the
  // queue is full.
  Command& Next();
  void Publish();
  // Returns once every command published before the call has run: the command processor's signal gate.
  void WaitIdle();

  // On the renderer's thread, while a command runs: the command `ahead` places after it, if the game's thread has
  // published it, else nullptr (D98). Published commands do not change until they have run. Threaded only.
  const Command* Peek(uint32_t ahead) const;
  // On the renderer's thread: the position of the command it runs, counted from the start.
  uint64_t running_index() const { return running_index_; }

  struct Waits {
    uint64_t count = 0;
    double ms = 0;
  };
  // Since the last call (for the log): the game's thread waiting for room; the command processor at its
  // signals.
  Waits TakeProducerWaits() { return Take(producer_waits_, producer_wait_ns_); }
  Waits TakeGateWaits() { return Take(gate_waits_, gate_wait_ns_); }

 private:
  void Run();
  // Until `target` commands have run, or the thread stopped.
  void WaitConsumed(uint64_t target, std::atomic<uint64_t>& waits, std::atomic<uint64_t>& wait_ns);
  static Waits Take(std::atomic<uint64_t>& waits, std::atomic<uint64_t>& wait_ns);

  std::vector<Command> slots_;
  Executor execute_ = nullptr;
  Poller poll_ = nullptr;
  bool threaded_ = false;
  // Each side writes its own position on a cache line of its own; the producer reads the consumer's only when its
  // last reading says the queue is full (D72).
  alignas(64) std::atomic<uint64_t> produced_{0};
  alignas(64) std::atomic<uint64_t> consumed_{0};
  alignas(64) uint64_t consumed_seen_ = 0;  // the producer's last reading of consumed_
  uint64_t running_index_ = 0;              // the renderer's thread only: the command it runs (D98)
  // Waking: the renderer's thread when it sleeps on an empty queue; the waiters for `wake_at_` commands run.
  alignas(64) std::atomic<bool> consumer_sleeping_{false};
  std::atomic<bool> wake_requested_{false};  // Wake
  std::atomic<uint64_t> wake_at_{UINT64_MAX};
  std::mutex mutex_;
  std::condition_variable produced_cv_, consumed_cv_;
  std::atomic<bool> running_{false}, stop_{false};
  bool finished_ = false;  // guarded by mutex_
  std::thread thread_;
  std::atomic<uint64_t> producer_waits_{0}, producer_wait_ns_{0}, gate_waits_{0}, gate_wait_ns_{0};
};

}  // namespace native_x_renderer
