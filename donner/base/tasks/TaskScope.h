#pragma once
/// @file
/// Synchronous scopes, joined on the thread that created them, and `Join`.

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

#include "donner/base/tasks/Parker.h"
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
 * In this version the creator is a thread that is not one of the scheduler's workers: it never
 * runs tasks. Its spawns go to the scheduler's global queue, and its wait re-checks the count and
 * sleeps on its parker, from the scheduler's \ref ParkerPool, until a finisher changes it. A scope
 * created on a worker fails a release assertion. With zero workers, every spawn runs inline and a
 * wait returns at once.
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
   * Returns once every task spawned into the scope has finished, sleeping on the creator's parker
   * meanwhile; on return, everything those tasks wrote is visible. On the creating thread only.
   */
  void wait();

private:
  friend class TaskScopeTestPeer;

  TaskScheduler& scheduler_;
  const std::thread::id creator_;

  /// Tasks spawned and not yet finished.
  std::atomic<std::uint64_t> count_{0};

  /// The counter and the creator's parker, which every task record of the scope carries. The
  /// parker is null with zero workers, when no task is ever queued.
  TaskScheduler::ScopeLink link_;
};

/**
 * Runs \p a and \p b, possibly in parallel, and returns when both have finished.
 *
 * On a thread that is not a worker, \p b is spawned to the scheduler's global queue, \p a runs on
 * the calling thread, and the call then waits for \p b as a \ref TaskScope does. With zero workers,
 * \p a runs and then \p b, both on the calling thread. Calling it on a worker fails a release
 * assertion in this version.
 *
 * @param scheduler The scheduler that runs \p b.
 * @param a Runs on the calling thread.
 * @param b Runs on a worker, or inline when the scheduler cannot queue it.
 */
template <typename A, typename B>
void Join(TaskScheduler& scheduler, A&& a, B&& b) {
  if (scheduler.workerCount() == 0) {
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
