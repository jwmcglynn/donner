#pragma once
/// @file
/// A test-owned root coroutine. Normal Tasks are started through co_await.

#include <coroutine>
#include <cstdlib>

#include "donner/base/Utils.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace donner::tasks {

class CoroutineTestRoot {
public:
  struct promise_type {
    CoroutineTestRoot get_return_object() {
      return CoroutineTestRoot(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    std::suspend_always initial_suspend() const noexcept { return {}; }

    struct FinalAwaiter {
      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<promise_type> handle) const noexcept {
        Event* done = handle.promise().done;
        done->set();
      }

      void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    [[noreturn]] void unhandled_exception() const noexcept { std::abort(); }
    Event* done = nullptr;
  };

  CoroutineTestRoot(const CoroutineTestRoot&) = delete;
  CoroutineTestRoot& operator=(const CoroutineTestRoot&) = delete;

  ~CoroutineTestRoot() { handle_.destroy(); }

  /// Starts the coroutine on the caller's thread. Call once, then wait on p done before
  /// destroying the root.
  void start(Event& done) {
    handle_.promise().done = &done;
    handle_.resume();
  }

private:
  explicit CoroutineTestRoot(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
  std::coroutine_handle<promise_type> handle_;
};

}  // namespace donner::tasks
