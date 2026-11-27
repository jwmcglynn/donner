#include "donner/gpu/browser/BrowserGpuOwner.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "donner/base/FatalAbort.h"
#include "donner/gpu/browser/BrowserBridge.h"
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
// Requests a pulse ran, which happens only when inline execution refused them. Every request
// the owner receives outside its wait pump is first offered inline, so this stays zero while the
// inline budget, renewed by each pulse, is never exhausted.
constexpr std::size_t kPulseDispatches = 24;
constexpr std::size_t kResourceCreation = 25;
constexpr std::size_t kResourceDestruction = 26;
constexpr std::size_t kBufferWrite = 27;
constexpr std::size_t kTextureWrite = 28;
constexpr std::size_t kResourceSharing = 29;
constexpr std::size_t kCommandBatch = 30;
constexpr std::size_t kQueueSubmit = 31;
constexpr std::size_t kBindGroupConstruction = 32;
constexpr std::size_t kPipelineConstruction = 33;
constexpr std::size_t kBufferWriteSizeBucketStart = 34;  // Seven disjoint size buckets.
constexpr std::size_t kBufferWrite256 = 41;
constexpr std::size_t kBufferWrite448 = 42;
constexpr std::size_t kBufferWriteBytes = 43;
constexpr std::size_t kDeliveryUs = 44;
constexpr std::size_t kOwnerQueueUs = 45;
constexpr std::size_t kExecuteUs = 46;
constexpr std::size_t kCompletionWakeUs = 47;
constexpr std::size_t kStartLatencyBucketStart = 48;  // Eight disjoint latency buckets.
constexpr std::size_t kCanvasFrameActiveUs = 56;
constexpr std::size_t kCanvasFrames = 57;
constexpr std::size_t kReceivedDuringCanvasFrame = 58;
std::array<std::atomic<uint32_t>, 59> gOwnerWaitStats{};
static_assert(kBufferWriteSizeBucketStart + 7 == kBufferWrite256);
static_assert(kStartLatencyBucketStart + kBrowserGpuOwnerStartLatencyBucketUpperUs.size() + 1 ==
              kCanvasFrameActiveUs);
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
static_assert(std::atomic<uint32_t>::is_always_lock_free);
thread_local bool gProcessingOwnerWait = false;
thread_local uint32_t gRequestedByThisThread = 0;
#ifdef DONNER_TEST_GPU_OWNER_TRACE
thread_local BrowserGpuCreationApi gCreationApi = BrowserGpuCreationApi::None;

// Only the owner writes the fixed trace. An epoch change invalidates in-flight records.
using detail::gCreationTraceEpoch;
std::atomic<uint32_t> gNextCreationTraceRender{1};
thread_local uint32_t gCreationTraceRender = 0;
constexpr std::size_t kTraceRenderCapacity = 64;
constexpr std::size_t kTraceRequestCapacity = 96;
constexpr std::size_t kTraceWords = 17;
struct TraceRender {
  double ordinal = 0.0;
  double count = 0.0;
  double firstSentMs = 0.0;
  double lastSentMs = 0.0;
  double deliveryMs = 0.0;
  double firstOmittedSentMs = 0.0;
  std::array<std::array<double, kTraceWords>, kTraceRequestCapacity> rows{};
};
struct DispatchTrace {
  double renderCount = 0.0;
  double unscopedCount = 0.0;
  double omittedRenderRequests = 0.0;
  double firstOmittedRenderSentMs = 0.0;
  std::array<TraceRender, kTraceRenderCapacity> renders{};
};
DispatchTrace gDispatchTrace;
static_assert(sizeof(TraceRender) == sizeof(double) * (6 + kTraceWords * kTraceRequestCapacity));
static_assert(sizeof(DispatchTrace) ==
              4 * sizeof(double) + kTraceRenderCapacity * sizeof(TraceRender));

std::array<uint32_t, 3> OwnerTurns() {
  return {gOwnerWaitStats[kCanvasFrames].load(std::memory_order_relaxed),
          gOwnerWaitStats[kPulses].load(std::memory_order_relaxed),
          gOwnerWaitStats[kPumpCount].load(std::memory_order_relaxed)};
}
#endif

// The queue outlives SDK notification messages, including those pending at owner shutdown.
em_proxying_queue* gNotificationQueue = nullptr;
BrowserGpuTaskQueue gReady;
bool gProcessingBatch = false;
bool gPulseScheduled = false;
bool gCanvasFrameActive = false;
double gCanvasFrameStartedMs = 0.0;
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

void AddSaturatingCounter(std::size_t index, double value) {
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
    case BrowserGpuOperationKind::ResourceCreation: return kResourceCreation;
    case BrowserGpuOperationKind::ResourceDestruction: return kResourceDestruction;
    case BrowserGpuOperationKind::BufferWrite: return kBufferWrite;
    case BrowserGpuOperationKind::TextureWrite: return kTextureWrite;
    case BrowserGpuOperationKind::ResourceSharing: return kResourceSharing;
    case BrowserGpuOperationKind::CommandBatch: return kCommandBatch;
    case BrowserGpuOperationKind::QueueSubmit: return kQueueSubmit;
    case BrowserGpuOperationKind::BindGroupConstruction: return kBindGroupConstruction;
    case BrowserGpuOperationKind::PipelineConstruction: return kPipelineConstruction;
  }
  FatalAbort("gpu-owner.operation-kind", {{"kind", static_cast<std::int64_t>(kind)}});
}

std::size_t ExecuteReadyBatch() {
  const double started = emscripten_get_now();
  const std::size_t count = gReady.executeBatch();
  if (count) {
    gOwnerWaitStats[kBatches].fetch_add(1, std::memory_order_relaxed);
    RecordMaximum(kMaximumBatchSize, static_cast<double>(count));
    const double elapsedUs = (emscripten_get_now() - started) * 1000.0;
    RecordMaximum(kMaximumBatchUs, elapsedUs);
    AddSaturatingCounter(kTotalBatchUs, elapsedUs);
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
  AddSaturatingCounter(kTotalInlineUs, elapsedMs * 1000.0);
  RecordMaximum(kMaximumInlineCallbackUs, elapsedMs * 1000.0);
  RecordMaximum(kMaximumInlineEpochCalls, static_cast<double>(gInlineCalls));
  gProcessingBatch = false;
}

struct OwnerRequest {
  void (*operation)(void*);
  void* context;
  std::atomic<int> complete{0};
  BrowserGpuTask task;
  // Written by the owner before `complete` is released; read by the requester after acquiring it.
  double receivedMs = 0.0;
  double startedMs = 0.0;
  double finishedMs = 0.0;
#ifdef DONNER_TEST_GPU_OWNER_TRACE
  uint32_t creationTraceEpoch = 0;
  BrowserGpuOperationKind kind = BrowserGpuOperationKind::Other;
  bool canvasActiveAtReceive = false;
  BrowserGpuCreationApi creationApi = BrowserGpuCreationApi::None;
  detail::CreationTraceIdentity identity{};
  uint32_t render = 0;
  double sentMs = 0.0;
  std::array<uint32_t, 3> turnsAtSend{};
  std::array<uint32_t, 3> turnsAtReceive{};
#endif
};

#ifdef DONNER_TEST_GPU_OWNER_TRACE
TraceRender* FindTraceRender(uint32_t ordinal) {
  const auto active = static_cast<std::size_t>(gDispatchTrace.renderCount);
  for (std::size_t i = active; i > 0; --i) {
    if (gDispatchTrace.renders[i - 1].ordinal == ordinal) {
      return &gDispatchTrace.renders[i - 1];
    }
  }
  if (active >= kTraceRenderCapacity) {
    return nullptr;
  }
  TraceRender& render = gDispatchTrace.renders[active];
  render.ordinal = ordinal;
  render.count = 0;
  render.deliveryMs = 0;
  render.firstOmittedSentMs = 0;
  ++gDispatchTrace.renderCount;
  return &render;
}

void RecordCreationTrace(const OwnerRequest& request) {
  if (!request.creationTraceEpoch ||
      request.creationTraceEpoch != gCreationTraceEpoch.load(std::memory_order_relaxed)) {
    return;
  }
  if (!request.render) {
    ++gDispatchTrace.unscopedCount;
    return;
  }
  TraceRender* render = FindTraceRender(request.render);
  if (!render) {
    double& first = gDispatchTrace.firstOmittedRenderSentMs;
    first = first == 0.0 ? request.sentMs : std::min(first, request.sentMs);
    ++gDispatchTrace.omittedRenderRequests;
    return;
  }
  const auto index = static_cast<std::size_t>(render->count++);
  render->firstSentMs = index == 0 ? request.sentMs : render->firstSentMs;
  render->lastSentMs = request.sentMs;
  render->deliveryMs += std::max(0.0, request.receivedMs - request.sentMs);
  if (index >= kTraceRequestCapacity) {
    if (index == kTraceRequestCapacity) {
      render->firstOmittedSentMs = request.sentMs;
    }
    return;
  }
  auto& fields = render->rows[index];
  fields = {static_cast<double>(request.creationApi),
            request.sentMs,
            request.receivedMs,
            request.startedMs,
            request.finishedMs,
            static_cast<double>(request.turnsAtSend[0]),
            static_cast<double>(request.turnsAtSend[1]),
            static_cast<double>(request.turnsAtSend[2]),
            static_cast<double>(request.turnsAtReceive[0]),
            static_cast<double>(request.turnsAtReceive[1]),
            static_cast<double>(request.turnsAtReceive[2]),
            static_cast<double>(request.identity.viewSite),
            static_cast<double>(request.identity.device),
            static_cast<double>(request.identity.textureLifetime),
            static_cast<double>(request.render),
            static_cast<double>(request.kind),
            request.canvasActiveAtReceive ? 1.0 : 0.0};
}

#endif

/// Adds the elapsed time from \p fromMs to \p toMs, or nothing when the clocks disagree on order.
void AddPhase(std::size_t index, double fromMs, double toMs) {
  AddSaturatingCounter(index, std::max(0.0, toMs - fromMs) * 1000.0);
}

/// Splits a completed request's blocked time into delivery, owner queue, execution and wake.
void RecordRequestPhases(const OwnerRequest& request, double requestStartedMs, double observedMs) {
  AddPhase(kDeliveryUs, requestStartedMs, request.receivedMs);
  AddPhase(kOwnerQueueUs, request.receivedMs, request.startedMs);
  AddPhase(kExecuteUs, request.startedMs, request.finishedMs);
  AddPhase(kCompletionWakeUs, request.finishedMs, observedMs);
  const double startLatencyUs = std::max(0.0, request.startedMs - requestStartedMs) * 1000.0;
  const auto& bounds = kBrowserGpuOwnerStartLatencyBucketUpperUs;
  const auto bucket = static_cast<std::size_t>(
      std::find_if(bounds.begin(), bounds.end(),
                   [&](uint32_t bound) { return startLatencyUs <= bound; }) -
      bounds.begin());
  gOwnerWaitStats[kStartLatencyBucketStart + bucket].fetch_add(1, std::memory_order_relaxed);
}

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
      FatalAbort("gpu-owner.dispatch-deadline", {{"waitedUs", waitedUs},
                                                 {"requested", gOwnerWaitStats[kRequested].load()},
                                                 {"started", gOwnerWaitStats[kStarted].load()},
                                                 {"finished", gOwnerWaitStats[kFinished].load()},
                                                 {"stopped", gStopped.load()}});
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
  const double observedMs = emscripten_get_now();
  AddSaturatingCounter(kTotalDispatchWaitUs, (observedMs - requestStartedMs) * 1000.0);
  RecordRequestPhases(request, requestStartedMs, observedMs);
}

void Invoke(void* context) {
  auto& request = *static_cast<OwnerRequest*>(context);
  gOwnerWaitStats[kStarted].fetch_add(1, std::memory_order_relaxed);
  if (gProcessingOwnerWait) {
    gOwnerWaitStats[kDispatchCount].fetch_add(1, std::memory_order_relaxed);
  }
  request.startedMs = emscripten_get_now();
  request.operation(request.context);
  request.finishedMs = emscripten_get_now();
#ifdef DONNER_TEST_GPU_OWNER_TRACE
  RecordCreationTrace(request);
#endif
  gOwnerWaitStats[kFinished].fetch_add(1, std::memory_order_relaxed);
  emscripten_futex_wake(&request.complete, INT_MAX);
  // This release is the callback's final access to the caller-owned request.
  request.complete.store(1, std::memory_order_release);
}

void EnqueueRequest(void* context) {
  auto& request = *static_cast<OwnerRequest*>(context);
  request.receivedMs = emscripten_get_now();
#ifdef DONNER_TEST_GPU_OWNER_TRACE
  if (request.creationTraceEpoch) {
    request.turnsAtReceive = OwnerTurns();
    request.canvasActiveAtReceive = gCanvasFrameActive;
  }
#endif
  if (gCanvasFrameActive) {
    gOwnerWaitStats[kReceivedDuringCanvasFrame].fetch_add(1, std::memory_order_relaxed);
  }
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
    FatalAbort("gpu-owner.register",
               {{"stopped", gStopped.load()}, {"ownerRegistered", gOwner.load() != pthread_t{}}});
  }
  SetFatalAbortThreadRole("gpu-owner");
  // Prewarm notifications before clients can hold application locks while dispatching.
  if (!gNotificationQueue) {
    gNotificationQueue = em_proxying_queue_create();
  }
  if (!gNotificationQueue ||
      !emscripten_proxy_async(gNotificationQueue, pthread_self(), [](void*) {}, nullptr)) {
    FatalAbort("gpu-owner.notification-queue", {{"queueCreated", gNotificationQueue != nullptr}});
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
      'resourceCreation': Atomics.load(HEAPU32, index + 25),
      'resourceDestruction': Atomics.load(HEAPU32, index + 26),
      'bufferWrite': Atomics.load(HEAPU32, index + 27),
      'textureWrite': Atomics.load(HEAPU32, index + 28),
      'resourceSharing': Atomics.load(HEAPU32, index + 29),
      'commandBatch': Atomics.load(HEAPU32, index + 30),
      'queueSubmit': Atomics.load(HEAPU32, index + 31),
      'bindGroupConstruction': Atomics.load(HEAPU32, index + 32),
      'pipelineConstruction': Atomics.load(HEAPU32, index + 33),
      'bufferWriteSizes': ({
        'zero': Atomics.load(HEAPU32, index + 34),
        'bytes1To64': Atomics.load(HEAPU32, index + 35),
        'bytes65To256': Atomics.load(HEAPU32, index + 36),
        'bytes257To512': Atomics.load(HEAPU32, index + 37),
        'bytes513To4096': Atomics.load(HEAPU32, index + 38),
        'bytes4097To65536': Atomics.load(HEAPU32, index + 39),
        'bytesAbove65536': Atomics.load(HEAPU32, index + 40),
        'exact256': Atomics.load(HEAPU32, index + 41),
        'exact448': Atomics.load(HEAPU32, index + 42),
        'totalBytes': Atomics.load(HEAPU32, index + 43),
        'bytesSaturated': Atomics.load(HEAPU32, index + 43) === 4294967295,
      }),
      'deliveryMs': Atomics.load(HEAPU32, index + 44) / 1000,
      'ownerQueueMs': Atomics.load(HEAPU32, index + 45) / 1000,
      'executeMs': Atomics.load(HEAPU32, index + 46) / 1000,
      'completionWakeMs': Atomics.load(HEAPU32, index + 47) / 1000,
      'startLatency': ({
        'upTo50Us': Atomics.load(HEAPU32, index + 48),
        'upTo100Us': Atomics.load(HEAPU32, index + 49),
        'upTo250Us': Atomics.load(HEAPU32, index + 50),
        'upTo1Ms': Atomics.load(HEAPU32, index + 51),
        'upTo4Ms': Atomics.load(HEAPU32, index + 52),
        'upTo16Ms': Atomics.load(HEAPU32, index + 53),
        'upTo64Ms': Atomics.load(HEAPU32, index + 54),
        'over64Ms': Atomics.load(HEAPU32, index + 55),
      }),
      'canvasFrameActiveMs': Atomics.load(HEAPU32, index + 56) / 1000,
      'canvasFrames': Atomics.load(HEAPU32, index + 57),
      'receivedDuringCanvasFrame': Atomics.load(HEAPU32, index + 58),
      'timingSaturated': [10, 11, 20, 44, 45, 46, 47, 56].some(
          (offset) => Atomics.load(HEAPU32, index + offset) === 4294967295),
    });
  }, gOwnerWaitStats.data());
#ifdef DONNER_TEST_GPU_OWNER_TRACE
  EM_ASM({
    const epoch = Number($0) / 4;
    const base = Number($1) / 8;
    const width = $2;
    const renderCapacity = $3;
    const requestCapacity = $4;
    const stride = 6 + requestCapacity * width;
    const ownerStats = Number($5) / 4;
    const requestedIndex = $6;
    const finishedIndex = $7;
    globalThis['__donnerGpuOwnerCreationTrace'] = (mode, pressMs, releaseMs) => {
      const active = Atomics.load(HEAPU32, epoch);
      Atomics.store(HEAPU32, epoch, (active + (active & 1)) >>> 0);
      if (mode === 'arm') {
        HEAPF64[base] = HEAPF64[base + 1] = HEAPF64[base + 2] = HEAPF64[base + 3] = 0;
        Atomics.store(HEAPU32, epoch, ((active | 1) + 2) >>> 0);
        return null;
      }
      const hasPhysicalBounds = Number.isFinite(pressMs) && Number.isFinite(releaseMs)
        && pressMs < releaseMs;
      const count = Math.min(HEAPF64[base], renderCapacity);
      const windows = [];
      for (let i = 0; i < count; ++i) {
        const header = base + 4 + i * stride;
        const requests = HEAPF64[header + 1];
        const firstSentMs = HEAPF64[header + 2];
        const lastSentMs = HEAPF64[header + 3];
        const rows = [];
        let heldDeliveryMs = 0;
        let heldRequests = 0;
        for (let n = 0; n < Math.min(requests, requestCapacity); ++n) {
          const row = header + 6 + n * width;
          rows.push(row);
          const sent = HEAPF64[row + 1];
          if (hasPhysicalBounds && pressMs <= sent && sent < releaseMs) {
            heldDeliveryMs += Math.max(0, HEAPF64[row + 2] - sent);
            ++heldRequests;
          }
        }
        windows.push({
          rows,
          summary: {
            'renderOrdinal': HEAPF64[header],
            'requests': requests,
            'omittedRequests': Math.max(0, requests - requestCapacity),
            'firstSentMs': firstSentMs,
            'lastSentMs': lastSentMs,
            'deliveryMs': HEAPF64[header + 4],
            'firstOmittedSentMs': HEAPF64[header + 5] || null,
            'heldDeliveryMs': heldDeliveryMs,
            'heldRequests': heldRequests,
          },
        });
      }
      const ranked = windows.filter(({summary}) => !hasPhysicalBounds || summary['heldRequests'])
        .sort((left, right) => hasPhysicalBounds
          ? right.summary['heldDeliveryMs'] - left.summary['heldDeliveryMs']
          : right.summary['deliveryMs'] - left.summary['deliveryMs']);
      const records = [];
      for (const row of ranked.slice(0, 2).flatMap(({rows}) => rows)) {
        records.push({
          'api': ['none', 'create_buffer', 'create_texture', 'create_texture_view',
                  'create_sampler', 'create_shader_module'][HEAPF64[row]],
          'sentMs': HEAPF64[row + 1],
          'receivedMs': HEAPF64[row + 2],
          'startedMs': HEAPF64[row + 3],
          'finishedMs': HEAPF64[row + 4],
          'turnsAtSend': Array.from(HEAPF64.subarray(row + 5, row + 8)),
          'turnsAtReceive': Array.from(HEAPF64.subarray(row + 8, row + 11)),
          'viewSite': HEAPF64[row + 11],
          'logicalDevice': HEAPF64[row + 12],
          'textureLifetimeId': HEAPF64[row + 13],
          'renderOrdinal': HEAPF64[row + 14],
          'operationKind': HEAPF64[row + 15],
          'canvasActiveAtReceive': HEAPF64[row + 16] !== 0,
        });
      }
      const outstanding = Math.max(0, Atomics.load(HEAPU32, ownerStats + requestedIndex)
        - Atomics.load(HEAPU32, ownerStats + finishedIndex));
      const firstOmittedRenderSentMs = HEAPF64[base + 3] || null;
      const couldMissHeld = (firstOmitted) => firstOmitted !== null && firstOmitted < releaseMs;
      return ({
        'turnOrder': ['canvas', 'pulse', 'waitPump'],
        'hasPhysicalBounds': hasPhysicalBounds,
        'unscopedRequests': HEAPF64[base + 1],
        'omittedRenderRequests': HEAPF64[base + 2],
        'firstOmittedRenderSentMs': firstOmittedRenderSentMs,
        'outstandingAtStop': outstanding,
        'incomplete': !hasPhysicalBounds || ranked.length === 0
          || couldMissHeld(firstOmittedRenderSentMs) || outstanding > 0
          || windows.some(({summary}) => couldMissHeld(summary['firstOmittedSentMs'])),
        'windows': windows.map(({summary}) => summary),
        'retainedRenderOrdinals': ranked.slice(0, 2).map(({summary}) => summary['renderOrdinal']),
        'records': records,
      });
    };
  }, &gCreationTraceEpoch, &gDispatchTrace, kTraceWords, kTraceRenderCapacity, kTraceRequestCapacity,
     gOwnerWaitStats.data(), kRequested, kFinished);
#endif
  // clang-format on
#endif
}

void RecordBrowserBufferWriteSize([[maybe_unused]] std::size_t bytes) {
#ifdef __EMSCRIPTEN_PTHREADS__
  const pthread_t owner = gOwner.load(std::memory_order_acquire);
  if (!owner || pthread_equal(owner, pthread_self()) || gStopped.load(std::memory_order_acquire)) {
    return;
  }
  constexpr std::array<std::size_t, 7> limits = {
      0, 64, 256, 512, 4096, 65536, std::numeric_limits<std::size_t>::max()};
  const auto bucket = static_cast<std::size_t>(
      std::lower_bound(limits.begin(), limits.end(), bytes) - limits.begin());
  gOwnerWaitStats[kBufferWriteSizeBucketStart + bucket].fetch_add(1, std::memory_order_relaxed);
  if (bytes == 256) {
    gOwnerWaitStats[kBufferWrite256].fetch_add(1, std::memory_order_relaxed);
  }
  if (bytes == 448) {
    gOwnerWaitStats[kBufferWrite448].fetch_add(1, std::memory_order_relaxed);
  }
  AddSaturatingCounter(kBufferWriteBytes, static_cast<double>(bytes));
#endif
}

#ifdef DONNER_TEST_GPU_OWNER_TRACE
BrowserGpuCreationTraceRenderScope::BrowserGpuCreationTraceRenderScope() {
#ifdef __EMSCRIPTEN_PTHREADS__
  previous_ = gCreationTraceRender;
  if (detail::CreationTraceIsArmed()) {
    gCreationTraceRender = gNextCreationTraceRender.fetch_add(1, std::memory_order_relaxed);
  }
#endif
}

BrowserGpuCreationTraceRenderScope::~BrowserGpuCreationTraceRenderScope() {
#ifdef __EMSCRIPTEN_PTHREADS__
  gCreationTraceRender = previous_;
#endif
}

BrowserGpuCreationApi SetBrowserGpuCreationApi([[maybe_unused]] BrowserGpuCreationApi api) {
#ifdef __EMSCRIPTEN_PTHREADS__
  return std::exchange(gCreationApi, api);
#else
  return BrowserGpuCreationApi::None;
#endif
}

#endif

void RunOnBrowserGpuOwner(void (*operation)(void*), void* context,
                          [[maybe_unused]] BrowserGpuOperationKind kind) {
#ifdef __EMSCRIPTEN_PTHREADS__
  if (gStopped.load(std::memory_order_acquire)) {
    FatalAbort("gpu-owner.dispatch-after-stop", {{"kind", static_cast<std::int64_t>(kind)},
                                                 {"requested", gOwnerWaitStats[kRequested].load()},
                                                 {"finished", gOwnerWaitStats[kFinished].load()}});
  }
  const pthread_t owner = gOwner.load(std::memory_order_acquire);
  if (owner && !pthread_equal(owner, pthread_self())) {
    const double requestStartedMs = emscripten_get_now();
    OwnerRequest request{operation, context, {}, {}};
#ifdef DONNER_TEST_GPU_OWNER_TRACE
    const uint32_t epoch = gCreationTraceEpoch.load(std::memory_order_relaxed);
    if (epoch & 1u) {
      request.creationTraceEpoch = epoch;
      request.kind = kind;
      request.creationApi = gCreationApi;
      request.identity = detail::gCreationTraceIdentity;
      request.render = gCreationTraceRender;
      request.sentMs = requestStartedMs;
      request.turnsAtSend = OwnerTurns();
    }
#endif
    gOwnerWaitStats[kRequested].fetch_add(1, std::memory_order_relaxed);
    ++gRequestedByThisThread;
    gOwnerWaitStats[OperationCounter(kind)].fetch_add(1, std::memory_order_relaxed);
    // The inline budget bounds repeated requests within the SDK's otherwise unbounded drain.
    if (!emscripten_proxy_async(gNotificationQueue, owner, &EnqueueRequest, &request)) {
      FatalAbort("gpu-owner.dispatch-proxy", {{"kind", static_cast<std::int64_t>(kind)},
                                              {"requested", gOwnerWaitStats[kRequested].load()},
                                              {"stopped", gStopped.load()}});
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
  if (!IsBrowserGpuOwnerThread()) {
    FatalAbort("gpu-owner.canvas-frame-off-owner",
               {{"stopped", gStopped.load()}, {"active", active}});
  }
  if (gCanvasFrameActive == active || (active && gProcessingBatch)) {
    FatalAbort("gpu-owner.canvas-frame", {{"active", active},
                                          {"wasActive", gCanvasFrameActive},
                                          {"processingBatch", gProcessingBatch}});
  }
  gCanvasFrameActive = active;
  const double nowMs = emscripten_get_now();
  if (active) {
    gCanvasFrameStartedMs = nowMs;
  } else {
    AddPhase(kCanvasFrameActiveUs, gCanvasFrameStartedMs, nowMs);
    gOwnerWaitStats[kCanvasFrames].fetch_add(1, std::memory_order_relaxed);
  }
  if (!active && !gReady.empty()) {
    SchedulePulse();
  }
#endif
}

void StopBrowserGpuOwner() {
#ifdef __EMSCRIPTEN_PTHREADS__
  if (!IsBrowserGpuOwnerThread()) {
    FatalAbort("gpu-owner.stop-off-owner", {{"stopped", gStopped.load()}});
  }
  if (gProcessingBatch || gCanvasFrameActive || !gReady.empty() ||
      gOwnerWaitStats[kRequested].load() != gOwnerWaitStats[kFinished].load()) {
    FatalAbort("gpu-owner.stop", {{"processingBatch", gProcessingBatch},
                                  {"canvasFrameActive", gCanvasFrameActive},
                                  {"readyEmpty", gReady.empty()},
                                  {"requested", gOwnerWaitStats[kRequested].load()},
                                  {"finished", gOwnerWaitStats[kFinished].load()}});
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

BrowserGpuOwnerDispatchTotals ReadBrowserGpuOwnerDispatchTotals() {
#ifdef __EMSCRIPTEN_PTHREADS__
  const auto read = [](std::size_t index) {
    return gOwnerWaitStats[index].load(std::memory_order_relaxed);
  };
  BrowserGpuOwnerDispatchTotals totals{
      .requested = read(kRequested),
      .waitSliceTimeouts = read(kFutexTimeouts),
      .dispatchWaitUs = read(kTotalDispatchWaitUs),
      .requestedByCallingThread = gRequestedByThisThread,
      .deliveryUs = read(kDeliveryUs),
      .ownerQueueUs = read(kOwnerQueueUs),
      .executeUs = read(kExecuteUs),
      .completionWakeUs = read(kCompletionWakeUs),
      .canvasFrameActiveUs = read(kCanvasFrameActiveUs),
      .canvasFrames = read(kCanvasFrames),
      .receivedDuringCanvasFrame = read(kReceivedDuringCanvasFrame),
  };
  for (std::size_t kind = 0; kind < kBrowserGpuOperationKindCount; ++kind) {
    totals.requestedByKind[kind] =
        read(OperationCounter(static_cast<BrowserGpuOperationKind>(kind)));
  }
  for (std::size_t bucket = 0; bucket < totals.startLatencyBuckets.size(); ++bucket) {
    totals.startLatencyBuckets[bucket] = read(kStartLatencyBucketStart + bucket);
  }
  return totals;
#else
  return {};
#endif
}

}  // namespace donner::gpu::browser
