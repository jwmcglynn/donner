#pragma once
/// @file
/// Helpers for cases that order events around a Metal device's bounded waits on a
/// \ref donner::gpu::metal::TestProgressClock: run the wait on a worker thread, move the clock by
/// hand, and observe how the wait judged its bounds at each time the case moved it to.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <thread>
#include <utility>

#include "donner/base/tests/StallGuard.h"
#include "donner/gpu/metal/MetalDevice.h"

namespace donner::gpu::metal {

/// How a wait stood once it had judged its bounds at a time the case moved the clock to.
enum class WaitStanding : uint8_t {
  StillWaiting,  //!< It looked at that time, found its work pending and went back to sleep.
  Returned,      //!< It returned.
  NeverJudged,   //!< It neither looked at that time nor returned within the hang guard.
};

/// Prints \p standing for a failed expectation. @param os Output stream.
/// @param standing Value to print.
inline std::ostream& operator<<(std::ostream& os, WaitStanding standing) {
  switch (standing) {
    case WaitStanding::StillWaiting: return os << "StillWaiting";
    case WaitStanding::Returned: return os << "Returned";
    case WaitStanding::NeverJudged: return os << "NeverJudged";
  }
  return os << "WaitStanding(" << static_cast<int>(standing) << ")";
}

/**
 * Runs a call that waits on a device, such as a submission that waits for room or a serial wait,
 * on a thread of its own, so the case's thread can order events around the wait.
 *
 * A case that ends before the call returns, for instance after a failed assertion, has it let go
 * by \p unblock on destruction, which must end every wait the call can be in.
 *
 * @tparam T What the call returns.
 */
template <typename T>
class WaitOnWorker {
public:
  /**
   * Starts \p call on a worker thread.
   *
   * @param call The call that waits.
   * @param unblock Ends every wait \p call can be in; run on destruction if it has not returned.
   */
  WaitOnWorker(std::function<T()> call, std::function<void()> unblock)
      : unblock_(std::move(unblock)), thread_([this, call = std::move(call)] {
          result_.emplace(call());
          returned_.store(true, std::memory_order_release);
        }) {}

  /// Lets the call go if it has not returned, then joins its thread.
  ~WaitOnWorker() {
    if (!returned()) {
      unblock_();
    }
    thread_.join();
  }

  WaitOnWorker(const WaitOnWorker&) = delete;
  WaitOnWorker& operator=(const WaitOnWorker&) = delete;

  /// Whether the call has returned. Readable from any thread.
  [[nodiscard]] bool returned() const { return returned_.load(std::memory_order_acquire); }

  /// Waits for the call to return, up to the hang guard. @return Whether it returned.
  [[nodiscard]] bool awaitReturn() const {
    return donner::tests::WaitUntil([this] { return returned(); });
  }

  /// What the call returned. Only valid once \ref returned holds.
  [[nodiscard]] const T& result() const { return *result_; }

private:
  std::function<void()> unblock_;
  std::optional<T> result_;  //!< Written by the worker before \ref returned_ is set.
  std::atomic<bool> returned_{false};
  std::thread thread_;  //!< Declared last, so it starts once every member it uses exists.
};

/**
 * Waits until the wait on \p device has judged its bounds at the time \p clock shows now, or until
 * \p returned holds, up to the hang guard.
 *
 * A wait records each look that finds its work pending, at the time it judged by, before it goes
 * back to sleep. Once it has recorded a look at the clock's current time, it has compared every
 * bound it keeps against that time and decided to keep waiting; nothing short of a further change
 * in completion state or another advance of the clock will wake it.
 *
 * @param device Device whose wait to observe. It must have only that one wait in progress.
 * @param clock Progress clock installed on \p device.
 * @param returned Whether the call that waits has returned.
 */
inline WaitStanding AwaitJudgedAtNow(const MetalDevice& device, const TestProgressClock& clock,
                                     const std::function<bool()>& returned) {
  const std::chrono::steady_clock::time_point now = clock.now();
  const auto judgedAtNow = [&] {
    const std::optional<std::chrono::steady_clock::time_point> looked =
        device.lastPendingWaitLookForTest();
    return looked.has_value() && *looked >= now;
  };
  if (!donner::tests::WaitUntil([&] { return returned() || judgedAtNow(); })) {
    return WaitStanding::NeverJudged;
  }
  return returned() ? WaitStanding::Returned : WaitStanding::StillWaiting;
}

}  // namespace donner::gpu::metal
