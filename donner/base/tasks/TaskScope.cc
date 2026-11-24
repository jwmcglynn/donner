#include "donner/base/tasks/TaskScope.h"

#include "donner/base/Utils.h"

namespace donner::tasks {

TaskScope::TaskScope(TaskScheduler& scheduler)
    : scheduler_(scheduler),
      creator_(std::this_thread::get_id()),
      worker_(scheduler.currentWorker()),
      watermark_(worker_ != nullptr ? worker_->deque.bottom() : 0),
      link_{.counter = &count_, .parker = nullptr, .stop = &stop_} {
  if (worker_ != nullptr) {
    link_.parker = &worker_->parker;
  } else if (scheduler.workerCount() != 0) {
    link_.parker = ParkerPool::ParkerForThread(scheduler.generation_, scheduler.parkerPool_);
  }
  scheduler.liveScopes_.fetch_add(1, std::memory_order_relaxed);
}

TaskScope::~TaskScope() {
  wait();
  scheduler_.liveScopes_.fetch_sub(1, std::memory_order_relaxed);
}

void TaskScope::wait() {
  UTILS_RELEASE_ASSERT_MSG(std::this_thread::get_id() == creator_,
                           "A TaskScope is joined on the thread that created it");
  if (worker_ != nullptr) {
    scheduler_.waitOnScope(*worker_, count_, watermark_);
    return;
  }
  // Acquire: on zero, everything the scope's tasks wrote is visible.
  while (count_.load(std::memory_order_acquire) != 0) {
    scheduler_.hook(SchedulerStep::ScopeWaiterBeforePrepareWait, 0);
    const Parker<>::Word token = link_.parker->prepareWait();
    // The re-check after the waiting bit: a finisher whose change came first has its zero seen
    // here; one whose change comes later finds the bit set and notifies.
    if (count_.load(std::memory_order_acquire) == 0) {
      link_.parker->finishWait();
      return;
    }
    scheduler_.hook(SchedulerStep::ScopeWaiterBeforeWait, 0);
    link_.parker->wait(token);
    link_.parker->finishWait();
  }
}

}  // namespace donner::tasks
