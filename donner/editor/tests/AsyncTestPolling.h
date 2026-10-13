#pragma once
/// @file
///
/// Shared polling/wait helpers for the async-renderer tests (see issue
/// #1218). Every wait is the same loop: poll, stop when it yields, sleep a
/// millisecond, give up at a bound. Only the bound and the predicate differ.
///
/// The stall guard and the clock-bounded helpers live in
/// `donner/base/tests/StallGuard.h` so suites outside the editor share them.

#include <chrono>

#include "donner/base/tests/StallGuard.h"

namespace donner::editor::tests {

using donner::tests::HangGuard;
using donner::tests::kHangGuard;
using donner::tests::kPollInterval;
using donner::tests::PollForResult;
using donner::tests::PollUntil;
using donner::tests::WaitUntil;

/// A test-injected render or thumbnail delay that no test waits out. Work held by it ends only by
/// cancellation, so seeing it end shows the cancellation happened, not that the host was fast.
constexpr std::chrono::milliseconds kUnelapsedDelay = std::chrono::hours(1);

/**
 * Waits under the stall guard until `renderer` reports it is no longer busy.
 *
 * @param renderer An \ref AsyncRenderer, or anything else with `isBusy()`.
 * @return Whether it went idle before the guard expired.
 */
template <typename Renderer>
bool WaitForIdle(const Renderer& renderer) {
  return WaitUntil([&] { return !renderer.isBusy(); });
}

/**
 * Waits under the stall guard until `renderer` has no render in flight.
 *
 * @param renderer An \ref AsyncRenderer.
 * @return Whether the in-flight render finished before the guard expired.
 */
template <typename Renderer>
bool WaitForNoRenderInFlight(Renderer& renderer) {
  return renderer.waitUntilNoRenderInFlightForTesting(HangGuard().deadline());
}

}  // namespace donner::editor::tests
