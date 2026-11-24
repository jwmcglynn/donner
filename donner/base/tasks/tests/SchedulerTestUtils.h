#pragma once
/// @file
/// Test helpers for the task scheduler: a peer that reaches its private entry points, waits under
/// the hang guard, and hooks that hold or delay a thread at a protocol step.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/tasks/TaskScheduler.h"
#include "donner/base/tests/StallGuard.h"

namespace donner::tasks {

/// Reaches the scheduler's private spawn entry points and counters. Tasks here have no scope:
/// tests observe completion through their own latches and counters.
class TaskSchedulerTestPeer {
public:
  /// Constructs a scheduler with a test seam.
  static std::unique_ptr<TaskScheduler> Make(TaskSchedulerOptions options,
                                             SchedulerTestHooks* hooks) {
    return std::unique_ptr<TaskScheduler>(new TaskScheduler(options, hooks));
  }

  /// Moves \p work into a task only when the spawn succeeds; a declined spawn leaves it intact.
  template <typename F>
  static bool TrySpawn(TaskScheduler& scheduler, F& work) {
    return scheduler.trySpawn(std::move(work));
  }

  template <typename F>
  static void Spawn(TaskScheduler& scheduler, F work) {
    scheduler.spawn(std::move(work));
  }

  static std::int64_t AwakeCount(const TaskScheduler& scheduler) {
    return scheduler.awake_.load(std::memory_order_seq_cst);
  }

  static std::int64_t AwakeStates(const TaskScheduler& scheduler) {
    std::int64_t count = 0;
    for (const auto& worker : scheduler.workers_) {
      if (worker->state.load(std::memory_order_seq_cst) == WorkerState::Awake) {
        ++count;
      }
    }
    return count;
  }

  static WorkerState State(const TaskScheduler& scheduler, std::size_t worker) {
    return scheduler.workers_[worker]->state.load(std::memory_order_seq_cst);
  }

  static const Parker<>* WorkerParker(const TaskScheduler& scheduler, std::size_t worker) {
    return &scheduler.workers_[worker]->parker;
  }

  static std::int64_t GlobalQueued(const TaskScheduler& scheduler) {
    return scheduler.globalQueued_.load(std::memory_order_seq_cst);
  }

  /// The calling thread's worker index in \p scheduler, or -1 if it is not one of its workers.
  static int WorkerIndex(const TaskScheduler& scheduler) {
    const TaskScheduler::Worker* worker = scheduler.currentWorker();
    return worker == nullptr ? -1 : static_cast<int>(worker->index);
  }

  static std::size_t StealVictim(std::size_t self, std::size_t workers, std::size_t offset,
                                 std::size_t k) {
    return TaskScheduler::StealVictim(self, workers, offset, k);
  }

  /// Records of the external pool, for threads that are not workers, not yet returned.
  static std::size_t ExternalLiveRecords(TaskScheduler& scheduler) {
    std::unique_lock lock(scheduler.globalMutex_);
    return scheduler.externalPool_.liveCount(lock);
  }

  /// The scheduler's parker pool.
  static std::shared_ptr<ParkerPool> Pool(const TaskScheduler& scheduler) {
    return scheduler.parkerPool_;
  }

  static std::size_t LiveScopes(const TaskScheduler& scheduler) {
    return scheduler.liveScopes_.load(std::memory_order_relaxed);
  }
};

/// A one-shot event a thread waits for under the hang guard.
class Event {
public:
  void set() {
    std::lock_guard lock(mutex_);
    set_ = true;
    cv_.notify_all();
  }

  /// @return False if the event was not set within the hang guard.
  [[nodiscard]] bool wait() {
    std::unique_lock lock(mutex_);
    return cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                          [this] { return set_; });
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool set_ = false;
};

/// Counts finished tasks; a test waits for the count it expects under the hang guard.
class Completion {
public:
  void arrive() {
    std::lock_guard lock(mutex_);
    ++count_;
    cv_.notify_all();
  }

  /// @return False if \p count arrivals did not happen within the hang guard.
  [[nodiscard]] bool waitFor(int count) {
    std::unique_lock lock(mutex_);
    return cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                          [&] { return count_ >= count; });
  }

  [[nodiscard]] int count() {
    std::lock_guard lock(mutex_);
    return count_;
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  int count_ = 0;
};

/// A rendezvous that holds one thread until the test releases it.
class Gate {
public:
  void arriveAndWait() {
    std::unique_lock lock(mutex_);
    arrived_ = true;
    cv_.notify_all();
    if (!cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                        [this] { return released_; })) {
      heldPastHangGuard_ = true;
    }
  }

  [[nodiscard]] bool waitForArrival() {
    std::unique_lock lock(mutex_);
    return cv_.wait_until(lock, std::chrono::steady_clock::now() + tests::kHangGuard,
                          [this] { return arrived_; });
  }

  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }

  [[nodiscard]] bool heldPastHangGuard() {
    std::lock_guard lock(mutex_);
    return heldPastHangGuard_;
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool arrived_ = false;
  bool released_ = false;
  bool heldPastHangGuard_ = false;
};

/// Hooks that hold the thread reaching a chosen step for the \p occurrence-th time at a gate.
class HoldOnceHooks : public SchedulerTestHooks {
public:
  HoldOnceHooks(SchedulerStep step, Gate* gate, int occurrence = 1)
      : step_(step), gate_(gate), occurrence_(occurrence) {}

  void at(SchedulerStep step, std::size_t /*worker*/) override {
    if (step == step_ && reached_.fetch_add(1) + 1 == occurrence_) {
      gate_->arriveAndWait();
    }
  }

private:
  const SchedulerStep step_;
  Gate* const gate_;
  const int occurrence_;
  std::atomic<int> reached_{0};
};

/**
 * Hooks that count every step, and hold the thread that reaches a chosen step for a chosen time at
 * a gate. Holds are added before the scheduler runs.
 */
class StepHooks : public SchedulerTestHooks {
public:
  /// Holds the thread reaching \p step for the \p occurrence-th time at \p gate.
  void holdAt(SchedulerStep step, int occurrence, Gate* gate) {
    holds_.push_back(Hold{step, occurrence, gate});
  }

  /// How many times any thread reached \p step.
  [[nodiscard]] int count(SchedulerStep step) const {
    return counts_[static_cast<std::size_t>(step)].load();
  }

  void at(SchedulerStep step, std::size_t /*worker*/) override {
    const int reached = counts_[static_cast<std::size_t>(step)].fetch_add(1) + 1;
    for (const Hold& hold : holds_) {
      if (hold.step == step && hold.occurrence == reached) {
        hold.gate->arriveAndWait();
      }
    }
  }

private:
  struct Hold {
    SchedulerStep step;
    int occurrence;
    Gate* gate;
  };

  std::array<std::atomic<int>, 32> counts_{};
  std::vector<Hold> holds_;
};

/// Hooks that combine two holds.
class HoldTwiceHooks : public SchedulerTestHooks {
public:
  HoldTwiceHooks(HoldOnceHooks* first, HoldOnceHooks* second) : first_(first), second_(second) {}

  void at(SchedulerStep step, std::size_t worker) override {
    first_->at(step, worker);
    second_->at(step, worker);
  }

private:
  HoldOnceHooks* const first_;
  HoldOnceHooks* const second_;
};

/**
 * Reads a worker's parker epoch. The pointer is taken while the scheduler is alive; it stays valid
 * while that worker is held at a hook, since the destructor joins the worker before freeing it.
 */
class ParkerEpochWatch {
public:
  ParkerEpochWatch(const TaskScheduler& scheduler, std::size_t worker)
      : parker_(TaskSchedulerTestPeer::WorkerParker(scheduler, worker)) {}

  [[nodiscard]] std::uint64_t load() const { return Parker<>::EpochOf(parker_->word()); }

private:
  const Parker<>* parker_;
};

/// Polls \p condition under the hang guard. @return False if it never held.
template <typename Condition>
bool PollUntil(Condition condition) {
  const tests::HangGuard guard;
  while (guard.active()) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(tests::kPollInterval);
  }
  return condition();
}

/// Small deterministic generator; one per thread.
class XorShift32 {
public:
  explicit XorShift32(std::uint32_t seed) : state_(seed == 0 ? 1 : seed) {}

  std::uint32_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return state_;
  }

private:
  std::uint32_t state_;
};

/// Hooks that delay the calling thread at every protocol step: usually a yield, sometimes a few
/// microseconds.
class DelayHooks : public SchedulerTestHooks {
public:
  void at(SchedulerStep /*step*/, std::size_t /*worker*/) override {
    thread_local XorShift32 rng(
        static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    const std::uint32_t roll = rng.next() % 16;
    if (roll < 8) {
      std::this_thread::yield();
    } else if (roll < 10) {
      std::this_thread::sleep_for(std::chrono::microseconds(roll));
    }
  }
};

}  // namespace donner::tasks
