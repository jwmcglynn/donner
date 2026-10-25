#include "donner/editor/wasm/BrowserGpuOwnerWait.h"

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

#include "donner/gpu/Device.h"
#include "donner/gpu/browser/BrowserGpuOwner.h"

extern "C" int __real_emscripten_futex_wait(volatile void* address, uint32_t value,
                                            double maximumWaitMs);

namespace {

struct OwnerWaitProbe {
  std::atomic<uint32_t> sliceReturned{0};
  std::atomic<uint32_t> complete{0};
  double sliceMs = 0.0;
  double ownerDeadline = 0.0;
  uint64_t serial = 0;
  bool producerIsGpuOwner = false;
  std::shared_ptr<donner::gpu::Device> device;
};

// Only the owner accesses this anchor. Retain it on failure even if abort throws in JavaScript;
// a producer still using the request/device must not depend on the owner's stack surviving.
std::shared_ptr<OwnerWaitProbe> gOwnerWaitProbe;

void CheckOwnerProbeDeadline() {
  if (gOwnerWaitProbe && emscripten_get_now() >= gOwnerWaitProbe->ownerDeadline) {
    std::abort();
  }
}

bool IsOwnerProbeSlice(volatile void* address) {
  return gOwnerWaitProbe && address == &gOwnerWaitProbe->complete &&
         !gOwnerWaitProbe->sliceReturned.load(std::memory_order_acquire);
}

void RecordOwnerProbeSlice(bool probeSlice, int result, double startedMs) {
  if (!probeSlice || result != -ETIMEDOUT) {
    return;
  }
  gOwnerWaitProbe->sliceMs = emscripten_get_now() - startedMs;
  if (gOwnerWaitProbe->sliceMs > 0.0) {
    gOwnerWaitProbe->sliceReturned.store(1, std::memory_order_release);
    emscripten_futex_wake(&gOwnerWaitProbe->sliceReturned, 1);
  }
}

/// Wait on the original futex while servicing the application pthread's GPU queue.
/// @param address Futex address. @param value Expected value. @param maximumWaitMs Caller budget.
int WaitWithGpuProgress(volatile void* address, uint32_t value, double maximumWaitMs) {
  const double deadline = maximumWaitMs > 0.0 ? emscripten_get_now() + maximumWaitMs
                                              : std::numeric_limits<double>::infinity();
  // Match the runtime thread's one-millisecond slices; idle browser event loops do not enter here.
  constexpr double kWaitSliceMs = 1.0;
  while (true) {
    CheckOwnerProbeDeadline();
    if (emscripten_get_now() >= deadline) {
      return __real_emscripten_futex_wait(address, value, 0.0);
    }
    donner::gpu::browser::ProcessBrowserGpuOwnerWait();
    CheckOwnerProbeDeadline();
    const double remaining = std::max(0.0, deadline - emscripten_get_now());
    const bool probeSlice = IsOwnerProbeSlice(address);
    const double probeStartedMs = probeSlice ? emscripten_get_now() : 0.0;
    const int result =
        __real_emscripten_futex_wait(address, value, std::min(remaining, kWaitSliceMs));
    CheckOwnerProbeDeadline();
    RecordOwnerProbeSlice(probeSlice, result, probeStartedMs);
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

namespace {

void* RunOwnerWaitProbeProducer(void* context) {
  std::unique_ptr<std::shared_ptr<OwnerWaitProbe>> start(
      static_cast<std::shared_ptr<OwnerWaitProbe>*>(context));
  const auto request = std::move(*start);
  start.reset();
  const double deadline = emscripten_get_now() + 2000.0;
  while (!request->sliceReturned.load(std::memory_order_acquire)) {
    const double remaining = deadline - emscripten_get_now();
    if (remaining <= 0.0) {
      std::abort();
    }
    emscripten_futex_wait(&request->sliceReturned, 0, std::min(remaining, 1.0));
  }
  request->producerIsGpuOwner = donner::gpu::browser::IsBrowserGpuOwnerThread();
  if (request->producerIsGpuOwner) {
    std::abort();
  }
  request->serial = request->device->completedSerial();
  request->complete.store(1, std::memory_order_release);
  emscripten_futex_wake(&request->complete, 1);
  return nullptr;
}

double JoinOwnerWaitProbe(pthread_t producer, OwnerWaitProbe& request, double deadline) {
  const double startedMs = emscripten_get_now();
  const timespec expired{};
  while (true) {
    CheckOwnerProbeDeadline();
    const int joined = pthread_timedjoin_np(producer, nullptr, &expired);
    CheckOwnerProbeDeadline();
    if (joined == 0) {
      return emscripten_get_now() - startedMs;
    }
    const double remaining = deadline - emscripten_get_now();
    if (joined != ETIMEDOUT || remaining <= 0.0) {
      std::abort();
    }
    emscripten_futex_wait(&request.complete, 1, std::min(remaining, 1.0));
  }
}

}  // namespace

namespace donner::editor::wasm {

OwnerWaitProbeResult ExerciseGpuOwnerLibcWaitForTesting(
    const std::shared_ptr<gpu::Device>& device) {
  if (!device || !gpu::browser::IsBrowserGpuOwnerThread() ||
      gpu::browser::IsBrowserGpuOwnerDispatchActive() || gOwnerWaitProbe) {
    std::abort();
  }
  static_assert(std::atomic<uint32_t>::is_always_lock_free);
  static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
  const auto request = std::make_shared<OwnerWaitProbe>();
  request->device = device;
  auto start = std::make_unique<std::shared_ptr<OwnerWaitProbe>>(request);
  const uint64_t initialSerial = device->completedSerial();
  const double deadline = emscripten_get_now() + 2000.0;
  request->ownerDeadline = deadline;
  gOwnerWaitProbe = request;
  pthread_t producer;
  if (pthread_create(&producer, nullptr, RunOwnerWaitProbeProducer, start.get()) != 0) {
    std::abort();
  }
  start.release();
  while (!request->complete.load(std::memory_order_acquire)) {
    const double remaining = deadline - emscripten_get_now();
    if (remaining <= 0.0) {
      std::abort();
    }
    emscripten_futex_wait(&request->complete, 0, remaining);
  }
  const double joinElapsedMs = JoinOwnerWaitProbe(producer, *request, deadline);
  if (request->serial < initialSerial || request->sliceMs <= 0.0) {
    std::abort();
  }
  gOwnerWaitProbe.reset();
  return {request->serial, request->sliceMs, request->producerIsGpuOwner, joinElapsedMs};
}

}  // namespace donner::editor::wasm
