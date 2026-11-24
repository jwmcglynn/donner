#pragma once
/// @file
/// A task scope joined once by its creating coroutine.

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <utility>

#include "donner/base/Utils.h"
#include "donner/base/tasks/TaskScheduler.h"

namespace donner::tasks {

/// A scope created, joined once, and destroyed by a coroutine. Its scheduler must outlive it.
/// Keep the coroutine frame alive until its join resumes.
class AsyncTaskScope {
public:
  /// @param scheduler Runs the scope's tasks and queues its continuation.
  explicit AsyncTaskScope(TaskScheduler& scheduler);
  ~AsyncTaskScope();

  AsyncTaskScope(const AsyncTaskScope&) = delete;
  AsyncTaskScope(AsyncTaskScope&&) = delete;
  AsyncTaskScope& operator=(const AsyncTaskScope&) = delete;
  AsyncTaskScope& operator=(AsyncTaskScope&&) = delete;

  /// Spawns from the creator before join, or from a task of this scope while it is still running.
  /// Runs inline if the scheduler has no capacity.
  template <typename F>
  void spawn(F&& work) {
    assertCanSpawn();
    scheduler_.spawn(std::forward<F>(work), &link_);
  }

  /// Returns false and leaves the callable with the caller at a bound.
  template <typename F>
  [[nodiscard]] bool trySpawn(F&& work) {
    assertCanSpawn();
    return scheduler_.trySpawn(std::forward<F>(work), &link_);
  }

  /// The waiting coroutine owns its queue entry until the scheduler resumes it.
  class JoinAwaitable {
  public:
    JoinAwaitable(const JoinAwaitable&) = delete;
    JoinAwaitable& operator=(const JoinAwaitable&) = delete;

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle) noexcept;
    void await_resume() const noexcept { scope_.joinCompleted_ = true; }

  private:
    friend class AsyncTaskScope;
    explicit JoinAwaitable(AsyncTaskScope& scope) : scope_(scope) {}

    AsyncTaskScope& scope_;
    CoroutineResumption resumption_;
  };

  /// Joins after outstanding work finishes. An empty scope completes synchronously.
  [[nodiscard]] JoinAwaitable join() noexcept { return JoinAwaitable(*this); }

private:
  friend class TaskScheduler;
  friend class AsyncTaskScopeTestPeer;

  static constexpr std::uint64_t kJoinFlag = std::uint64_t{1} << 63;
  static constexpr std::uint64_t kCountMask = kJoinFlag - 1;

  void assertCanSpawn() const;
  void finish(std::size_t workerIndex);

  TaskScheduler& scheduler_;
  std::atomic<std::uint64_t> count_{0};
  const TaskScheduler::ScopeLink link_;
  std::atomic<bool> joinStarted_{false};
  bool joinCompleted_ = false;  //!< Set by the creating coroutine when join returns.
  CoroutineResumption* waiting_ = nullptr;
};

}  // namespace donner::tasks
