#pragma once
/// @file
/// Synchronous scopes, joined on the thread that created them, and `Join`.

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

#include "donner/base/tasks/Parker.h"
#include "donner/base/tasks/ScopeStop.h"
#include "donner/base/tasks/TaskScheduler.h"

namespace donner::tasks {

class TaskScopeTestPeer;

/**
 * Counts the tasks spawned into it, and is joined, by \ref wait or the destructor, on the thread
 * that created it.
 *
 * The scope records its creating thread and that thread's parker when it is constructed. A task
 * spawned into it, by the creator or by any task of the scope, carries the scope's counter and the
 * recorded parker in its task record; the thread that finishes the task destroys its closure,
 * returns its record, then decrements the counter, and the finisher whose decrement reaches zero
 * changes the parker. That decrement is the finisher's last access to the scope, and the parker
 * belongs to the scheduler, so the creator may destroy the scope as soon as it sees the count at
 * zero.
 *
 * On a worker, the scope also records the worker's deque bottom index, its watermark: every entry
 * the creator, or a task run on top of its wait, pushes from then on lies at or above it. The
 * creator's spawns go to its own deque, and its wait runs entries above the watermark, returns once
 * the count is zero, and otherwise helps with other work while it has more than
 * \ref TaskSchedulerOptions::helpMarginBytes of stack left, then sleeps on the worker's parker (see
 * \ref TaskScheduler). Nested scopes must be joined in last-in, first-out order. A coroutine
 * must not suspend while a synchronous scope is live.
 *
 * On a thread that is not a worker, the creator never runs tasks. Its spawns go to the scheduler's
 * global queue, and its wait re-checks the count and sleeps on its parker, from the scheduler's
 * \ref ParkerPool, until a finisher changes it. Without a seed and with zero workers, every spawn
 * runs inline and a wait returns at once.
 *
 * On a seeded scheduler the creator queues tasks in the replay pool and runs them during its wait.
 *
 * \ref requestStop asks the scope's queued tasks not to run: each is destroyed unrun when a thread
 * takes it, and still completes its scope's accounting, so the wait returns. Running tasks may
 * poll \ref stopRequested.
 *
 * Creating a scope while the calling thread's own thread-local objects are being destroyed (from
 * another thread-local's destructor, at thread exit) is undefined: the thread's parker entry is a
 * thread-local object and may already be gone.
 */
class TaskScope {
public:
  /**
   * Records the calling thread as the scope's creator, and its parker.
   *
   * @param scheduler The scheduler that runs the scope's tasks; must outlive the scope.
   */
  explicit TaskScope(TaskScheduler& scheduler);

  TaskScope(const TaskScope&) = delete;
  TaskScope(TaskScope&&) = delete;
  TaskScope& operator=(const TaskScope&) = delete;
  TaskScope& operator=(TaskScope&&) = delete;

  /// Joins: returns once every task spawned into the scope has finished. On the creating thread.
  ~TaskScope();

  /**
   * Spawns \p work as a task of this scope, or runs it inline on the calling thread when the
   * scheduler cannot queue it (zero workers, a record bound, a full deque). From the creator or
   * from any task of this scope.
   *
   * @param work The task's closure, copied or moved into the task as it is passed.
   */
  template <typename F>
  void spawn(F&& work) {
    scheduler_.spawn(std::forward<F>(work), &link_);
  }

  /**
   * Spawns \p work as a task of this scope, or returns false and leaves \p work with the caller
   * when the scheduler cannot queue it. From the creator or from any task of this scope.
   *
   * @param work The task's closure, copied or moved into the task as it is passed, and only when
   *   the spawn succeeds.
   */
  template <typename F>
  [[nodiscard]] bool trySpawn(F&& work) {
    return scheduler_.trySpawn(std::forward<F>(work), &link_);
  }

  /**
   * Returns once every task spawned into the scope has finished; on return, everything those tasks
   * wrote is visible. On the creating thread only. On a worker it runs and helps with work
   * meanwhile; on another thread it sleeps on the creator's parker.
   */
  void wait();

  /**
   * Requests cooperative cancellation from any thread. A queued task that observes the request
   * discards its closure and counts as finished. Inline fallback work still runs.
   */
  void requestStop() { stop_.request(); }

  /// Whether \ref requestStop was called; tasks that run long poll it.
  [[nodiscard]] bool stopRequested() const { return stop_.requested(); }

private:
  friend class TaskScopeTestPeer;

  TaskScheduler& scheduler_;
  const std::thread::id creator_;

  /// The creating worker, or null when the creator is not a worker of the scheduler.
  TaskScheduler::Worker* const worker_;

  /// The creating worker's deque bottom index when the scope was created; unused off a worker.
  const WorkStealingDeque<SchedulerWork*>::Index watermark_;

  /// Tasks spawned and not yet finished.
  std::atomic<std::uint64_t> count_{0};

  /// The scope's stop request, checked before each queued task of the scope runs.
  ScopeStop stop_;

  /// The counter, the creator's parker and the stop request, which every task record of the
  /// scope carries. The parker is null when the creator runs inline or drains its own seeded queue.
  TaskScheduler::ScopeLink link_;
};

/**
 * Runs \p a and \p b, possibly in parallel, and returns when both have finished.
 *
 * On a worker, \p b is pushed to the worker's deque and \p a runs on the calling thread; the wait
 * then takes \p b back and runs it if no other worker stole it, and otherwise waits for it as a
 * \ref TaskScope's creator does. On any other thread, \p b is spawned to the scheduler's global
 * queue, \p a runs on the calling thread, and the call then waits for \p b as a \ref TaskScope
 * does; it cannot take \p b back. With zero workers, \p a runs and then \p b, both on the calling
 * thread.
 *
 * @param scheduler The scheduler that runs \p b.
 * @param a Runs on the calling thread.
 * @param b Runs on a worker, or inline when the scheduler cannot queue it.
 */
template <typename A, typename B>
void Join(TaskScheduler& scheduler, A&& a, B&& b) {
  if (detail::ParallelForAccess::Concurrency(scheduler) == 0) {
    a();
    b();
    return;
  }
  TaskScope scope(scheduler);
  scope.spawn(std::forward<B>(b));
  a();
  scope.wait();
}

}  // namespace donner::tasks
