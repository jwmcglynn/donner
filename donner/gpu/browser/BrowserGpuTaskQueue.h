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
    return executeBatchWhile([](std::size_t) { return true; });
  }

  /// Execute a prefix of the detached batch while the caller admits another callback.
  /// Unstarted entries stay ahead of any work appended reentrantly.
  /// @param admit Receives the number of callbacks already started in this batch.
  template <typename Predicate>
  std::size_t executeBatchWhile(Predicate admit) {
    BrowserGpuTask* task = head_;
    BrowserGpuTask* detachedTail = tail_;
    head_ = nullptr;
    tail_ = nullptr;
    std::size_t count = 0;
    while (task && admit(count)) {
      BrowserGpuTask* next = task->next;
      task->next = nullptr;
      task->callback(task->context);
      task = next;
      ++count;
    }
    if (task) {
      detachedTail->next = head_;
      head_ = task;
      if (!tail_) {
        tail_ = detachedTail;
      }
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
