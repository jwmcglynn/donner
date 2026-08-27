#include "donner/svg/renderer/geode/GeodeGpuWait.h"

#include <thread>

namespace donner::geode {

bool DeclareDeviceLost(GeodeDeviceLostState& state) {
  return !state.lost.exchange(true, std::memory_order_acq_rel);
}

bool DeclareDeviceLostAfterWaitTimeout(GeodeDeviceLostState& state, GpuWaitSite site,
                                       std::chrono::milliseconds elapsed) {
  if (!DeclareDeviceLost(state)) {
    return false;
  }
  // Winning the transition makes this call the only writer of the
  // attribution, so the stores below race with nothing. Publish the elapsed
  // time first and the site last: `timedOutSite` is what a reader tests, so
  // releasing it last is what makes the pair observable together.
  state.timedOutElapsedMs.store(static_cast<int>(elapsed.count()), std::memory_order_relaxed);
  GpuWaitSite unattributed = GpuWaitSite::None;
  state.timedOutSite.compare_exchange_strong(unattributed, site, std::memory_order_release,
                                             std::memory_order_relaxed);
  return true;
}

GpuWaitResult BoundedGpuWait(const std::function<bool()>& pollOnce,
                             std::chrono::microseconds timeout,
                             std::chrono::microseconds pollInterval,
                             const GpuWaitTestHooks& testHooks) {
  const auto now = [&testHooks]() {
    return testHooks.now ? testHooks.now() : std::chrono::steady_clock::now();
  };
  const auto sleep = [&testHooks](std::chrono::microseconds duration) {
    if (testHooks.sleep) {
      testHooks.sleep(duration);
    } else {
      std::this_thread::sleep_for(duration);
    }
  };

  const std::chrono::steady_clock::time_point deadline = now() + timeout;
  for (;;) {
    if (pollOnce()) {
      return GpuWaitResult::Complete;
    }
    if (now() >= deadline) {
      return GpuWaitResult::TimedOut;
    }
    if (pollInterval.count() > 0) {
      sleep(pollInterval);
    }
  }
}

}  // namespace donner::geode
