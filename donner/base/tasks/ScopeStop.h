#pragma once
/// @file
/// A scope's cooperative stop request, and the check a queued task of the scope makes before it
/// runs.

#include <atomic>

namespace donner::tasks {

/**
 * A scope's stop request: cooperative cancellation. Once requested, a task of the scope that is
 * still queued is not run when a thread takes it: its closure is destroyed unrun, and it still
 * completes its scope's accounting, so the scope's join returns. A task that has started runs to
 * its end and may poll \ref requested to return early. Spawns that run inline, because the
 * scheduler could not queue them, are not queued and are not skipped.
 *
 * Every access is relaxed: the request is advisory, publishes no data, and a task that misses a
 * request made concurrently with its start simply runs.
 *
 * A scope type embeds one and gives its address to each task record it spawns; the scheduler
 * makes the check, \ref SkipsQueuedTask, wherever it starts a queued task, so every scope type
 * that links its flag gets the same behavior.
 */
class ScopeStop {
public:
  ScopeStop() = default;

  ScopeStop(const ScopeStop&) = delete;
  ScopeStop(ScopeStop&&) = delete;
  ScopeStop& operator=(const ScopeStop&) = delete;
  ScopeStop& operator=(ScopeStop&&) = delete;

  /// Requests the stop. From any thread, any number of times.
  void request() { requested_.store(true, std::memory_order_relaxed); }

  /// Whether a stop was requested.
  [[nodiscard]] bool requested() const { return requested_.load(std::memory_order_relaxed); }

  /**
   * The check a thread makes before it starts a queued task: true when the task's scope requested
   * a stop, and the task must be destroyed unrun.
   *
   * @param stop The task's scope's stop request, or null for a task without one.
   */
  [[nodiscard]] static bool SkipsQueuedTask(const ScopeStop* stop) {
    return stop != nullptr && stop->requested();
  }

private:
  std::atomic<bool> requested_{false};
};

}  // namespace donner::tasks
