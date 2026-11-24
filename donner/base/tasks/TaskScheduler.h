#pragma once
/// @file
/// The task scheduler: worker threads, their deques, the global queue for threads that are not
/// workers, and the sleep and publish protocol that keeps queued work from waiting on a sleeping
/// pool.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/base/tasks/FalseSharing.h"
#include "donner/base/tasks/Parker.h"
#include "donner/base/tasks/ParkerPool.h"
#include "donner/base/tasks/TaskRecord.h"
#include "donner/base/tasks/WorkStealingDeque.h"

namespace donner::tasks {

/// Options for a \ref TaskScheduler.
struct TaskSchedulerOptions {
  /// Workers to start. 0 runs every spawn inline on the spawning thread.
  std::size_t workerCount = 0;

  /// Entries per worker deque; a power of two.
  std::size_t dequeCapacity = 4096;

  /// Live task records per worker.
  std::size_t maxTasksPerCreator = 4096;

  /// Live task records for every thread that is not a worker, together.
  std::size_t maxExternalTasks = 64 * 1024;

  /// Steal rounds an idle worker makes before it tries to sleep.
  std::size_t idleSpinRounds = 1;
};

/// A worker's state word.
enum class WorkerState : std::uint8_t {
  Awake,          //!< Counted in the scheduler's awake count.
  AsleepShallow,  //!< Asleep while within its stack margin, as in the idle loop.
  /**
   * Asleep past its stack margin. Not stored yet: only a worker waiting on a scope can be past its
   * margin, and an idle worker sleeps shallow. The swap and a publisher's scan already accept it,
   * so a deep sleeper is woken by the same code once one exists.
   */
  AsleepDeep,
};

/// Ostream output operator for \ref WorkerState, for test diagnostics.
inline std::ostream& operator<<(std::ostream& os, WorkerState state) {
  switch (state) {
    case WorkerState::Awake: return os << "Awake";
    case WorkerState::AsleepShallow: return os << "AsleepShallow";
    case WorkerState::AsleepDeep: return os << "AsleepDeep";
  }
  UTILS_UNREACHABLE();
}

/// A point in the sleep, publish and shutdown protocols where a test can hold or delay a thread.
enum class SchedulerStep : std::uint8_t {
  AfterDrain,             //!< A sleeper drained its deque (step 1), before its waiting bit.
  AfterSleeperDecrement,  //!< A sleeper stored asleep and decremented (step 3), before
                          //!< its re-check.
  BeforeSleeperWaits,     //!< A sleeper's re-check found nothing (step 4), before its wait.
  BeforeSelfWakeSwap,     //!< A sleeper decided to wake itself, before its swap.
  AfterPublishPush,       //!< A publisher queued work, before it reads the awake count.
  BetweenWakeSwapAndParkerChange,  //!< A waker made its swap, before it changes the parker.
  AfterStopFlagSet,                //!< The destructor set the stop flag, before its wakes.
  ScopeWaiterBeforePrepareWait,    //!< A scope's creator found tasks outstanding, before it sets
                                   //!< its parker's waiting bit.
  ScopeWaiterBeforeWait,  //!< A scope's creator set the waiting bit and its re-check found tasks
                          //!< outstanding, before it waits.
  ScopeFinisherAfterDecrement,  //!< A finisher's decrement brought its scope's count to zero,
                                //!< before it changes the recorded parker.
};

/// Ostream output operator for \ref SchedulerStep, for test diagnostics.
std::ostream& operator<<(std::ostream& os, SchedulerStep step);

/**
 * Test seam: called at each \ref SchedulerStep with the index of the worker the step concerns.
 * Production schedulers have none, and each step then costs one branch on a path that is already
 * about to sleep or wake a thread.
 */
class SchedulerTestHooks {
public:
  virtual ~SchedulerTestHooks() = default;

  /**
   * Called at \p step.
   *
   * @param step The protocol step.
   * @param worker The worker the step concerns: the sleeper, the woken worker or the finisher;
   *   zero for \ref SchedulerStep::AfterPublishPush, \ref SchedulerStep::AfterStopFlagSet and the
   *   steps of a scope's creator, which is not a worker.
   */
  virtual void at(SchedulerStep step, std::size_t worker) = 0;
};

class TaskSchedulerTestPeer;
class TaskScope;

/**
 * A work-stealing task scheduler.
 *
 * Each worker owns a fixed-capacity deque and a pool of task records. A task spawned on a worker
 * goes onto that worker's deque; a task spawned by any other thread goes into one global
 * first-in, first-out queue, protected by a mutex, with a count beside it that the sleep protocol
 * reads without the mutex. An idle worker runs its own deque, takes from the global queue at least
 * once every 61 tasks it runs from its deque, and otherwise makes steal rounds: the global queue,
 * then every other worker once, starting at an offset from a per-worker seeded generator.
 *
 * A worker that finds nothing for \ref TaskSchedulerOptions::idleSpinRounds rounds sleeps on its
 * parker: it drains its deque into the global queue, sets the parker's waiting bit, stores asleep
 * in its state word and decrements the awake count, then re-checks the stop flag and the global
 * count before it waits. A thread that pushes onto the global queue increments the count, then
 * reads the awake count and, if no worker is awake, wakes an asleep worker. Both sides use
 * sequentially consistent operations, so either the sleeper sees the work or the publisher sees no
 * worker awake: whenever a task is queued, at least one worker is awake.
 *
 * Work reaches the scheduler through a \ref TaskScope, which counts its outstanding tasks: the
 * thread that finishes a task decrements its scope's count and, on reaching zero, changes the
 * parker the scope recorded, waking the scope's creator.
 *
 * With zero workers every spawn runs inline on the spawning thread.
 */
class TaskScheduler {
public:
  /**
   * Starts the workers.
   *
   * @param options The scheduler's options.
   */
  explicit TaskScheduler(TaskSchedulerOptions options);

  TaskScheduler(const TaskScheduler&) = delete;
  TaskScheduler(TaskScheduler&&) = delete;
  TaskScheduler& operator=(const TaskScheduler&) = delete;
  TaskScheduler& operator=(TaskScheduler&&) = delete;

  /// Sets the stop flag, wakes and joins every worker, then closes the parker pool. Fails a release
  /// assertion if a \ref TaskScope is live, if any task is still queued, or if the awake count
  /// disagrees with the workers' state words.
  ~TaskScheduler();

  /// The workers started: every one requested, since a thread that fails to start ends the
  /// process (constructing a `std::thread` without exceptions terminates on failure).
  [[nodiscard]] std::size_t workerCount() const { return workers_.size(); }

  /// True when the options asked for zero workers.
  [[nodiscard]] bool threadingDisabledByConfiguration() const { return options_.workerCount == 0; }

private:
  friend class TaskSchedulerTestPeer;
  friend class TaskScope;

  /// What a task's finisher touches: its scope's counter and the parker the scope recorded.
  struct ScopeLink {
    std::atomic<std::uint64_t>* counter;
    Parker<>* parker;
  };

  /**
   * Starts the workers, with a test seam called at each protocol step. Reached only through the
   * test peer.
   *
   * @param options The scheduler's options.
   * @param testHooks Called at each \ref SchedulerStep; must outlive the scheduler.
   */
  TaskScheduler(TaskSchedulerOptions options, SchedulerTestHooks* testHooks);

  /// A worker's state: its deque, record pool and parker, its state word, and its thread.
  struct alignas(kFalseSharingBytes) Worker {
    Worker(std::size_t index, const TaskSchedulerOptions& options);

    const std::size_t index;
    WorkStealingDeque<TaskRecord*> deque;
    CreatorRecordPool<> pool;
    Parker<> parker;
    alignas(kFalseSharingBytes) std::atomic<WorkerState> state{WorkerState::Awake};
    std::thread thread;

    // Owner-only state.
    std::uint32_t stealRng;                 //!< Seeded generator for the steal round's offset.
    std::size_t localRunsSinceOutside = 0;  //!< Tasks run from the own deque since the last
                                            //!< outside take.
    std::size_t outsideCursor = 0;          //!< Rotating cursor into the outside sources.
  };

  /**
   * Spawns \p work as a task, or returns false and leaves \p work untouched: with zero workers,
   * when the spawning thread's record pool is at its bound, or when the spawning worker's deque is
   * full. \p work is copied or moved into the record, as it was passed, only once the spawn cannot
   * fail.
   *
   * @param work The task's closure.
   * @param scope The scope the task counts in, or null for a task no scope accounts for (tests
   *   only). Its count is incremented before the task is pushed, so the task's decrement follows.
   */
  template <typename F>
  [[nodiscard]] bool trySpawn(F&& work, const ScopeLink* scope = nullptr) {
    if (workers_.empty()) {
      return false;
    }
    if (Worker* worker = currentWorker()) {
      TaskRecord* record = worker->pool.tryAllocate();
      if (record == nullptr) {
        return false;
      }
      if (worker->deque.full()) {
        worker->pool.releaseFromCreator(record);
        return false;
      }
      prepareRecord(record, std::forward<F>(work), scope);
      const bool pushed = worker->deque.push(record);
      UTILS_RELEASE_ASSERT_MSG(pushed, "A worker deque that was not full rejected a push");
      return true;
    }
    TaskRecord* record = allocateExternalRecord();
    if (record == nullptr) {
      return false;
    }
    prepareRecord(record, std::forward<F>(work), scope);
    publishToGlobalQueue(record);
    return true;
  }

  /// Spawns \p work as a task, or runs it inline on this thread when \ref trySpawn cannot.
  template <typename F>
  void spawn(F&& work, const ScopeLink* scope = nullptr) {
    // A declined trySpawn leaves the closure untouched, so it is still there to run.
    if (!trySpawn(std::forward<F>(work), scope)) {
      work();  // NOLINT(bugprone-use-after-move): not moved when declined.
    }
  }

  /// Stores \p work in \p record and links it to \p scope, counting it there.
  template <typename F>
  static void prepareRecord(TaskRecord* record, F&& work, const ScopeLink* scope) {
    record->setClosure(std::forward<F>(work));
    if (scope != nullptr) {
      record->setScope(scope->counter, scope->parker);
      // Relaxed: the push that publishes the task orders this before the task's decrement.
      scope->counter->fetch_add(1, std::memory_order_relaxed);
    }
  }

  /// Decrements \p counter for a finished task and, on reaching zero, changes \p parker.
  void finishScopeTask(std::atomic<std::uint64_t>* counter, Parker<>* parker,
                       std::size_t workerIndex);

  /// The calling thread's worker of this scheduler, or null.
  Worker* currentWorker() const;

  /// Allocates a record from the external pool. @return Null at its bound.
  TaskRecord* allocateExternalRecord();

  /// Links \p record into the global queue, then wakes a worker if none is awake.
  void publishToGlobalQueue(TaskRecord* record);

  /// Links \p record into the global queue under the mutex.
  void pushGlobal(TaskRecord* record);

  /// Takes the oldest global entry. @return Null if the queue is empty.
  TaskRecord* takeGlobal();

  /// Takes from the outside sources from the worker's cursor. @return Null if all are empty.
  TaskRecord* takeOutside(Worker& worker);

  /// The next task for an awake worker in its idle loop. @return Null if a steal round found none.
  TaskRecord* findTask(Worker& worker);

  /// Probes every other worker once from a seeded offset. @return Null if none had work.
  TaskRecord* stealFromWorkers(Worker& worker);

  /**
   * The victim at position \p k of a steal round by worker \p self among \p workers workers,
   * starting at \p offset: every other worker exactly once, in index order from the offset.
   */
  static std::size_t StealVictim(std::size_t self, std::size_t workers, std::size_t offset,
                                 std::size_t k) {
    const std::size_t others = workers - 1;
    return (self + 1 + (offset + k) % others) % workers;
  }

  /// Runs \p record and returns it to its pool.
  void runTask(Worker& worker, TaskRecord* record);

  /// Sleep steps 1 to 5. @return False when the stop flag is set and the worker must exit.
  bool trySleep(Worker& worker);

  /// Whether queued work is visible to a sleeper's re-check.
  bool queuedWorkVisible() const;

  /// Swaps \p worker from asleep to awake, incrementing the awake count only if the swap
  /// succeeds. @return True if this call's swap succeeded.
  bool swapToAwake(Worker& worker);

  /// The swap rule for a thread waking another worker: the swap, then a parker change.
  void wakeWorker(Worker& worker);

  /// Wakes one asleep worker, preferring one asleep shallow.
  void wakeAsleepWorker();

  /// The idle loop of one worker.
  void workerMain(Worker& worker);

  void hook(SchedulerStep step, std::size_t worker) {
    if (testHooks_ != nullptr) {
      testHooks_->at(step, worker);
    }
  }

  const TaskSchedulerOptions options_;
  SchedulerTestHooks* const testHooks_;

  /// Unique for the life of the process, so a thread's parker entry is never confused with that of
  /// a destroyed scheduler at the same address.
  const std::uint64_t generation_;

  /// Parkers for non-worker scope creators; closed after the workers are joined.
  const std::shared_ptr<ParkerPool> parkerPool_;

  /// Live \ref TaskScope objects. Relaxed: the destructor's check needs only the happens-before
  /// that a correct program already has between a scope's destruction and the scheduler's.
  std::atomic<std::size_t> liveScopes_{0};

  /// Protects the global queue's list and the external record pool. On its own cache line, away
  /// from the read-mostly options and hooks pointer, as the deque separates its indices.
  alignas(kFalseSharingBytes) std::mutex globalMutex_;
  TaskRecord* globalHead_ = nullptr;
  TaskRecord* globalTail_ = nullptr;
  ExternalRecordPool externalPool_;

  alignas(kFalseSharingBytes) std::atomic<std::int64_t> globalQueued_{0};
  alignas(kFalseSharingBytes) std::atomic<std::int64_t> awake_{0};
  alignas(kFalseSharingBytes) std::atomic<bool> stop_{false};

  std::vector<std::unique_ptr<Worker>> workers_;
};

}  // namespace donner::tasks
