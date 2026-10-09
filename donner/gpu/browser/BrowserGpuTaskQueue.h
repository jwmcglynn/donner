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
    BrowserGpuTask* task = head_;
    head_ = nullptr;
    tail_ = nullptr;
    std::size_t count = 0;
    while (task) {
      BrowserGpuTask* next = task->next;
      task->next = nullptr;
      task->callback(task->context);
      task = next;
      ++count;
    }
    return count;
  }

  /// Whether every queued callback has been handed off for execution.
  bool empty() const { return head_ == nullptr; }

private:
  BrowserGpuTask* head_ = nullptr;
  BrowserGpuTask* tail_ = nullptr;
};

}  // namespace donner::gpu::browser
