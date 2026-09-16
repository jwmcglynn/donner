#pragma once
/// @file
///
/// Shared polling/wait helpers for the async-renderer tests (see issue
/// #1218). Every wait is the same loop: poll, stop when it yields, sleep a
/// millisecond, give up at a bound. Only the bound and the predicate differ.

#include <chrono>
#include <optional>
#include <thread>

namespace donner::editor::tests {

/// Interval between poll iterations. Every wait in the async suites rounds up
/// to this quantum.
constexpr auto kPollInterval = std::chrono::milliseconds(1);

// Polls until `poll()` yields a value, or `maxPolls` intervals elapse.
//
// Iteration-bounded rather than clock-bounded on purpose. The callers this
// replaced counted iterations, and each iteration also spends the poll's own
// time, so a wall-clock deadline of `maxPolls * kPollInterval` would be
// strictly TIGHTER than what they had - a silent timeout reduction is exactly
// the kind of change that turns into a flake on a loaded runner months later.
template <typename PollFn>
auto PollForResult(PollFn&& poll, int maxPolls) -> decltype(poll()) {
  for (int i = 0; i < maxPolls; ++i) {
    if (auto result = poll(); result.has_value()) {
      return result;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  return std::nullopt;
}

// Polls until `poll()` yields a value, or `deadline` passes.
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

// Waits until `isDone()` holds or `deadline` passes, without polling anything.
// Returns whether it finished rather than timed out. Evaluates `isDone()` one
// final time after the deadline so a predicate that flips on the last instant
// still reports success instead of flaking.
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

// Runs `poll()` for its side effects until `isDone()` holds or `deadline`
// passes. Returns whether it finished rather than timed out.
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

}  // namespace donner::editor::tests
