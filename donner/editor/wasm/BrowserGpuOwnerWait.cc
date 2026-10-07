#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <limits>

#include "donner/gpu/browser/BrowserGpuOwner.h"

extern "C" int __real_emscripten_futex_wait(volatile void* address, uint32_t value,
                                            double maximumWaitMs);

namespace {

/// Wait on the original futex while servicing the application pthread's GPU queue.
/// @param address Futex address. @param value Expected value. @param maximumWaitMs Caller budget.
int WaitWithGpuProgress(volatile void* address, uint32_t value, double maximumWaitMs) {
  const double deadline = maximumWaitMs > 0.0 ? emscripten_get_now() + maximumWaitMs
                                              : std::numeric_limits<double>::infinity();
  // Match the runtime thread's one-millisecond slices; idle browser event loops do not enter here.
  constexpr double kWaitSliceMs = 1.0;
  while (true) {
    donner::gpu::browser::ProcessBrowserGpuOwnerWait();
    const double remaining = std::max(0.0, deadline - emscripten_get_now());
    const int result =
        __real_emscripten_futex_wait(address, value, std::min(remaining, kWaitSliceMs));
    if (result != -ETIMEDOUT || emscripten_get_now() >= deadline) {
      return result;
    }
  }
}

}  // namespace

/// The editor-only link wraps libc futex waits. Other threads retain the runtime's behavior.
/// @param address Futex address. @param value Expected value. @param maximumWaitMs Caller budget.
extern "C" int __wrap_emscripten_futex_wait(volatile void* address, uint32_t value,
                                            double maximumWaitMs) {
  if (!donner::gpu::browser::IsBrowserGpuOwnerThread() || maximumWaitMs == 0.0 ||
      std::isnan(maximumWaitMs)) {
    return __real_emscripten_futex_wait(address, value, maximumWaitMs);
  }
  return WaitWithGpuProgress(address, value, maximumWaitMs);
}
