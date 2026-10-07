#include "donner/gpu/browser/BrowserGpuOwner.h"

#include <emscripten/emscripten.h>

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdlib>
#ifdef __EMSCRIPTEN_PTHREADS__
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>
#endif

namespace donner::gpu::browser {
namespace {
#ifdef __EMSCRIPTEN_PTHREADS__
std::atomic<pthread_t> gOwner{};
std::atomic<uint32_t> gOwnerWaitPumps{0};
thread_local bool gProcessingOwnerWait = false;

struct OwnerRequest {
  void (*operation)(void*);
  void* context;
  std::atomic<int> complete{0};
};

void Invoke(void* context) {
  auto& request = *static_cast<OwnerRequest*>(context);
  request.operation(request.context);
  emscripten_futex_wake(&request.complete, INT_MAX);
  // This release is the callback's final access to the caller-owned request.
  request.complete.store(1, std::memory_order_release);
}
#endif
}  // namespace

void RegisterBrowserGpuOwner() {
#ifdef __EMSCRIPTEN_PTHREADS__
  pthread_t expected{};
  if (!gOwner.compare_exchange_strong(expected, pthread_self()) &&
      !pthread_equal(expected, pthread_self())) {
    std::abort();
  }
  // Allocate the owner's queue before any client can hold an application lock while dispatching.
  if (!emscripten_proxy_async(
          emscripten_proxy_get_system_queue(), pthread_self(), [](void*) {}, nullptr)) {
    std::abort();
  }
  emscripten_proxy_execute_queue(emscripten_proxy_get_system_queue());
  // clang-format off
  EM_ASM({
    globalThis['__donnerGpuOwner'] = true;
    const index = Number($0) / 4;
    globalThis['__donnerReadGpuOwnerWaitPumps'] = () => Atomics.load(HEAPU32, index);
  }, &gOwnerWaitPumps);
  // clang-format on
#endif
}

void RunOnBrowserGpuOwner(void (*operation)(void*), void* context) {
#ifdef __EMSCRIPTEN_PTHREADS__
  const pthread_t owner = gOwner.load(std::memory_order_acquire);
  if (owner && !pthread_equal(owner, pthread_self())) {
    OwnerRequest request{operation, context};
    // The editor wait adapter services this queue; callbacks only call nonblocking JS primitives.
    if (!emscripten_proxy_async(emscripten_proxy_get_system_queue(), owner, &Invoke, &request)) {
      std::abort();
    }
    const double deadlineMs = emscripten_get_now() + 10000.0;
    while (request.complete.load(std::memory_order_acquire) == 0) {
      const double remainingMs = deadlineMs - emscripten_get_now();
      if (remainingMs <= 0.0) {
        // Returning would leave the owner's callback holding borrowed caller memory.
        std::abort();
      }
      // Recheck within one millisecond if the final store raced the preceding wake.
      emscripten_futex_wait(&request.complete, 0, remainingMs < 1.0 ? remainingMs : 1.0);
    }
    return;
  }
#endif
  operation(context);
}

bool IsBrowserGpuOwnerThread() {
#ifdef __EMSCRIPTEN_PTHREADS__
  const pthread_t owner = gOwner.load(std::memory_order_acquire);
  return owner && pthread_equal(owner, pthread_self());
#else
  return false;
#endif
}

void ProcessBrowserGpuOwnerWait() {
#ifdef __EMSCRIPTEN_PTHREADS__
  if (gProcessingOwnerWait || !IsBrowserGpuOwnerThread()) {
    return;
  }
  gProcessingOwnerWait = true;
  gOwnerWaitPumps.fetch_add(1, std::memory_order_relaxed);
  emscripten_proxy_execute_queue(emscripten_proxy_get_system_queue());
  gProcessingOwnerWait = false;
#endif
}

bool UsesBrowserGpuOwner() {
#ifdef __EMSCRIPTEN_PTHREADS__
  return gOwner.load(std::memory_order_acquire) != pthread_t{};
#else
  return false;
#endif
}

}  // namespace donner::gpu::browser
