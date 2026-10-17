// NativeXRenderer - see commands.h.

#include "commands.h"

#include <rex/logging.h>
#include <rex/thread.h>

#include <chrono>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace native_x_renderer {
namespace {

using Clock = std::chrono::steady_clock;

void Pause() {
#if defined(_M_X64) || defined(__x86_64__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

// Waits on a condition variable are bounded: a missed wake-up costs at most this long.
constexpr auto kWaitSlice = std::chrono::milliseconds(5);

}  // namespace

CommandQueue::CommandQueue() = default;

CommandQueue::~CommandQueue() { Stop(); }

void CommandQueue::Start(bool threaded, Executor execute) {
  slots_.resize(kSlots);
  execute_ = execute;
  threaded_ = threaded;
  if (!threaded) return;
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this] { Run(); });
}

void CommandQueue::Stop() {
  if (!thread_.joinable()) return;
  stop_.store(true, std::memory_order_seq_cst);
  bool finished;
  {
    std::unique_lock lock(mutex_);
    produced_cv_.notify_all();
    finished = consumed_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return finished_; });
  }
  // A thread stuck in the driver at exit is left to the process's end.
  if (finished) {
    thread_.join();
  } else {
    thread_.detach();
  }
}

Command& CommandQueue::Next() {
  uint64_t produced = produced_.load(std::memory_order_relaxed);
  if (threaded_ && produced - consumed_seen_ >= kSlots) {
    consumed_seen_ = consumed_.load(std::memory_order_acquire);
    if (produced - consumed_seen_ >= kSlots) {
      WaitConsumed(produced - kSlots + 1, producer_waits_, producer_wait_ns_);
      consumed_seen_ = consumed_.load(std::memory_order_acquire);
    }
  }
  Command& command = slots_[produced % kSlots];
  command.Reset();
  return command;
}

void CommandQueue::Publish() {
  uint64_t produced = produced_.load(std::memory_order_relaxed);
  if (!threaded_) {
    execute_(slots_[produced % kSlots]);
    produced_.store(produced + 1, std::memory_order_relaxed);
    consumed_.store(produced + 1, std::memory_order_relaxed);
    return;
  }
  // Sequentially consistent with the renderer's thread going to sleep (Run): one of the two sees the other.
  produced_.store(produced + 1, std::memory_order_seq_cst);
  if (consumer_sleeping_.load(std::memory_order_seq_cst)) {
    std::lock_guard lock(mutex_);
    produced_cv_.notify_one();
  }
}

void CommandQueue::WaitIdle() {
  if (!threaded_) return;
  WaitConsumed(produced_.load(std::memory_order_seq_cst), gate_waits_, gate_wait_ns_);
}

void CommandQueue::WaitConsumed(uint64_t target, std::atomic<uint64_t>& waits, std::atomic<uint64_t>& wait_ns) {
  if (consumed_.load(std::memory_order_acquire) >= target || !running_.load(std::memory_order_acquire)) return;
  auto start = Clock::now();
  // The thread is usually a few commands away: spin briefly before sleeping.
  for (uint32_t i = 0; i < 4096 && consumed_.load(std::memory_order_acquire) < target; ++i) Pause();
  if (consumed_.load(std::memory_order_acquire) < target) {
    std::unique_lock lock(mutex_);
    while (running_.load(std::memory_order_acquire)) {
      // Ask the thread to wake us at `target`, then check: sequentially consistent with its store of
      // `consumed_` and its load of `wake_at_` (Run), so one of the two sees the other.
      uint64_t wake_at = wake_at_.load(std::memory_order_seq_cst);
      while (target < wake_at && !wake_at_.compare_exchange_weak(wake_at, target, std::memory_order_seq_cst)) {
      }
      if (consumed_.load(std::memory_order_seq_cst) >= target) break;
      consumed_cv_.wait_for(lock, kWaitSlice);
    }
  }
  waits.fetch_add(1, std::memory_order_relaxed);
  wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()),
                    std::memory_order_relaxed);
}

CommandQueue::Waits CommandQueue::Take(std::atomic<uint64_t>& waits, std::atomic<uint64_t>& wait_ns) {
  Waits result;
  result.count = waits.exchange(0, std::memory_order_relaxed);
  result.ms = double(wait_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
  return result;
}

void CommandQueue::Run() {
  rex::thread::set_current_thread_name("NativeXRenderer");
  uint64_t consumed = consumed_.load(std::memory_order_relaxed);
  while (!stop_.load(std::memory_order_acquire)) {
    if (produced_.load(std::memory_order_acquire) == consumed) {
      // Empty: spin briefly (the game's thread is usually inside its next call), then sleep.
      for (uint32_t i = 0; i < 1024 && produced_.load(std::memory_order_acquire) == consumed; ++i) Pause();
      if (produced_.load(std::memory_order_acquire) == consumed) {
        std::unique_lock lock(mutex_);
        consumer_sleeping_.store(true, std::memory_order_seq_cst);
        while (produced_.load(std::memory_order_seq_cst) == consumed && !stop_.load(std::memory_order_acquire)) {
          produced_cv_.wait_for(lock, kWaitSlice);
        }
        consumer_sleeping_.store(false, std::memory_order_relaxed);
      }
      continue;
    }
    execute_(slots_[consumed % kSlots]);
    ++consumed;
    consumed_.store(consumed, std::memory_order_seq_cst);
    if (consumed >= wake_at_.load(std::memory_order_seq_cst)) {
      std::lock_guard lock(mutex_);
      wake_at_.store(UINT64_MAX, std::memory_order_seq_cst);
      consumed_cv_.notify_all();
    }
  }
  std::lock_guard lock(mutex_);
  running_.store(false, std::memory_order_release);
  finished_ = true;
  consumed_cv_.notify_all();
}

}  // namespace native_x_renderer
