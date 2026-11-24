#pragma once
/// @file
/// A lazy, single-consumer coroutine with a scheduler-owned frame allocation.

#include <atomic>
#include <coroutine>
#include <cstdlib>
#include <optional>
#include <type_traits>
#include <utility>

#include "donner/base/Utils.h"
#include "donner/base/tasks/TaskScheduler.h"

namespace donner::tasks {

template <typename T>
class Task;

namespace detail {

class TaskPromiseBase {
public:
  template <typename... Args>
  explicit TaskPromiseBase(TaskScheduler& scheduler, Args&&...) noexcept
      : pool_(scheduler.framePool_) {
    // The promise is constructed even when the compiler elides the frame allocation.
    pool_.trackFrame();
  }

  template <typename Object, typename... Args>
    requires(!std::is_same_v<std::remove_cvref_t<Object>, TaskScheduler>)
  explicit TaskPromiseBase(Object&&, TaskScheduler& scheduler, Args&&...) noexcept
      : TaskPromiseBase(scheduler) {}

  ~TaskPromiseBase() { pool_.untrackFrame(); }
  // The allocation happens before the coroutine body can select a scheduler.
  template <typename... Args>
  static void* operator new(std::size_t bytes, TaskScheduler& scheduler, Args&&...) {
    return scheduler.framePool_.allocate(bytes);
  }

  // A member coroutine's implicit object precedes its explicit arguments.
  template <typename Object, typename... Args>
    requires(!std::is_same_v<std::remove_cvref_t<Object>, TaskScheduler>)
  static void* operator new(std::size_t bytes, Object&&, TaskScheduler& scheduler, Args&&...) {
    return scheduler.framePool_.allocate(bytes);
  }

  static void operator delete(void* frame, std::size_t bytes) noexcept {
    CoroutineFramePool::Release(frame, bytes);
  }

  std::suspend_always initial_suspend() const noexcept { return {}; }

  struct FinalAwaiter {
    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
      return handle.promise().continuation;
    }

    void await_resume() const noexcept {}
  };

  FinalAwaiter final_suspend() const noexcept { return {}; }
  [[noreturn]] void unhandled_exception() const noexcept { std::abort(); }

  enum class State { Lazy, Awaited, Consumed };
  std::atomic<State> state{State::Lazy};
  std::coroutine_handle<> continuation;

private:
  CoroutineFramePool& pool_;
};

template <typename T>
class TaskPromise : public TaskPromiseBase {
public:
  using TaskPromiseBase::TaskPromiseBase;

  Task<T> get_return_object() noexcept;

  template <typename U>
  void return_value(U&& value) {
    result.emplace(std::forward<U>(value));
  }

  std::optional<T> result;
};

template <>
class TaskPromise<void> : public TaskPromiseBase {
public:
  using TaskPromiseBase::TaskPromiseBase;

  Task<void> get_return_object() noexcept;
  void return_void() const noexcept {}
};

}  // namespace detail

/**
 * A lazily started coroutine that transfers directly to its single awaiter at completion.
 *
 * A coroutine returning this type takes a TaskScheduler& as its first explicit parameter.
 * Keep the owner alive through the await and the allocating scheduler alive until the owner
 * releases the frame, including after a completed await. Destroying an unstarted task releases
 * it without executing its body. Moving a task transfers ownership of its frame.
 * The caller serializes operations on the owner itself.
 *
 * @tparam T The result value or void.
 */
template <typename T>
class [[nodiscard]] Task {
public:
  using promise_type = detail::TaskPromise<T>;         //!< Coroutine promise.
  using Handle = std::coroutine_handle<promise_type>;  //!< Handle to the owned frame.

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  ~Task() { reset(); }

  /// Borrows the frame for its sole await.
  class Awaiter {
  public:
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) const noexcept {
      auto& promise = handle_.promise();
      using State = detail::TaskPromiseBase::State;
      State expected = State::Lazy;
      UTILS_RELEASE_ASSERT_MSG(promise.state.compare_exchange_strong(expected, State::Awaited,
                                                                     std::memory_order_relaxed),
                               "A Task may be awaited only once");
      promise.continuation = continuation;
      return handle_;
    }

    T await_resume() const {
      auto& promise = handle_.promise();
      struct ConsumeOnExit {
        detail::TaskPromiseBase& promise;
        ~ConsumeOnExit() {
          promise.state.store(detail::TaskPromiseBase::State::Consumed, std::memory_order_relaxed);
        }
      } consume{promise};
      if constexpr (!std::is_void_v<T>) {
        return std::move(*promise.result);
      }
    }

  private:
    friend class Task;
    explicit Awaiter(Handle handle) : handle_(handle) {}
    Handle handle_;
  };

  [[nodiscard]] Awaiter operator co_await() const noexcept {
    UTILS_RELEASE_ASSERT_MSG(handle_, "A moved-from Task cannot be awaited");
    return Awaiter(handle_);
  }

private:
  friend class detail::TaskPromise<T>;
  explicit Task(Handle handle) noexcept : handle_(handle) {}

  void reset() noexcept {
    if (handle_) {
      using State = detail::TaskPromiseBase::State;
      UTILS_RELEASE_ASSERT_MSG(
          handle_.promise().state.load(std::memory_order_relaxed) != State::Awaited,
          "A Task owner was destroyed before its await completed");
      handle_.destroy();
    }
  }

  Handle handle_;
};

static_assert(!std::is_copy_constructible_v<Task<void>> && !std::is_copy_assignable_v<Task<void>> &&
              std::is_nothrow_move_constructible_v<Task<void>> &&
              std::is_nothrow_move_assignable_v<Task<void>>);

template <typename T>
Task<T> detail::TaskPromise<T>::get_return_object() noexcept {
  return Task<T>(std::coroutine_handle<TaskPromise>::from_promise(*this));
}

inline Task<void> detail::TaskPromise<void>::get_return_object() noexcept {
  return Task<void>(std::coroutine_handle<TaskPromise>::from_promise(*this));
}

}  // namespace donner::tasks
