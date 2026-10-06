#pragma once
/// @file
/// gmock matchers and fixtures for \c donner::gpu model tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <thread>
#include <utility>

#include "donner/gpu/GpuResult.h"

namespace donner::gpu {

/**
 * Matches a \ref Result that contains a value (no error). On mismatch prints the contained
 * error.
 */
MATCHER(HasResult, "has a result") {
  if (arg.hasError()) {
    *result_listener << "which has error: " << arg.error();
    return false;
  }
  return arg.hasResult();
}

/**
 * Matches a successful \ref Status. On mismatch prints the contained error.
 */
MATCHER(IsOk, "is ok") {
  if (arg.hasError()) {
    *result_listener << "which has error: " << arg.error();
    return false;
  }
  return arg.hasResult();
}

/**
 * Matches a \ref Result or \ref GpuError whose error type equals \p expectedType. On mismatch
 * prints the full error (type and message) or the absence of one.
 *
 * @param expectedType Expected \ref GpuErrorType.
 */
MATCHER_P(IsGpuError, expectedType,
          std::string("is a GpuError of type ") + testing::PrintToString(expectedType)) {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  if constexpr (std::is_same_v<ArgType, GpuError>) {
    *result_listener << "which is: " << arg;
    return arg.type == expectedType;
  } else {
    if (!arg.hasError()) {
      *result_listener << "which has no error";
      return false;
    }
    *result_listener << "which has error: " << arg.error();
    return arg.error().type == expectedType;
  }
}

/**
 * Matches a \ref Result or \ref GpuError whose error type equals \p expectedType and whose
 * message matches \p messageMatcher (string or gmock matcher).
 *
 * @param expectedType Expected \ref GpuErrorType.
 * @param messageMatcher Matcher for the error message.
 */
MATCHER_P2(IsGpuErrorWithMessage, expectedType, messageMatcher,
           std::string("is a GpuError of type ") + testing::PrintToString(expectedType) +
               " with message " + testing::PrintToString(messageMatcher)) {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  const GpuError* error = nullptr;
  if constexpr (std::is_same_v<ArgType, GpuError>) {
    error = &arg;
  } else {
    if (!arg.hasError()) {
      *result_listener << "which has no error";
      return false;
    }
    error = &arg.error();
  }

  *result_listener << "which is: " << *error;
  return error->type == expectedType &&
         testing::ExplainMatchResult(messageMatcher, error->message, result_listener);
}

/**
 * Unwraps a \ref Result, adding a test failure and returning a default-constructed value if it
 * holds an error.
 *
 * @param result Result to unwrap.
 */
template <typename T>
T GetResultOrFail(Result<T>&& result) {
  if (result.hasError()) {
    ADD_FAILURE() << "Expected a result, got error: " << result.error();
    return T{};
  }
  return std::move(result).result();
}

/**
 * Whole milliseconds from \p from to \p to, as an integer a failure message prints.
 *
 * @param from Start of the span.
 * @param to End of the span.
 */
inline int64_t MillisecondsBetween(std::chrono::steady_clock::time_point from,
                                   std::chrono::steady_clock::time_point to) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

/**
 * Runs a step on a thread of its own at a steady pace, standing in for a GPU that finishes work
 * slowly: \p count steps, \p interval apart, the first one \p interval after construction. Records
 * when the last step began and returned, and the longest time between consecutive step starts
 * (the first counted from construction), so a case can tell a host that fell behind from the
 * behavior it checks; see \ref KeptPaceWithin.
 */
class PacedSteps {
public:
  /**
   * @param count Steps to run.
   * @param interval Time to sleep before each step.
   * @param step Run for each step with its number, counting from 1.
   */
  PacedSteps(int count, std::chrono::milliseconds interval, std::function<void(int)> step)
      : step_(std::move(step)), thread_([this, count, interval] { run(count, interval); }) {}

  /// Waits for the last step.
  ~PacedSteps() { join(); }

  PacedSteps(const PacedSteps&) = delete;
  PacedSteps& operator=(const PacedSteps&) = delete;

  /// Waits for the last step. The accessors read what the stepping thread wrote, so call this
  /// first.
  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /// Just before the last step began: nothing the step caused can have happened earlier.
  std::chrono::steady_clock::time_point lastStepStart() const { return lastStepStart_; }

  /// Just after the last step returned.
  std::chrono::steady_clock::time_point lastStepEnd() const { return lastStepEnd_; }

  /// Longest time in milliseconds between consecutive step starts, the first counted from
  /// construction.
  int64_t longestGapMs() const { return longestGapMs_; }

  /// Prints the longest gap, which is what a failed \ref KeptPaceWithin is about.
  /// @param steps Steps to print. @param os Output stream.
  friend void PrintTo(const PacedSteps& steps, std::ostream* os) {
    *os << "steps whose longest gap was " << steps.longestGapMs_ << " ms";
  }

private:
  /// The stepping thread. @param count Steps to run. @param interval Sleep before each step.
  void run(int count, std::chrono::milliseconds interval) {
    std::chrono::steady_clock::time_point previous = constructedAt_;
    for (int i = 1; i <= count; ++i) {
      std::this_thread::sleep_for(interval);
      lastStepStart_ = std::chrono::steady_clock::now();
      longestGapMs_ = std::max(longestGapMs_, MillisecondsBetween(previous, lastStepStart_));
      previous = lastStepStart_;
      step_(i);
      lastStepEnd_ = std::chrono::steady_clock::now();
    }
  }

  std::function<void(int)> step_;  //!< What each step does.
  const std::chrono::steady_clock::time_point constructedAt_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point lastStepStart_;  //!< Written by the stepping thread.
  std::chrono::steady_clock::time_point lastStepEnd_;    //!< Written by the stepping thread.
  int64_t longestGapMs_ = 0;                             //!< Written by the stepping thread.
  std::thread thread_;  //!< Declared last, so it starts once every member it uses exists.
};

/**
 * Matches \ref PacedSteps whose every gap between steps stayed under three quarters of
 * \p stallBound, read after \ref PacedSteps::join. A host that left the stepping thread asleep
 * longer than that can stall the work the steps stand for, once the time a wait takes to see a
 * step's effect is added, so a case that judges a wait with that bound would blame the wait for
 * the host's delay. The quarter left over is that margin.
 *
 * @param stallBound The bound the case's wait judges a stall by.
 */
MATCHER_P(
    KeptPaceWithin, stallBound,
    "kept every gap under three quarters of a " +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(stallBound).count()) +
        " ms stall bound") {
  const int64_t limitMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(stallBound).count() * 3 / 4;
  *result_listener << "the host left " << arg.longestGapMs()
                   << " ms between steps, so the work they pace may really have stalled and the "
                      "case cannot tell that from the behavior it checks";
  return arg.longestGapMs() < limitMs;
}

}  // namespace donner::gpu
