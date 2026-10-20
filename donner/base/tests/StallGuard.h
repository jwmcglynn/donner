#pragma once
/// @file
///
/// Stall guards for tests that wait on another thread, a worker or the GPU.
///
/// A correctness test asserts ordering and causality: which result arrived, which wake fired,
/// which frame a serial belongs to. How long the work took is not part of that contract. Each wait
/// here polls until its condition holds, and gives up only at a generous hang guard so a real hang
/// fails the case with a message. Latency belongs in a performance test.
///
/// Bounds, from inner to outer:
/// - \ref donner::tests::kHangGuard bounds one wait.
/// - The per-case watchdog in `//donner/base:gtest_timeout_main` (60 s by default) bounds a whole
///   case and names it when it fires.
/// - Bazel's test timeout bounds the whole target: 60 s for a `small` test.

#include <chrono>
#include <optional>
#include <thread>
#include <utility>

namespace donner::tests {

/// Interval between poll iterations. Every polled wait rounds up to this quantum.
constexpr auto kPollInterval = std::chrono::milliseconds(1);

/// How long a wait for real work, such as a render completing, may run before the test gives up.
///
/// This is a hang guard, not a latency budget. A test that checks ordering, for example that a drag
/// frame reaches the presenter while the pointer is still held, pumps frames until its condition
/// holds and waits on the renderer's completion; how fast that happens depends on the build and the
/// host. Sanitizer builds and software rasterizers take many times longer than an ordinary build,
/// so a tight wall-clock deadline there fails while the renderer is still making progress. The
/// guard is long enough never to bind on progress and short enough that a real hang fails the case
/// with a message instead of running into the test runner's timeout. Latency belongs in a
/// performance test.
///
/// It must stay well below the 60 s outer bounds above, or a hang reaches the per-case watchdog or
/// Bazel's timeout first and fails with no message. 30 s leaves the case that owns the wait as much
/// time again for its setup and earlier waits.
constexpr auto kHangGuard = std::chrono::seconds(30);

/// A `kHangGuard` window that starts when the guard is constructed. Pump a wait loop while
/// `active()`, and pass `deadline()` to any blocking wait inside it, so the whole loop shares one
/// window.
class HangGuard {
public:
  HangGuard() : deadline_(std::chrono::steady_clock::now() + kHangGuard) {}

  /// Whether the window is still open. @return False once `kHangGuard` has elapsed.
  [[nodiscard]] bool active() const { return std::chrono::steady_clock::now() < deadline_; }

  /// The instant the window closes. @return The deadline.
  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const { return deadline_; }

private:
  std::chrono::steady_clock::time_point deadline_;
};

/**
 * Polls until `poll()` yields a value, or `deadline` passes.
 *
 * @param poll Returns an engaged optional once the awaited value exists.
 * @param deadline When to give up.
 * @return The first engaged result, or an empty optional if the deadline passed first.
 */
template <typename PollFn>
auto PollForResult(PollFn&& poll, std::chrono::steady_clock::time_point deadline)
    -> decltype(poll()) {
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto result = poll(); result.has_value()) {
      return result;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  return std::nullopt;
}

/**
 * Polls until `poll()` yields a value, under a fresh \ref HangGuard.
 *
 * @param poll Returns an engaged optional once the awaited value exists.
 * @return The first engaged result, or an empty optional if the guard expired first.
 */
template <typename PollFn>
auto PollForResult(PollFn&& poll) -> decltype(poll()) {
  return PollForResult(std::forward<PollFn>(poll), HangGuard().deadline());
}

/**
 * Waits until `isDone()` holds or `deadline` passes, without polling anything. Evaluates `isDone()`
 * one final time after the deadline so a predicate that flips on the last instant still reports
 * success.
 *
 * @param isDone The awaited condition.
 * @param deadline When to give up.
 * @return Whether the condition held.
 */
template <typename DoneFn>
bool WaitUntil(DoneFn&& isDone, std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    if (isDone()) {
      return true;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  return isDone();
}

/**
 * Waits until `isDone()` holds, under a fresh \ref HangGuard.
 *
 * @param isDone The awaited condition.
 * @return Whether the condition held before the guard expired.
 */
template <typename DoneFn>
bool WaitUntil(DoneFn&& isDone) {
  return WaitUntil(std::forward<DoneFn>(isDone), HangGuard().deadline());
}

/**
 * Runs `poll()` for its side effects until `isDone()` holds or `deadline` passes.
 *
 * @param poll Advances the system under test, for example by consuming a result.
 * @param isDone The awaited condition, checked after each poll.
 * @param deadline When to give up.
 * @return Whether the condition held.
 */
template <typename PollFn, typename DoneFn>
bool PollUntil(PollFn&& poll, DoneFn&& isDone, std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    poll();
    if (isDone()) {
      return true;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  return false;
}

/**
 * Runs `poll()` for its side effects until `isDone()` holds, under a fresh \ref HangGuard.
 *
 * @param poll Advances the system under test, for example by consuming a result.
 * @param isDone The awaited condition, checked after each poll.
 * @return Whether the condition held before the guard expired.
 */
template <typename PollFn, typename DoneFn>
bool PollUntil(PollFn&& poll, DoneFn&& isDone) {
  return PollUntil(std::forward<PollFn>(poll), std::forward<DoneFn>(isDone),
                   HangGuard().deadline());
}

}  // namespace donner::tests
