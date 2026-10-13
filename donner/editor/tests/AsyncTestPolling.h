#pragma once
/// @file
///
/// Shared polling/wait helpers for the async-renderer tests (see issue
/// #1218). Every wait is the same loop: poll, stop when it yields, sleep a
/// millisecond, give up at a bound. Only the bound and the predicate differ.
///
/// The stall guard and the clock-bounded helpers live in
/// `donner/base/tests/StallGuard.h` so suites outside the editor share them.

#include <optional>
#include <thread>

#include "donner/base/tests/StallGuard.h"

namespace donner::editor::tests {

using donner::tests::HangGuard;
using donner::tests::kHangGuard;
using donner::tests::kPollInterval;
using donner::tests::PollForResult;
using donner::tests::PollUntil;
using donner::tests::WaitUntil;

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

}  // namespace donner::editor::tests
