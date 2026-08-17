#include "donner/svg/renderer/geode/GeodeGpuWait.h"

#include <thread>

namespace donner::geode {

GpuWaitResult BoundedGpuWait(const std::function<bool()>& pollOnce,
                             std::chrono::milliseconds timeout,
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
