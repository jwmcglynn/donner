#pragma once
/// @file
/// Dispatches browser GPU primitives to the editor's canvas-owning application thread.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace donner::gpu::browser {

/// Cross-thread dispatch categories, not object/command counts; owner-local calls are excluded.
/// BindGroupConstruction counts whole descriptors; PipelineConstruction counts begin/entry/finish
/// calls. CommandBatch counts batches.
/// OwnershipQuery includes the combined ownership/loss query.
enum class BrowserGpuOperationKind {
  Other,
  OwnershipQuery,
  DeviceLossQuery,
  CompletionQuery,
  ResourceCreation,
  ResourceDestruction,
  BufferWrite,
  TextureWrite,
  ResourceSharing,
  CommandBatch,
  QueueSubmit,
  BindGroupConstruction,
  PipelineConstruction,
};

#ifdef DONNER_TEST_GPU_OWNER_TRACE
/// Concrete resource-creation API for the opt-in owner delivery diagnostic.
enum class BrowserGpuCreationApi : uint32_t {
  None,
  Buffer,
  Texture,
  TextureView,
  Sampler,
  ShaderModule,
};

/// Change this caller's diagnostic tag without changing the owner dispatch API.
/// @param api Creation call to tag during the current scope.
/// @return Previous caller tag, for restoration on scope exit.
BrowserGpuCreationApi SetBrowserGpuCreationApi(BrowserGpuCreationApi api);

/// Tags resource requests issued by one admitted render iteration, when tracing is armed.
class BrowserGpuCreationTraceRenderScope {
public:
  BrowserGpuCreationTraceRenderScope();
  ~BrowserGpuCreationTraceRenderScope();
  BrowserGpuCreationTraceRenderScope(const BrowserGpuCreationTraceRenderScope&) = delete;
  BrowserGpuCreationTraceRenderScope& operator=(const BrowserGpuCreationTraceRenderScope&) = delete;

private:
  uint32_t previous_ = 0;
};

#endif

/// Number of \ref BrowserGpuOperationKind values; per-kind totals are indexed by the enum value.
inline constexpr std::size_t kBrowserGpuOperationKindCount =
    static_cast<std::size_t>(BrowserGpuOperationKind::PipelineConstruction) + 1;

/// Inclusive upper bounds, in microseconds, of the request-to-start latency buckets of
/// \ref BrowserGpuOwnerDispatchTotals; a last bucket holds every longer latency.
inline constexpr std::array<std::uint32_t, 7> kBrowserGpuOwnerStartLatencyBucketUpperUs = {
    50, 100, 250, 1000, 4000, 16000, 64000};

/// Record an attempted cross-owner buffer-write size without reading payloads or resource ids.
/// Owner-local calls are excluded. @param bytes Borrowed payload size before dispatch.
void RecordBrowserBufferWriteSize(std::size_t bytes);

/// Register the calling application thread before creating any browser GPU devices or workers.
void RegisterBrowserGpuOwner();

/** Execute bounded browser work synchronously on the registered owner.
 * Without a registered owner, standalone modules execute on the calling thread.
 * @param operation Callback that must not allocate, lock, yield, or call arbitrary C++ code.
 * @param context Caller-owned data, valid until the operation completes.
 * @param kind Diagnostic category; it does not affect dispatch.
 * Inline notification work stops starting callbacks after 32 calls or 2 ms of callback CPU;
 * a later owner pulse renews that budget. A running callback is not preempted.
 * A failed dispatch or expired deadline terminates the module rather than leaving borrowed data
 * reachable by a delayed callback. The owner remains alive until all clients are destroyed.
 */
void RunOnBrowserGpuOwner(void (*operation)(void*), void* context,
                          BrowserGpuOperationKind kind = BrowserGpuOperationKind::Other);

/** Invoke a stack-owned callback without allocating a type-erased function.
 * @param operation Nonblocking browser primitive or prepared bounded command batch.
 * @param kind Diagnostic category; it does not affect dispatch.
 */
template <typename Function>
void RunOnBrowserGpuOwner(Function&& operation,
                          BrowserGpuOperationKind kind = BrowserGpuOperationKind::Other) {
  using Callback = std::remove_reference_t<Function>;
  RunOnBrowserGpuOwner([](void* context) { (*static_cast<Callback*>(context))(); }, &operation,
                       kind);
}

/// Whether the calling thread is the registered browser GPU owner.
[[nodiscard]] bool IsBrowserGpuOwnerThread();

/// Process one bounded batch of GPU commands while the owner is in a runtime wait.
/// The editor's futex adapter calls this between finite wait slices; it does not run JS promises.
void ProcessBrowserGpuOwnerWait();

/// Whether this owner is already executing a GPU batch; nested editor frames must defer.
[[nodiscard]] bool IsBrowserGpuOwnerDispatchActive();

/// Defer asynchronous command batches across canvas acquisition, drawing and retirement.
/// Idle and pre-acquisition waits may service GPU work; futex dependency progress is also allowed.
/// @param active True on frame entry, false on exit; called only by the owner, without nesting.
void SetBrowserGpuOwnerCanvasFrameActive(bool active);

/// Stop delivery on the owner after every client has joined and released its resources.
/// This is terminal: later GPU calls fail closed instead of reverting to standalone dispatch.
void StopBrowserGpuOwner();

/// Whether this module has registered a shared browser GPU owner.
[[nodiscard]] bool UsesBrowserGpuOwner();

/// Running totals of cross-thread owner dispatches from every client thread. Owner-local calls are
/// excluded. Each field wraps or saturates as the matching `__donnerReadGpuOwnerWaitStats` field.
struct BrowserGpuOwnerDispatchTotals {
  /// Dispatches requested; each blocks its calling thread until the owner has run it.
  std::uint32_t requested = 0;
  /// One-millisecond wait slices that expired while a dispatch was still pending.
  std::uint32_t waitSliceTimeouts = 0;
  /// Saturating wall time requesting threads spent blocked on dispatches, in microseconds.
  std::uint32_t dispatchWaitUs = 0;
  /// Dispatches requested by the thread reading the totals. Thread-local, so it wraps
  /// independently of \ref requested.
  std::uint32_t requestedByCallingThread = 0;
  /// Dispatches requested from every client thread, indexed by \ref BrowserGpuOperationKind.
  std::array<std::uint32_t, kBrowserGpuOperationKindCount> requestedByKind{};

  // The phases below partition each completed dispatch's blocked time, are added when the
  // requesting thread observes completion, and saturate in microseconds. Phase boundaries are
  // read on the requesting thread and on the owner; pthread builds report absolute time on every
  // thread, so they compare across threads to within the browser's timer resolution.

  /// From the request until the owner received it: the owner was busy elsewhere or waking.
  std::uint32_t deliveryUs = 0;
  /// From the owner's receipt until execution started: queued behind an active canvas frame, a
  /// running batch, the inline budget or earlier requests.
  std::uint32_t ownerQueueUs = 0;
  /// Execution of the dispatched operations on the owner.
  std::uint32_t executeUs = 0;
  /// From the owner's completion until the requesting thread observed it.
  std::uint32_t completionWakeUs = 0;
  /// Completed dispatches by request-to-start latency (delivery plus owner queue), bucketed by
  /// \ref kBrowserGpuOwnerStartLatencyBucketUpperUs with a final unbounded bucket.
  std::array<std::uint32_t, kBrowserGpuOwnerStartLatencyBucketUpperUs.size() + 1>
      startLatencyBuckets{};
  /// Saturating owner time with an active canvas frame, in microseconds, added when each canvas
  /// frame ends. No inline or pulse dispatch starts then; the owner still runs requests from its
  /// runtime wait pump when the frame blocks.
  std::uint32_t canvasFrameActiveUs = 0;
  /// Canvas frames that ended.
  std::uint32_t canvasFrames = 0;
  /// Requests the owner received while a canvas frame was active. The owner receives requests
  /// then only inside a runtime wait, whose pump runs them at once, so these add little owner
  /// queue time.
  std::uint32_t receivedDuringCanvasFrame = 0;
};

/// Read the dispatch totals without synchronizing with dispatches in flight. All zero without a
/// registered owner and outside pthread builds.
[[nodiscard]] BrowserGpuOwnerDispatchTotals ReadBrowserGpuOwnerDispatchTotals();

}  // namespace donner::gpu::browser
