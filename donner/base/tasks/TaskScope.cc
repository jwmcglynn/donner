#include "donner/base/tasks/TaskScope.h"

#include "donner/base/Utils.h"

namespace donner::tasks {

TaskScope::TaskScope(TaskScheduler& scheduler)
    : scheduler_(scheduler),
      creator_(std::this_thread::get_id()),
      worker_(scheduler.currentWorker()),
      adopted_(worker_ == nullptr ? scheduler.currentAdoptedSlot() : nullptr),
      watermark_(worker_ != nullptr ? worker_->deque.bottom()
                                    : (adopted_ ? adopted_->deque.bottom() : 0)),
      link_{.counter = &count_, .parker = nullptr, .stop = &stop_} {
  scheduler.assertSeededOwner();
  if (worker_ != nullptr) {
    link_.parker = &worker_->parker;
  } else if (adopted_ != nullptr) {
    link_.parker = &adopted_->parker;
  } else if (scheduler.workerCount() != 0) {
    link_.parker = ParkerPool::ParkerForThread(scheduler.generation_, scheduler.parkerPool_);
  }
  if (worker_ == nullptr) {
    ++TaskScheduler::threadScopeCount_;
    if (TaskScheduler::adoptedOnThread_ != nullptr) {
      TaskScheduler::recordAdoptedBirth(link_);
      previousScope_ = TaskScheduler::innermostOnAdoptedThread_;
      TaskScheduler::innermostOnAdoptedThread_ = &link_;
    }
  }
  scheduler.liveScopes_.fetch_add(1, std::memory_order_relaxed);
}

TaskScope::~TaskScope() {
  wait();
  if (link_.birthOrder != 0) {
    TaskScheduler::innermostOnAdoptedThread_ = previousScope_;
  }
  if (worker_ == nullptr) {
    --TaskScheduler::threadScopeCount_;
  }
  scheduler_.liveScopes_.fetch_sub(1, std::memory_order_relaxed);
}

void TaskScope::wait() {
  UTILS_RELEASE_ASSERT_MSG(std::this_thread::get_id() == creator_,
                           "A TaskScope is joined on the thread that created it");
  UTILS_RELEASE_ASSERT_MSG(
      link_.birthOrder == 0 || TaskScheduler::innermostOnAdoptedThread_ == &link_,
      "An adopted thread must join its innermost synchronous scope first");
  if (scheduler_.seeded_) {
    scheduler_.drainSeededUntil(count_);
    return;
  }
  if (worker_ != nullptr) {
    scheduler_.waitOnScope(*worker_, count_, watermark_);
    return;
  }
  for (;;) {
    if (adopted_ && adopted_->deque.bottom() > watermark_) {
      if (const auto record = adopted_->deque.take()) {
        UTILS_RELEASE_ASSERT_MSG(
            StackRemaining(CurrentStackPosition(), adopted_->stack) >= kStackReserveBytes,
            "An adopted scheduler thread is out of stack");
        scheduler_.runRecord(adopted_->pool, 0, *record);
        continue;
      }
    }
    if (count_.load(std::memory_order_acquire) == 0) {
      return;
    }
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
