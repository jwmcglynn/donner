#include "donner/gpu/browser/BrowserGpuOwner.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

#include "donner/gpu/browser/BrowserGpuTaskQueue.h"
#ifdef __EMSCRIPTEN_PTHREADS__
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>
#endif

namespace donner::gpu::browser {
namespace {
#ifdef __EMSCRIPTEN_PTHREADS__
std::atomic<pthread_t> gOwner{};
std::atomic<bool> gStopped{false};
constexpr std::size_t kPumpCount = 0;
constexpr std::size_t kDispatchCount = 1;
constexpr std::size_t kMaximumPumpUs = 2;
constexpr std::size_t kRequested = 3;
constexpr std::size_t kStarted = 4;
constexpr std::size_t kFinished = 5;
constexpr std::size_t kMaximumDispatchWaitUs = 6;
constexpr std::size_t kBatches = 7;
constexpr std::size_t kMaximumBatchSize = 8;
constexpr std::size_t kMaximumBatchUs = 9;
constexpr std::size_t kTotalDispatchWaitUs = 10;
constexpr std::size_t kTotalBatchUs = 11;
constexpr std::size_t kFutexTimeouts = 12;
constexpr std::size_t kWakeBeforeCompletion = 13;
constexpr std::size_t kPostWakeTimeouts = 14;
constexpr std::size_t kOtherOperations = 15;
constexpr std::size_t kOwnershipQueries = 16;
constexpr std::size_t kDeviceLossQueries = 17;
constexpr std::size_t kCompletionQueries = 18;
constexpr std::size_t kInlineDispatches = 19;
constexpr std::size_t kTotalInlineUs = 20;
constexpr std::size_t kMaximumInlineCallbackUs = 21;
constexpr std::size_t kMaximumInlineEpochCalls = 22;
constexpr std::size_t kPulses = 23;
constexpr std::size_t kPulseDispatches = 24;
std::array<std::atomic<uint32_t>, 25> gOwnerWaitStats{};
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
static_assert(std::atomic<uint32_t>::is_always_lock_free);
thread_local bool gProcessingOwnerWait = false;
// The queue outlives SDK notification messages, including those pending at owner shutdown.
em_proxying_queue* gNotificationQueue = nullptr;
BrowserGpuTaskQueue gReady;
bool gProcessingBatch = false;
bool gPulseScheduled = false;
bool gCanvasFrameActive = false;
constexpr std::size_t kMaximumInlineCalls = 32;
constexpr double kMaximumInlineMs = 2.0;
std::size_t gInlineCalls = 0;
double gInlineMs = 0.0;

// clang-format off
EM_JS(void, ScheduleGpuPulse, (), {
  globalThis['__donnerGpuPulsePort'].postMessage(0);
});
// clang-format on

void SchedulePulse() {
  if (!gPulseScheduled && !gCanvasFrameActive) {
    gPulseScheduled = true;
    ScheduleGpuPulse();
  }
}

void RecordMaximum(std::size_t index, double value) {
  const auto bounded = static_cast<uint32_t>(
      std::clamp(value, 0.0, static_cast<double>(std::numeric_limits<uint32_t>::max())));
  auto& maximum = gOwnerWaitStats[index];
  maximum.store(std::max(maximum.load(std::memory_order_relaxed), bounded),
                std::memory_order_relaxed);
}

void AddSaturatingMicros(std::size_t index, double value) {
  const auto increment = static_cast<uint32_t>(
      std::clamp(value, 0.0, static_cast<double>(std::numeric_limits<uint32_t>::max())));
  auto& total = gOwnerWaitStats[index];
  uint32_t previous = total.load(std::memory_order_relaxed);
  while (!total.compare_exchange_weak(
      previous, previous + std::min(increment, std::numeric_limits<uint32_t>::max() - previous),
      std::memory_order_relaxed)) {}
}

std::size_t OperationCounter(BrowserGpuOperationKind kind) {
  switch (kind) {
    case BrowserGpuOperationKind::OwnershipQuery: return kOwnershipQueries;
    case BrowserGpuOperationKind::DeviceLossQuery: return kDeviceLossQueries;
    case BrowserGpuOperationKind::CompletionQuery: return kCompletionQueries;
    case BrowserGpuOperationKind::Other: return kOtherOperations;
  }
  std::abort();
}

std::size_t ExecuteReadyBatch() {
  const double started = emscripten_get_now();
  const std::size_t count = gReady.executeBatch();
  if (count) {
    gOwnerWaitStats[kBatches].fetch_add(1, std::memory_order_relaxed);
    RecordMaximum(kMaximumBatchSize, static_cast<double>(count));
    const double elapsedUs = (emscripten_get_now() - started) * 1000.0;
    RecordMaximum(kMaximumBatchUs, elapsedUs);
    AddSaturatingMicros(kTotalBatchUs, elapsedUs);
  }
  if (!gReady.empty()) {
    SchedulePulse();
  }
  return count;
}

void TryInlineDispatch(bool wasEmpty) {
  if (!wasEmpty || gProcessingBatch || gCanvasFrameActive || gInlineCalls >= kMaximumInlineCalls ||
      gInlineMs >= kMaximumInlineMs) {
    return;
  }
  gProcessingBatch = true;
  const double started = emscripten_get_now();
  // The FIFO held only this request; reentrant requests stay in the next detached batch.
  const std::size_t count = ExecuteReadyBatch();
  const double elapsedMs = emscripten_get_now() - started;
  gInlineCalls += count;
  gInlineMs += elapsedMs;
  gOwnerWaitStats[kInlineDispatches].fetch_add(count, std::memory_order_relaxed);
  AddSaturatingMicros(kTotalInlineUs, elapsedMs * 1000.0);
  RecordMaximum(kMaximumInlineCallbackUs, elapsedMs * 1000.0);
  RecordMaximum(kMaximumInlineEpochCalls, static_cast<double>(gInlineCalls));
  gProcessingBatch = false;
}

struct OwnerRequest {
  void (*operation)(void*);
  void* context;
  std::atomic<int> complete{0};
  BrowserGpuTask task;
};

void WaitForRequest(OwnerRequest& request, double requestStartedMs) {
  const double deadlineMs = emscripten_get_now() + 10000.0;
  bool wokeBeforeCompletion = false;
  while (request.complete.load(std::memory_order_acquire) == 0) {
    const double remainingMs = deadlineMs - emscripten_get_now();
    const uint32_t waitedUs =
        static_cast<uint32_t>(std::clamp((10000.0 - remainingMs) * 1000.0, 0.0, 10000000.0));
    auto& maximumWait = gOwnerWaitStats[kMaximumDispatchWaitUs];
    uint32_t previous = maximumWait.load(std::memory_order_relaxed);
    while (previous < waitedUs &&
           !maximumWait.compare_exchange_weak(previous, waitedUs, std::memory_order_relaxed)) {}
    if (remainingMs <= 0.0) {
      // Returning would leave the owner's callback holding borrowed caller memory.
      std::abort();
    }
    // Recheck within one millisecond if the final store raced the preceding wake.
    const int waitResult =
        emscripten_futex_wait(&request.complete, 0, remainingMs < 1.0 ? remainingMs : 1.0);
    const bool complete = request.complete.load(std::memory_order_acquire) != 0;
    if (waitResult == -ETIMEDOUT) {
      gOwnerWaitStats[kFutexTimeouts].fetch_add(1, std::memory_order_relaxed);
      if (wokeBeforeCompletion && complete) {
        gOwnerWaitStats[kPostWakeTimeouts].fetch_add(1, std::memory_order_relaxed);
      }
    }
    wokeBeforeCompletion = waitResult == 0 && !complete;
    if (wokeBeforeCompletion) {
      gOwnerWaitStats[kWakeBeforeCompletion].fetch_add(1, std::memory_order_relaxed);
    }
  }
  AddSaturatingMicros(kTotalDispatchWaitUs, (emscripten_get_now() - requestStartedMs) * 1000.0);
}

void Invoke(void* context) {
  auto& request = *static_cast<OwnerRequest*>(context);
  gOwnerWaitStats[kStarted].fetch_add(1, std::memory_order_relaxed);
  if (gProcessingOwnerWait) {
    gOwnerWaitStats[kDispatchCount].fetch_add(1, std::memory_order_relaxed);
  }
  request.operation(request.context);
  gOwnerWaitStats[kFinished].fetch_add(1, std::memory_order_relaxed);
  emscripten_futex_wake(&request.complete, INT_MAX);
  // This release is the callback's final access to the caller-owned request.
  request.complete.store(1, std::memory_order_release);
}

void EnqueueRequest(void* context) {
  auto& request = *static_cast<OwnerRequest*>(context);
  request.task.callback = &Invoke;
  request.task.context = &request;
  const bool wasEmpty = gReady.empty();
  gReady.enqueue(request.task);
  // Keep this future pulse even if inline work empties the FIFO: only it renews the budget.
  SchedulePulse();
  TryInlineDispatch(wasEmpty);
}
#endif
}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void donner_browser_gpu_pulse() {
#ifdef __EMSCRIPTEN_PTHREADS__
  if (!IsBrowserGpuOwnerThread()) {
    return;
  }
  gPulseScheduled = false;
  if (gCanvasFrameActive) {
    return;
  }
  if (gProcessingBatch) {
    SchedulePulse();
    return;
  }
  gInlineCalls = 0;
  gInlineMs = 0.0;
  gOwnerWaitStats[kPulses].fetch_add(1, std::memory_order_relaxed);
  gProcessingBatch = true;
  const std::size_t count = ExecuteReadyBatch();
  gOwnerWaitStats[kPulseDispatches].fetch_add(count, std::memory_order_relaxed);
  gProcessingBatch = false;
#endif
}

void RegisterBrowserGpuOwner() {
#ifdef __EMSCRIPTEN_PTHREADS__
  pthread_t expected{};
  if (gStopped.load(std::memory_order_acquire) ||
      !gOwner.compare_exchange_strong(expected, pthread_self())) {
    std::abort();
  }
  // Prewarm notifications before clients can hold application locks while dispatching.
  if (!gNotificationQueue) {
    gNotificationQueue = em_proxying_queue_create();
  }
  if (!gNotificationQueue ||
      !emscripten_proxy_async(gNotificationQueue, pthread_self(), [](void*) {}, nullptr)) {
    std::abort();
  }
  emscripten_proxy_execute_queue(gNotificationQueue);
  // clang-format off
  EM_ASM({
    globalThis['__donnerGpuOwner'] = true;
    const channel = new MessageChannel();
    channel.port1.onmessage = () => _donner_browser_gpu_pulse();
    globalThis['__donnerGpuPulsePort'] = channel.port2;
    globalThis['__donnerCloseGpuPulse'] = () => {
      channel.port1.onmessage = null;
      channel.port1.close();
      channel.port2.close();
      delete globalThis['__donnerGpuPulsePort'];
      globalThis['__donnerGpuOwner'] = false;
    };
    const index = Number($0) / 4;
    globalThis['__donnerReadGpuOwnerWaitStats'] = () => ({
      'pumps': Atomics.load(HEAPU32, index),
      'dispatches': Atomics.load(HEAPU32, index + 1),
      'maximumPumpMs': Atomics.load(HEAPU32, index + 2) / 1000,
      'requested': Atomics.load(HEAPU32, index + 3),
      'started': Atomics.load(HEAPU32, index + 4),
      'finished': Atomics.load(HEAPU32, index + 5),
      'maximumDispatchWaitMs': Atomics.load(HEAPU32, index + 6) / 1000,
      'batches': Atomics.load(HEAPU32, index + 7),
      'maximumBatchSize': Atomics.load(HEAPU32, index + 8),
      'maximumBatchMs': Atomics.load(HEAPU32, index + 9) / 1000,
      'totalDispatchWaitMs': Atomics.load(HEAPU32, index + 10) / 1000,
      'totalBatchMs': Atomics.load(HEAPU32, index + 11) / 1000,
      'futexTimeouts': Atomics.load(HEAPU32, index + 12),
      'wakeBeforeCompletion': Atomics.load(HEAPU32, index + 13),
      'postWakeTimeouts': Atomics.load(HEAPU32, index + 14),
      'otherOperations': Atomics.load(HEAPU32, index + 15),
      'ownershipQueries': Atomics.load(HEAPU32, index + 16),
      'deviceLossQueries': Atomics.load(HEAPU32, index + 17),
      'completionQueries': Atomics.load(HEAPU32, index + 18),
      'inlineDispatches': Atomics.load(HEAPU32, index + 19),
      'totalInlineMs': Atomics.load(HEAPU32, index + 20) / 1000,
      'maximumInlineCallbackMs': Atomics.load(HEAPU32, index + 21) / 1000,
      'maximumInlineEpochCalls': Atomics.load(HEAPU32, index + 22),
      'pulses': Atomics.load(HEAPU32, index + 23),
      'pulseDispatches': Atomics.load(HEAPU32, index + 24),
      'timingSaturated': Atomics.load(HEAPU32, index + 10) === 4294967295 ||
                         Atomics.load(HEAPU32, index + 11) === 4294967295 ||
                         Atomics.load(HEAPU32, index + 20) === 4294967295,
    });
  }, gOwnerWaitStats.data());
  // clang-format on
#endif
}

void RunOnBrowserGpuOwner(void (*operation)(void*), void* context,
                          [[maybe_unused]] BrowserGpuOperationKind kind) {
#ifdef __EMSCRIPTEN_PTHREADS__
  if (gStopped.load(std::memory_order_acquire)) {
    std::abort();
  }
  const pthread_t owner = gOwner.load(std::memory_order_acquire);
  if (owner && !pthread_equal(owner, pthread_self())) {
    const double requestStartedMs = emscripten_get_now();
    OwnerRequest request{operation, context, {}, {}};
    gOwnerWaitStats[kRequested].fetch_add(1, std::memory_order_relaxed);
    gOwnerWaitStats[OperationCounter(kind)].fetch_add(1, std::memory_order_relaxed);
    // The inline budget bounds repeated requests within the SDK's otherwise unbounded drain.
    if (!emscripten_proxy_async(gNotificationQueue, owner, &EnqueueRequest, &request)) {
      std::abort();
    }
    WaitForRequest(request, requestStartedMs);
    return;
  }
#endif
  operation(context);
}

bool IsBrowserGpuOwnerThread() {
#ifdef __EMSCRIPTEN_PTHREADS__
  const pthread_t owner = gOwner.load(std::memory_order_acquire);
  return !gStopped.load(std::memory_order_acquire) && owner && pthread_equal(owner, pthread_self());
#else
  return false;
#endif
}

void ProcessBrowserGpuOwnerWait() {
#ifdef __EMSCRIPTEN_PTHREADS__
  if (gProcessingBatch || !IsBrowserGpuOwnerThread()) {
    return;
  }
  gProcessingBatch = true;
  gProcessingOwnerWait = true;
  gOwnerWaitStats[kPumpCount].fetch_add(1, std::memory_order_relaxed);
  const double started = emscripten_get_now();
  emscripten_proxy_execute_queue(gNotificationQueue);
  ExecuteReadyBatch();
  RecordMaximum(kMaximumPumpUs, (emscripten_get_now() - started) * 1000.0);
  gProcessingOwnerWait = false;
  gProcessingBatch = false;
#endif
}

bool IsBrowserGpuOwnerDispatchActive() {
#ifdef __EMSCRIPTEN_PTHREADS__
  return IsBrowserGpuOwnerThread() && gProcessingBatch;
#else
  return false;
#endif
}

void SetBrowserGpuOwnerCanvasFrameActive([[maybe_unused]] bool active) {
#ifdef __EMSCRIPTEN_PTHREADS__
  if (!IsBrowserGpuOwnerThread() || gCanvasFrameActive == active || (active && gProcessingBatch)) {
    std::abort();
  }
  gCanvasFrameActive = active;
  if (!active && !gReady.empty()) {
    SchedulePulse();
  }
#endif
}

void StopBrowserGpuOwner() {
#ifdef __EMSCRIPTEN_PTHREADS__
  if (!IsBrowserGpuOwnerThread() || gProcessingBatch || gCanvasFrameActive || !gReady.empty() ||
      gOwnerWaitStats[kRequested].load() != gOwnerWaitStats[kFinished].load()) {
    std::abort();
  }
  // All clients have joined and released resources before the pulse ports close.
  EM_ASM({ globalThis['__donnerCloseGpuPulse'](); });
  gPulseScheduled = false;
  gStopped.store(true, std::memory_order_release);
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
