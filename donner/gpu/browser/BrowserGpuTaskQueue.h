#pragma once
/// @file
/// Owner-thread FIFO for borrowed GPU requests, executed one detached batch at a time.

#include <cstddef>

namespace donner::gpu::browser {

/// Intrusive task. Its caller keeps it alive until the callback completes.
struct BrowserGpuTask {
  void (*callback)(void*) = nullptr;  //!< Nonblocking callback, allowed to reclaim this task.
  void* context = nullptr;            //!< Borrowed callback context.
  BrowserGpuTask* next = nullptr;     //!< Owner-thread link; never read after invoking callback.
};

/// Accessed exclusively by the GPU owner. New work cannot extend an executing batch.
class BrowserGpuTaskQueue {
public:
  /// Append a live, currently unqueued task. @param task Borrowed until its callback runs.
  void enqueue(BrowserGpuTask& task) {
    task.next = nullptr;
    if (tail_) {
      tail_->next = &task;
    } else {
      head_ = &task;
    }
    tail_ = &task;
  }

  /// Execute the current batch in FIFO order, returning its size. Reentrant work stays queued.
  std::size_t executeBatch() {
    return executeBatchWhile([] { return true; });
  }

  /** Execute a prefix of the current batch in FIFO order, returning its size.
   * Tasks the predicate declines stay queued ahead of reentrant work.
   * @param mayStart Asked before each callback starts; a running callback is not preempted.
   */
  template <typename Predicate>
  std::size_t executeBatchWhile(Predicate&& mayStart) {
    BrowserGpuTask* task = head_;
    head_ = nullptr;
    tail_ = nullptr;
    std::size_t count = 0;
    while (task && mayStart()) {
      BrowserGpuTask* next = task->next;
      task->next = nullptr;
      task->callback(task->context);
      task = next;
      ++count;
    }
    if (task) {
      requeueFront(*task);
    }
    return count;
  }

  /// Whether every queued callback has been handed off for execution.
  bool empty() const { return head_ == nullptr; }

private:
  /// Restore an unexecuted chain ahead of any work enqueued while it was detached.
  void requeueFront(BrowserGpuTask& first) {
    BrowserGpuTask* last = &first;
    while (last->next) {
      last = last->next;
    }
    last->next = head_;
    if (!head_) {
      tail_ = last;
    }
    head_ = &first;
  }

  BrowserGpuTask* head_ = nullptr;
  BrowserGpuTask* tail_ = nullptr;
};

}  // namespace donner::gpu::browser
