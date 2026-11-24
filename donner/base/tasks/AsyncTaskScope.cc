#include "donner/base/tasks/AsyncTaskScope.h"

namespace donner::tasks {

AsyncTaskScope::AsyncTaskScope(TaskScheduler& scheduler)
    : scheduler_(scheduler), link_{&count_, nullptr, this} {
  scheduler_.liveScopes_.fetch_add(1, std::memory_order_relaxed);
}

AsyncTaskScope::~AsyncTaskScope() {
  UTILS_RELEASE_ASSERT_MSG(
      joinCompleted_ && (count_.load(std::memory_order_relaxed) & kCountMask) == 0,
      "An AsyncTaskScope must be joined before destruction");
  scheduler_.liveScopes_.fetch_sub(1, std::memory_order_relaxed);
}

void AsyncTaskScope::assertCanSpawn() const {
  // During registration or suspension, a running scoped task still has a record counted.
  UTILS_RELEASE_ASSERT_MSG(!joinStarted_.load(std::memory_order_relaxed) ||
                               (count_.load(std::memory_order_relaxed) & kCountMask) != 0,
                           "An AsyncTaskScope cannot be reopened after join");
}

bool AsyncTaskScope::JoinAwaitable::await_suspend(std::coroutine_handle<> handle) noexcept {
  UTILS_RELEASE_ASSERT_MSG(!scope_.joinStarted_.exchange(true, std::memory_order_relaxed),
                           "An AsyncTaskScope may be joined only once");
  TaskScheduler& scheduler = scope_.scheduler_;
  SchedulerTestHooks* const hooks = scheduler.testHooks_;
  resumption_.handle = handle;
  scope_.waiting_ = &resumption_;

  std::uint64_t observed = scope_.count_.load(std::memory_order_acquire);
  if ((observed & kCountMask) != 0) {
    scheduler.hook(SchedulerStep::AsyncJoinBeforeRegister, 0);
  }
  while ((observed & kCountMask) != 0) {
    if (scope_.count_.compare_exchange_strong(
            observed, observed | kJoinFlag, std::memory_order_acq_rel, std::memory_order_acquire)) {
      // The finisher may now resume and destroy the awaiting frame.
      if (hooks != nullptr) {
        hooks->at(SchedulerStep::AsyncJoinRegistered, 0);
      }
      return true;
    }
  }
  return false;
}

void AsyncTaskScope::finish(std::size_t workerIndex) {
  TaskScheduler& scheduler = scheduler_;
  const std::uint64_t before = count_.fetch_sub(1, std::memory_order_acq_rel);
  UTILS_RELEASE_ASSERT_MSG((before & kCountMask) != 0, "A scope's task count went below zero");
  if ((before & kCountMask) == 1) {
    scheduler.hook(SchedulerStep::AsyncJoinFinisherAfterDecrement, workerIndex);
  }
  if (before == (kJoinFlag | 1)) {
    // The join flag publishes the pointer and frame contents. The scope is still suspended.
    CoroutineResumption& resumption = *waiting_;
    scheduler.hook(SchedulerStep::AsyncJoinBeforeEnqueue, workerIndex);
    scheduler.enqueueResumption(resumption);
  }
}

}  // namespace donner::tasks
