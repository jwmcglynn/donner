#pragma once
/// @file
/// Worker threads with an explicit stack size, and the bounds of the calling thread's stack.

#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace donner::tasks {

/// Stack kept free below every task a worker runs: a worker that starts a task with less than this
/// left fails a release assertion instead of overflowing. It covers the assertion's own reporting
/// and the sanitizer runtimes' use of the stack.
inline constexpr std::size_t kStackReserveBytes = 32 * 1024;

/**
 * The stack of one thread, which grows down on every platform the scheduler builds for: the
 * lowest usable address (the floor) and the size above it.
 */
struct StackBounds {
  std::uintptr_t floor = 0;  //!< Lowest usable address.
  std::size_t size = 0;      //!< Bytes from the floor to the top of the stack.
};

/**
 * The calling thread's stack bounds, as the platform reports them: `pthread_get_stackaddr_np` and
 * `pthread_get_stacksize_np` on Apple platforms, `pthread_getattr_np` elsewhere. Intended for
 * threads created with an explicit stack size; the main thread's bounds depend on the platform's
 * resource limits.
 */
StackBounds CurrentThreadStackBounds();

/**
 * The calling frame's position on the real stack. Under AddressSanitizer with its fake stack, the
 * addresses of locals can lie on the heap; the frame address never does.
 *
 * Unsupported on an alternate signal stack: in a handler installed with `SA_ONSTACK`, the frame is
 * on that stack, not the thread's, so a stack-left measurement taken there is meaningless.
 */
[[gnu::always_inline]] inline std::uintptr_t CurrentStackPosition() {
  return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
}

/**
 * Bytes of \p bounds' stack left below \p position: zero at or below the floor, never negative.
 *
 * @param position A position on the stack, from \ref CurrentStackPosition.
 * @param bounds The stack's bounds.
 */
constexpr std::size_t StackRemaining(std::uintptr_t position, const StackBounds& bounds) {
  return position > bounds.floor ? static_cast<std::size_t>(position - bounds.floor) : 0;
}

/**
 * The stack size a worker is created with for a requested \p bytes: rounded up to whole pages.
 * Fails a release assertion when \p bytes is below the platform's minimum thread stack or does not
 * leave room above \ref kStackReserveBytes.
 */
std::size_t WorkerStackSize(std::size_t bytes);

/**
 * The part of a thread's stack a worker uses: the top \p usableBytes of \p platform's bounds, or
 * all of them if the platform's stack is smaller. A platform can hand a new thread a larger stack
 * than it asked for (glibc reuses a cached stack up to four times the requested size), so the
 * configured size, not the platform's, is what a worker counts as its stack.
 *
 * @param platform The thread's bounds, from \ref CurrentThreadStackBounds.
 * @param usableBytes The configured stack size, from \ref WorkerStackSize.
 */
constexpr StackBounds UsableStack(const StackBounds& platform, std::size_t usableBytes) {
  const std::size_t size = platform.size < usableBytes ? platform.size : usableBytes;
  return StackBounds{platform.floor + platform.size - size, size};
}

/// Thread creation entry point used by the scheduler and its failure-injection tests.
using WorkerThreadCreate = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

/**
 * Attempts to start a thread with an explicit stack size. Invalid sizes fail a release assertion.
 *
 * @param bytes Requested stack size.
 * @param entry Thread entry point.
 * @param argument Argument passed to the entry point.
 * @param create Thread creation function.
 * @return A joinable handle, or empty when the platform refused to start the thread.
 */
std::optional<pthread_t> TryStartThreadWithStack(std::size_t bytes, void* (*entry)(void*),
                                                 void* argument, WorkerThreadCreate create);

/**
 * Starts a thread running `entry(argument)` on a stack of \ref WorkerStackSize(\p bytes) bytes.
 * Fails a release assertion if the thread cannot be created, as a `std::thread` built without
 * exceptions ends the process.
 *
 * @return The thread, to pass to `pthread_join`.
 */
pthread_t StartThreadWithStack(std::size_t bytes, void* (*entry)(void*), void* argument);

}  // namespace donner::tasks
