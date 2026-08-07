#pragma once
/// @file
///
/// `AsyncRenderer` owns a `svg::Renderer` and runs compositor rendering plus
/// any final presentation snapshot handoff on a dedicated worker thread so
/// heavy renders don't block the UI thread.
///
/// ## Threading model
///
/// The worker thread owns the Renderer for its entire lifetime. Backends
/// with thread-affined GPU objects depend on device, pipeline, texture, and
/// readback use staying on that thread.
///
/// The worker additionally takes exclusive ownership of the
/// `SVGDocument` during an active render. The UI thread must not
/// mutate the document while a render is in flight.
///
/// UI thread flow per frame:
/// 1. `pollResult()` - if a render just finished, pick up the bitmap.
/// 2. If NOT busy: process mutations via `flushFrame()`.
/// 3. If NOT busy AND a new render is needed: `requestRender()`.
/// 4. If busy: skip flushFrame, leave pending mutations in the queue.
///    They apply on the next idle frame. Input (drags, typing) still
///    gets processed and queued - just not dispatched to the ECS.
/// 5. The editor overlay is the exception: it may take guarded document access for immediate
///    presentation chrome. That access serializes behind the worker's render access instead of
///    racing it, and must not be taken while holding `AsyncRenderer`'s mutex.
///
/// The safety invariant: between `requestRender()` and a non-`nullopt`
/// return from `pollResult()`, the UI thread must not mutate the
/// `SVGDocument`. Registry-reading UI paths should normally gate on
/// `!isBusy()` unless they are using guarded access for immediate overlay presentation. The UI
/// thread must not call any method on the worker `Renderer` at any time - it lives on the worker.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#ifdef DONNER_WASM_WORKER_SURFACE
#include <emscripten/proxying.h>
#include <pthread.h>
#endif

#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {
class SVGDocument;
namespace compositor {
class CompositorController;
}
}  // namespace donner::svg

namespace donner::geode {
class GeodeDevice;
}

namespace donner::editor {

#ifdef DONNER_WASM_WORKER_SURFACE
struct WasmWorkerRuntime;
struct WasmWorkerRuntimeInitControl;
#endif

/// Non-null renderer/document handoff for a render request.
struct RenderLease {
  /**
   * Construct a render lease.
   *
   * @param renderer Renderer backend that remains alive until the request completes.
   * @param document Document handle to render. The lease keeps its registry handle alive while the
   * worker owns the request.
   */
  RenderLease(svg::Renderer& renderer, svg::SVGDocument document)
      : renderer_(renderer), document_(std::move(document)) {}

  /// Renderer backend for this request.
  [[nodiscard]] svg::Renderer& renderer() const { return renderer_.get(); }
  /// SVG document for this request.
  [[nodiscard]] svg::SVGDocument& document() { return document_; }
  /// SVG document for this request.
  [[nodiscard]] const svg::SVGDocument& document() const { return document_; }

private:
  std::reference_wrapper<svg::Renderer> renderer_;
  svg::SVGDocument document_;
};

/// Per-request handoff data captured at render-request time so the
/// worker has everything it needs without touching live UI state.
struct RenderRequest {
  struct DragPreview {
    Entity entity = entt::null;
    /// Additional entities moving with \ref entity under the same active drag transform.
    std::vector<Entity> extraEntities;
    /// Which interaction phase drove this preview. `Selection` means the
    /// editor is pre-warming a layer for the selected entity before any
    /// drag begins. `ActiveDrag` means the user is actively dragging - the
    /// DOM's transform attribute already reflects the cursor delta. The
    /// compositor stamps the correct `InteractionHint` on the entity based
    /// on this field so downstream introspection stays accurate.
    svg::compositor::InteractionHint interactionKind = svg::compositor::InteractionHint::ActiveDrag;
    /// Active drag translation represented by this request. Selection prewarms use zero.
    Vector2d translation = Vector2d::Zero();
    /// Active affine transform represented by this request, relative to the
    /// drag-start cached document. Selection prewarms use identity.
    Transform2d documentFromCachedDocument = Transform2d();
    /// Monotonic id for the active drag gesture. Selection prewarms use zero.
    std::uint64_t dragGeneration = 0;
    /// True when this request must re-rasterize the promoted drag layer instead of only publishing
    /// updated compose metadata. Used for scheduler-requested affine recaptures that keep scaled
    /// drag previews crisp.
    bool forceLayerRasterization = false;
  };

  /**
   * Construct a render request.
   *
   * @param renderer Renderer backend that remains alive until the request completes.
   * @param document Document handle to render. The request keeps its registry handle alive while
   * the worker owns the request.
   */
  RenderRequest(svg::Renderer& renderer, svg::SVGDocument document)
      : lease(renderer, std::move(document)) {}

  /// Non-null renderer/document lease for the worker handoff.
  RenderLease lease;
  /// Internal timestamp captured by `requestRender()` for queue-latency diagnostics.
  std::chrono::steady_clock::time_point queuedAt;
  /// Document frame version snapshotted at request time so the UI can
  /// match the landed bitmap with other same-version assets.
  std::uint64_t version = 0;
  /// Generation counter from `AsyncSVGDocument::documentGeneration()`. Bumps
  /// only when the inner document is fully replaced, so the worker can reset
  /// entity-keyed compositor state without treating every frame mutation as a
  /// replacement.
  std::uint64_t documentGeneration = 0;
  /// Entity remap for structurally equivalent document replacement. When
  /// present, the worker remaps compositor state instead of fully resetting
  /// layer bitmaps and static segments.
  std::unordered_map<Entity, Entity> structuralRemap;
  /// Snapshot of the selection at request time (used for overlay chrome).
  /// The worker holds this optional by value, so if the UI thread clears
  /// the selection mid-render the worker still draws the pre-render chrome.
  std::optional<svg::SVGElement> selection;
  /// Frozen Select-mode chrome appended to the same worker texture as the document.
  /// Keeping these pixels in one WebGPU surface epoch prevents browser compositor
  /// timing from separating a dragged shape from its outline.
  std::optional<SelectionChromeSnapshot> directSurfaceSelectionChrome;
  /// Currently-selected entity (if any) that the compositor should keep
  /// promoted across renders. The compositor demotes the previous entity and
  /// promotes this one when it changes. Allows pre-warming on selection so
  /// the subsequent drag frame has cached bg/fg/layer bitmaps ready. The
  /// compositor stays alive across drag → idle transitions for as long as
  /// this stays non-null and pointing at the same entity.
  Entity selectedEntity = entt::null;
  /// Optional in-progress drag preview rendered through the compositor fast path.
  std::optional<DragPreview> dragPreview;
  /// Raster viewport for this request. The UI thread computes it from the
  /// editor camera so the worker can render a bounded high-zoom output
  /// surface without reading live UI state.
  EditorRasterViewport rasterViewport;
  /// Live editor viewport \ref rasterViewport was derived from.
  ///
  /// Carried through the worker untouched so presentation can place the
  /// resulting pixels with the same screen transform they were rasterized
  /// against. Placing them through a later viewport would stretch a
  /// viewport-bounded raster past the document region it actually covers.
  ViewportState viewport;
  /// True when this request should produce only a low-resolution full-document overview infill.
  ///
  /// The worker still keeps the selected entity promoted, but skips the composited split preview
  /// and publishes a full-canvas tile. The UI uploads it into the retained overview cache without
  /// replacing active viewport-bounded tiles.
  bool overviewInfillOnly = false;
  /// Capture a CPU-readable copy of the fully composed frame.
  ///
  /// Geode normally publishes GPU texture tiles without readback. Diagnostics, replay tools, and
  /// pixel-asserting tests set this flag when they explicitly need a bitmap.
  bool captureCpuSnapshot = false;
};

/// Final full-canvas snapshot work needed after compositor rendering.
struct PresentationSnapshotPlan {
  /// Capture a CPU bitmap with `Renderer::takeSnapshot()`.
  bool captureCpuSnapshot = false;
  /// Capture a GPU texture snapshot with `Renderer::takeTextureSnapshot()`.
  bool captureTextureSnapshot = false;
};

/// GPU-texture lifetime required by one presentation handoff.
enum class TextureSnapshotHandoff : std::uint8_t {
  BorrowCurrentFrame,
  TakeOwnership,
};

/**
 * Choose whether a texture handoff borrows the current frame or takes ownership.
 *
 * @param consumerOutlivesCurrentFrame True when presentation retains the texture after the
 *   synchronous handoff returns.
 * @return BorrowCurrentFrame for synchronous copies, otherwise TakeOwnership.
 */
[[nodiscard]] TextureSnapshotHandoff ChooseTextureSnapshotHandoff(
    bool consumerOutlivesCurrentFrame) noexcept;

/**
 * Choose final full-canvas snapshot work for a render result.
 *
 * @param hasCompositedPreview True when compositor tiles already provide the presented pixels.
 * @param requiresTextureSnapshotPresentation True when presentation must remain on GPU textures.
 * @param captureCpuSnapshot True when the caller explicitly requested a CPU-readable frame.
 * @return The final snapshot plan for this worker iteration.
 */
[[nodiscard]] PresentationSnapshotPlan ChoosePresentationSnapshotPlan(
    bool hasCompositedPreview, bool requiresTextureSnapshotPresentation, bool captureCpuSnapshot);

/// Typed failure observed while presenting a worker-owned WebGPU surface.
enum class WorkerSurfaceFailureKind : std::uint8_t {
  Timeout,
  OutdatedOrLost,
  Setup,
  Incompatible,
  Fatal,
};

/// Recovery action for one failed worker-surface presentation attempt.
enum class WorkerSurfaceRecoveryAction : std::uint8_t {
  Retry,
  ReconfigureAndRetry,
  RecreateAndRetry,
  TerminalFailure,
};

/// Final presentation outcome published with one worker render result.
enum class DirectSurfacePresentationOutcome : std::uint8_t {
  None,
  Presented,
  RetryAfterBackoff,
  Unavailable,
};

/**
 * Resolve a bounded terminal worker-surface failure.
 *
 * @param failure Failure whose immediate/bounded retry policy was exhausted.
 * @return RetryAfterBackoff for transient exhaustion, otherwise Unavailable for permanent failure.
 */
[[nodiscard]] constexpr DirectSurfacePresentationOutcome DirectSurfaceTerminalOutcomeFor(
    WorkerSurfaceFailureKind failure) noexcept {
  if (failure != WorkerSurfaceFailureKind::Incompatible &&
      failure != WorkerSurfaceFailureKind::Fatal) {
    return DirectSurfacePresentationOutcome::RetryAfterBackoff;
  }
  return DirectSurfacePresentationOutcome::Unavailable;
}

/**
 * Choose bounded recovery for a worker-owned WebGPU surface failure.
 *
 * Recoverable failures get at most two follow-up worker tasks. Permanent compatibility and device
 * failures stop retrying instead of aborting or spinning the Wasm runtime.
 *
 * @param failure Typed failure from surface setup or `getCurrentTexture`.
 * @param consecutiveFailuresBeforeAttempt Failures already observed for this surface slot.
 * @return The recovery action for the failed attempt.
 */
[[nodiscard]] constexpr WorkerSurfaceRecoveryAction WorkerSurfaceRecoveryDecisionFor(
    WorkerSurfaceFailureKind failure, unsigned consecutiveFailuresBeforeAttempt) noexcept {
  constexpr unsigned kMaxFailedAttempts = 3u;
  if (failure == WorkerSurfaceFailureKind::Incompatible ||
      failure == WorkerSurfaceFailureKind::Fatal ||
      consecutiveFailuresBeforeAttempt >= kMaxFailedAttempts - 1u) {
    return WorkerSurfaceRecoveryAction::TerminalFailure;
  }

  if (failure == WorkerSurfaceFailureKind::OutdatedOrLost) {
    return WorkerSurfaceRecoveryAction::ReconfigureAndRetry;
  }
  if (failure == WorkerSurfaceFailureKind::Setup) {
    return WorkerSurfaceRecoveryAction::RecreateAndRetry;
  }
  return WorkerSurfaceRecoveryAction::Retry;
}

/// Follow-up scheduling decision after one event-loop-bounded worker task returns.
enum class WorkerTaskFollowUp : std::uint8_t {
  Park,
  SchedulePendingRequest,
};

/**
 * Choose whether a worker callback must schedule another event-loop task.
 *
 * @param hasPendingRequest True when the completed callback left a newer render request queued.
 * @param cancellationPending True when a concurrent cancellation still needs a worker callback to
 * transition the renderer to idle.
 * @return `SchedulePendingRequest` when queued worker state still needs a guaranteed callback.
 */
[[nodiscard]] WorkerTaskFollowUp ChooseWorkerTaskFollowUp(
    bool hasPendingRequest, bool cancellationPending = false) noexcept;

/// Action taken after one Wasm renderer callback finishes its bounded worker iteration.
enum class WorkerTaskCompletionDisposition : std::uint8_t {
  Park,
  ScheduleFollowUp,
  ExitWorker,
};

/**
 * Choose how a completed Wasm renderer callback returns control to the worker event loop.
 *
 * @param shuttingDown True when the owner is joining the worker.
 * @param hasPendingRequest True when a newer render request is queued.
 * @param cancellationPending True when cancellation still needs a worker callback.
 * @param lowPriorityWorkPending True when thumbnail or deferred-cache work is ready.
 * @param presentationBoundaryPending True when a direct-surface result needs a later task-boundary
 * acknowledgment before publication.
 */
[[nodiscard]] WorkerTaskCompletionDisposition ChooseWorkerTaskCompletionDisposition(
    bool shuttingDown, bool hasPendingRequest, bool cancellationPending,
    bool lowPriorityWorkPending, bool presentationBoundaryPending) noexcept;

/// Lifecycle of the callback-driven Wasm worker WebGPU runtime.
enum class WasmWorkerRuntimeInitializationStatus : std::uint8_t {
  Initializing,
  Ready,
  Failed,
};

/// Point at which shutdown detaches browser callbacks from their renderer owner.
enum class WasmWorkerOwnerDetachTiming : std::uint8_t {
  BeforeWorkerJoin,
  AfterWorkerJoin,
};

/**
 * Choose when shutdown may detach callback access to the renderer owner.
 *
 * A ready worker can have a task-boundary callback holding the single-flight wake gate. That
 * callback must remain attached through join so it can observe `Shutdown`, release the gate, and
 * exit its pthread. During initialization or after initialization failure, a browser Promise can
 * outlive pthread cancellation, so those callbacks must be detached before join.
 */
[[nodiscard]] constexpr WasmWorkerOwnerDetachTiming ChooseWasmWorkerOwnerDetachTiming(
    WasmWorkerRuntimeInitializationStatus status) noexcept {
  return status == WasmWorkerRuntimeInitializationStatus::Ready
             ? WasmWorkerOwnerDetachTiming::AfterWorkerJoin
             : WasmWorkerOwnerDetachTiming::BeforeWorkerJoin;
}

/// Whether a thumbnail request can still be completed by the Wasm worker runtime.
[[nodiscard]] constexpr bool CanAcceptWasmSampleThumbnailRequest(
    WasmWorkerRuntimeInitializationStatus status) noexcept {
  return status == WasmWorkerRuntimeInitializationStatus::Ready;
}

/// Queue/lifecycle action for a render or thumbnail wake.
enum class WasmWorkerRuntimeWakeAction : std::uint8_t {
  DeferUntilRuntimeReady,
  ScheduleWorkerTask,
  DetachAndCancelWorker,
  ReportRuntimeUnavailable,
};

/**
 * Choose how work interacts with callback-driven worker initialization.
 *
 * Initializing work remains represented in worker state without consuming the proxy wake gate.
 * Shutdown detaches the callback owner before cancelling the pthread, so a late browser Promise
 * cannot dereference the renderer.
 */
[[nodiscard]] WasmWorkerRuntimeWakeAction ChooseWasmWorkerRuntimeWakeAction(
    WasmWorkerRuntimeInitializationStatus status, bool shuttingDown) noexcept;

/// Safe destination for a spontaneous browser completion after owner detachment.
enum class WasmWorkerRuntimeCallbackDisposition : std::uint8_t {
  DeliverToOwner,
  DisposeDetachedResult,
};

[[nodiscard]] constexpr WasmWorkerRuntimeCallbackDisposition
ChooseWasmWorkerRuntimeCallbackDisposition(bool ownerAttached) noexcept {
  return ownerAttached ? WasmWorkerRuntimeCallbackDisposition::DeliverToOwner
                       : WasmWorkerRuntimeCallbackDisposition::DisposeDetachedResult;
}

/// Result of attempting to schedule one Wasm worker event-loop task.
enum class WorkerTaskScheduleResult : std::uint8_t {
  ScheduledOrCoalesced,
  DeferredUntilRuntimeReady,
  EnqueueRejected,
  RuntimeUnavailable,
};

/// Whether the thumbnail caller should treat a worker wake as accepted.
[[nodiscard]] constexpr bool DidAcceptWasmSampleThumbnailScheduleResult(
    WorkerTaskScheduleResult result) noexcept {
  return result == WorkerTaskScheduleResult::ScheduledOrCoalesced ||
         result == WorkerTaskScheduleResult::DeferredUntilRuntimeReady;
}

/// State resolution when work arrives after callback-driven runtime creation failed.
struct WasmWorkerRuntimeUnavailablePlan {
  bool resolveRenderState = false;
  bool dropPendingThumbnail = false;
  bool wakeOwner = false;
};

[[nodiscard]] constexpr WasmWorkerRuntimeUnavailablePlan ChooseWasmWorkerRuntimeUnavailablePlan(
    bool renderStatePending, bool thumbnailPending) noexcept {
  return WasmWorkerRuntimeUnavailablePlan{
      .resolveRenderState = renderStatePending,
      .dropPendingThumbnail = thumbnailPending,
      .wakeOwner = renderStatePending || thumbnailPending,
  };
}

/// Result of one callback-driven browser device creation attempt.
enum class WasmWorkerRuntimeFinishAction : std::uint8_t {
  Ready,
  RetryWithRgba8,
  ExitWorker,
};

/// Shutdown action after a Wasm renderer proxy enqueue is rejected.
enum class WorkerTaskShutdownDisposition : std::uint8_t {
  None,
  CancelWorkerBeforeJoin,
  ExitCurrentWorker,
};

/// Owner-side cleanup after the Wasm renderer pthread has joined.
enum class WasmWorkerRuntimeOwnerCleanupDisposition : std::uint8_t {
  ExpectWorkerCleanup,
  AbandonThreadAffinedRuntime,
};

/**
 * Choose whether the owner may destroy the worker-affined Wasm renderer runtime after join.
 *
 * @param workerCancellationRequested True when normal worker-thread cleanup could not run.
 * @param runtimeStillOwnedAfterJoin True when the worker did not release its runtime before exit.
 */
[[nodiscard]] WasmWorkerRuntimeOwnerCleanupDisposition
ChooseWasmWorkerRuntimeOwnerCleanupDisposition(bool workerCancellationRequested,
                                               bool runtimeStillOwnedAfterJoin) noexcept;

/// Recovery actions after Emscripten rejects a renderer-worker proxy enqueue.
struct WorkerTaskEnqueueFailurePlan {
  bool resolveRenderState = false;
  bool dropPendingThumbnail = false;
  bool wakeOwnerForRetry = false;
  bool reportSurfaceUnavailable = false;
  WorkerTaskShutdownDisposition shutdownDisposition = WorkerTaskShutdownDisposition::None;
};

/**
 * Choose bounded recovery for a rejected renderer-worker proxy enqueue.
 *
 * @param shuttingDown True when the owner is already joining the worker.
 * @param renderStatePending True for `RenderingState` or `CancellingState`.
 * @param thumbnailPending True when low-priority work has not reached the worker.
 * @param consecutiveFailureCount Number of consecutive rejected enqueue attempts, including this
 * one.
 * @param enqueueAttemptFromWorker True when the rejected follow-up was queued by the worker
 * callback that can exit directly.
 */
[[nodiscard]] WorkerTaskEnqueueFailurePlan ChooseWorkerTaskEnqueueFailurePlan(
    bool shuttingDown, bool renderStatePending, bool thumbnailPending,
    std::uint64_t consecutiveFailureCount, bool enqueueAttemptFromWorker = false) noexcept;

/// Single-flight gate for event-loop worker wakeups. Pointer input may replace the pending render
/// request many times while one callback is queued or running; only the newest request needs a
/// follow-up callback.
class WorkerTaskWakeGate {
public:
  /// Mark a callback queued/running. Returns false when one already owns the wakeup slot.
  [[nodiscard]] bool trySchedule() noexcept {
    bool expected = false;
    return scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
  }

  /**
   * Finish the enqueue attempt that followed a successful \ref trySchedule call.
   *
   * A rejected enqueue never created a callback that could release the slot. Roll it back here so
   * shutdown and a later request cannot remain coalesced behind nonexistent work.
   *
   * @param queued True when the platform accepted the callback.
   */
  void completeEnqueue(bool queued) noexcept {
    if (!queued) {
      completeTask();
    }
  }

  /// Release the slot before checking worker state for follow-up work.
  void completeTask() noexcept { scheduled_.store(false, std::memory_order_release); }

private:
  std::atomic_bool scheduled_{false};
};

/// Presentation payload plus the document version it was rendered from.
struct RenderResult {
  /// Internal timing split for one async worker iteration.
  struct WorkerTimingBreakdown {
    /// Time from UI-thread request submission until the worker dequeues the request.
    double queueWaitMs = 0.0;
    /// Worker-thread preflight between dequeue and the measured render iteration.
    double dequeueToStartMs = 0.0;
    /// Time before `CompositorController::renderFrame`, including compositor selection setup.
    double setupMs = 0.0;
    /// Time spent in `CompositorController::renderFrame`.
    double renderFrameMs = 0.0;
    /// Time spent building composited-preview tile metadata/payloads.
    double buildPreviewMs = 0.0;
    /// Time spent taking the final fallback canvas snapshot, when needed.
    double finalSnapshotMs = 0.0;
    /// Time spent borrowing/copying the worker texture into the browser presentation surface.
    double presentMs = 0.0;
    /// Time spent copying compositor diagnostics for editor panels.
    double diagnosticsMs = 0.0;
    /// Time from worker result completion until the UI thread polls it.
    double pollDelayMs = 0.0;
    /// GPU-to-CPU readbacks performed by the worker renderer and its offscreen instances.
    int readbackCount = 0;
    /// Legacy device-poll iterations used while waiting for those readbacks.
    int readbackPollIterations = 0;
    /// True when browser readbacks used the event-driven timed WaitAny path.
    bool usedTimedWaitAny = false;
  };

  /// One composite tile from the worker's `CompositorController::
  /// snapshotCompositorTiles()` snapshot (design doc 0033 §M2C). The
  /// editor uploads one GL texture per tile (keyed on `id`) and
  /// blits each tile at its canvas offset. Immediate tiles intentionally use
  /// transient ids and always carry a fresh payload. Geometry fields are
  /// doc-unit quantities so the editor can scale them by the current
  /// `pixelsPerDocUnit` during canvas-resize debouncing.
  struct CompositedTile {
    enum class Kind : std::uint8_t { Segment, Layer, Immediate };

    Kind kind = Kind::Segment;
    /// Stable id from the compositor - `"seg:{i}"` or
    /// `"layer:{entity}"`. The editor's per-tile texture cache uses
    /// this to reuse GL textures across frames when the tile's
    /// `generation` hasn't bumped.
    std::string id;
    /// Promoted entity represented by this layer tile. Null for static segment and full-canvas
    /// tiles. The presenter uses this to suppress stale cached pixels for a selected element that
    /// became non-rendering while keeping editor chrome visible.
    Entity layerEntity = entt::null;
    /// Monotonic generation from the compositor. Editor re-uploads
    /// the bitmap only when this advances.
    std::uint64_t generation = 0;
    /// Source bitmap; uploaded as the tile's GL texture content.
    svg::RendererBitmap bitmap;
    /// Backend-owned texture payload. Geode editor builds present this directly via ImGui WGPU.
    std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot;
    /// Intrinsic texture dimensions in raster pixels. Metadata-only tiles
    /// keep this populated so the presentation cache can reject stale
    /// texture reuse across canvas-size epochs.
    Vector2i bitmapDimsPx = Vector2i::Zero();
    /// Raster canvas size that produced this payload. Metadata-only reuse is
    /// only valid inside the same raster-canvas epoch.
    Vector2i rasterCanvasSize = Vector2i::Zero();
    /// Canvas-space top-left of `bitmap`, in document units. Editor
    /// multiplies by `pixelsPerDocUnit` to get the on-screen blit
    /// origin.
    Vector2d canvasOffsetDoc = Vector2d::Zero();
    /// Bitmap's intrinsic dimensions, in document units. Editor
    /// multiplies by current `pixelsPerDocUnit` to get the on-screen
    /// blit size - keeps the bitmap stretching with pinch-zoom while
    /// the canvas-size commit is debounced.
    Vector2d bitmapDimsDoc = Vector2d::Zero();
    /// For drag-target tiles, the per-frame DOM translation in doc
    /// units (the delta between the bitmap's rasterize-time DOM
    /// transform and the entity's current DOM transform). Editor
    /// adds this to `canvasOffsetDoc` so the dragged tile slides in
    /// real time without re-rasterizing.
    Vector2d dragTranslationDoc = Vector2d::Zero();
    /// Affine transform from cached document placement to presented document placement.
    Transform2d documentFromCachedDocument = Transform2d();
    /// True when this tile is the active drag target. Useful for
    /// pre-test inspection; the editor's blit math treats drag and
    /// non-drag tiles uniformly via `dragTranslationDoc`.
    bool isDragTarget = false;
  };

  struct CompositedPreview {
    /// Paint-order tile list. Blit in `tiles` order: each tile gets
    /// one `AddImage` call at `(canvasOffsetDoc + dragTranslationDoc)
    /// * pixelsPerDocUnit` with size `bitmapDimsDoc *
    /// pixelsPerDocUnit`.
    std::vector<CompositedTile> tiles;
    /// Active drag-target entity (for selection chrome routing). May
    /// be `entt::null` if no entity is currently being dragged.
    Entity entity = entt::null;
    /// Interaction phase that produced this composited preview.
    svg::compositor::InteractionHint interactionKind = svg::compositor::InteractionHint::Selection;
    /// Drag preview state represented by the tile transforms in this result.
    std::optional<RenderRequest::DragPreview> representedDragPreview;

    [[nodiscard]] bool valid() const { return !tiles.empty(); }
  };

  svg::RendererBitmap bitmap;
  std::optional<CompositedPreview> compositedPreview;
  /// Raster viewport used to produce this result.
  EditorRasterViewport rasterViewport;
  /// Editor viewport \ref rasterViewport was derived from, copied from the request.
  ViewportState viewport;
  /// True when this result should update only retained overview infill.
  bool overviewInfillOnly = false;
  std::uint64_t version = 0;
  /// Document generation captured by the render request.
  std::uint64_t documentGeneration = 0;
  /// Wall-clock milliseconds spent in the worker iteration after a request is
  /// dequeued, including `CompositorController::renderFrame`, final
  /// snapshot/readback work, and diagnostic snapshots that gate presentation.
  /// Reported so the editor can plot worker latency alongside ImGui frame time
  /// on the frame graph. Zero means no worker timing was recorded.
  double workerMs = 0.0;
  WorkerTimingBreakdown workerTiming;
  /// Internal completion timestamp used to populate `workerTiming.pollDelayMs` on acceptance.
  std::chrono::steady_clock::time_point workerCompletedAt;
  /// Worker-owned browser-surface outcome for this result.
  DirectSurfacePresentationOutcome directSurfaceOutcome = DirectSurfacePresentationOutcome::None;
  /// Monotonic count of direct worker-surface frames presented this session.
  std::uint64_t directSurfaceFrames = 0;
  /// Entity whose worker-side compositor cache backs the direct surface.
  Entity directSurfaceEntity = entt::null;
  /// Drag preview represented by the pixels currently on the direct surface.
  std::optional<RenderRequest::DragPreview> directSurfaceDragPreview;
  /// Worker surface slot containing this frame. Direct WebGPU uses slot 0; the bitmap bridge
  /// retains slots 0 and 1 on the main thread.
  int directSurfaceSlot = 0;
  /// True when a bitmap-bridge back-buffer frame is staged for this result's surface token.
  bool bitmapBridgeFrameStaged = false;
  /// True when Select-mode path/bounds chrome is already part of the direct-surface pixels.
  bool directSurfaceSelectionChromeBaked = false;
};

/**
 * Build the compositor-tile metadata that accompanies a worker-owned browser surface.
 *
 * The surface itself owns the pixels, so every returned tile is metadata-only. This metadata is
 * still required for developer overlays, including low-resolution overview-infill frames.
 *
 * @param compositor Compositor that produced the presented browser surface.
 * @param rasterViewport Raster viewport used for the surface frame.
 * @param documentViewBox Document-space view box used to normalize tile offsets.
 * @param compositorEntity Explicitly promoted entity for this presentation.
 * @param dragPreview Drag state represented by the presented pixels, if any.
 * @param overviewInfillOnly True when the surface frame is low-resolution overview infill.
 * @return Paint-ordered metadata-only tiles, or `std::nullopt` when no valid tile exists.
 */
[[nodiscard]] std::optional<RenderResult::CompositedPreview>
BuildDirectSurfaceCompositorTileMetadata(
    svg::compositor::CompositorController& compositor, const EditorRasterViewport& rasterViewport,
    const Box2d& documentViewBox, Entity compositorEntity,
    const std::optional<RenderRequest::DragPreview>& dragPreview, bool overviewInfillOnly);

/// Terminal outcome for one low-priority sample-thumbnail render attempt.
enum class SampleThumbnailRenderOutcome : std::uint8_t {
  Rendered,
  Cancelled,
  ParseError,
  RenderError,
  RendererUnavailable,
};

/// One SVG source queued for bounded, low-priority rendering on the existing render worker.
struct SampleThumbnailRenderRequest {
  /// Caller-defined key copied into the result (the sample-catalog index in `EditorShell`).
  std::uint64_t key = 0;
  /// Complete SVG source. The request owns its copy until the worker finishes parsing it.
  std::string source;
  /// Output bitmap dimensions in device pixels.
  Vector2i dimensions = Vector2i::Zero();
  /// Root renderer used to create the worker-local offscreen on native builds.
  ///
  /// Browser builds ignore this pointer and reuse the renderer already owned by their worker.
  /// The native caller must keep it alive until the result is polled or the renderer is destroyed.
  svg::RendererInterface* nativeRenderer = nullptr;
};

/// CPU bitmap returned by one asynchronous sample-thumbnail attempt.
struct SampleThumbnailRenderResult {
  std::uint64_t key = 0;
  SampleThumbnailRenderOutcome outcome = SampleThumbnailRenderOutcome::RenderError;
  svg::RendererBitmap bitmap;
};

/// Observable state and monotonic counters for the bounded sample-thumbnail lane.
struct SampleThumbnailRenderStats {
  std::uint64_t requested = 0;
  std::uint64_t started = 0;
  std::uint64_t completed = 0;
  std::uint64_t rendered = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t offscreenRendererCreations = 0;
  bool pending = false;
  bool active = false;
  bool resultReady = false;
};

struct DirectSurfacePresentationState {
  bool active = false;
  EditorRasterViewport rasterViewport;
  /// Editor viewport the accepted pixels were rasterized against.
  ///
  /// Presentation places the surface with this transform rather than the live
  /// one, so an accepted epoch's geometry and its pixels always change
  /// together. A degenerate value (zero pane, non-positive zoom) means the
  /// epoch published no usable transform and the live viewport is used instead.
  ViewportState viewport;
  std::uint64_t frameCount = 0;
  int surfaceSlot = 0;
  bool selectionChromeBaked = false;
};

/// Whether \p viewport can place an accepted surface epoch on screen.
[[nodiscard]] bool DirectSurfacePlacementViewportIsUsable(const ViewportState& viewport);

/**
 * Return whether a worker-surface result may replace the currently loaded document surface.
 *
 * @param requestDocumentGeneration Generation captured by the completed render request.
 * @param minimumDocumentGeneration Oldest generation still eligible for presentation.
 */
[[nodiscard]] bool DirectSurfacePresentationGenerationIsCurrent(
    std::uint64_t requestDocumentGeneration, std::uint64_t minimumDocumentGeneration);

/// Whether construction starts the render worker or leaves it inert until an explicit start call.
enum class AsyncRendererStartMode : std::uint8_t {
  Immediate,
  Deferred,
};

class AsyncRenderer {
public:
  explicit AsyncRenderer(AsyncRendererStartMode startMode = AsyncRendererStartMode::Immediate);
  ~AsyncRenderer();

  AsyncRenderer(const AsyncRenderer&) = delete;
  AsyncRenderer& operator=(const AsyncRenderer&) = delete;
  AsyncRenderer(AsyncRenderer&&) = delete;
  AsyncRenderer& operator=(AsyncRenderer&&) = delete;

  /**
   * Start the render worker after its borrowed main-thread dependencies are ready.
   *
   * Native construction starts immediately for existing standalone users. Browser editor startup
   * calls this explicitly after synchronous main-thread WebGPU initialization, because Safari
   * cannot safely interleave another device's Promise completion with an Asyncify readback.
   * Repeated calls and calls after shutdown are no-ops.
   */
  void start();

  /// True when this instance currently owns a joinable/running render worker.
  [[nodiscard]] bool workerStartedForTesting() const {
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef DONNER_WASM_WORKER_SURFACE
    return threadStarted_;
#else
    return thread_.joinable();
#endif
  }

  /**
   * Stop accepting work, cancel both priority lanes, detach the wake callback, and join the worker.
   *
   * Safe to call more than once from the owning thread. The destructor calls this automatically;
   * owners with borrowed worker dependencies may call it earlier to control teardown order.
   */
  void shutdown();

  /// Returns true while a render/result or document-reading cache warmup owns the input gate.
  /// The UI thread must not touch the `Renderer` or mutate the `SVGDocument` while this returns
  /// true.
  [[nodiscard]] bool isBusy() const;

  /// Returns true only while the worker may still be computing or cancelling a render.
  /// Unlike `isBusy()`, a staged result waiting in `DoneState` is not in flight.
  [[nodiscard]] bool hasRenderInFlightForTesting() const;

  /**
   * Wait until no worker render is actively in flight.
   *
   * @param deadline Steady-clock deadline for the bounded wait.
   */
  [[nodiscard]] bool waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::time_point deadline);

  /// Whether the compositor's document reference is bound to the renderer-owned retained value.
  /// A compositor bound to a request-local RenderLease becomes dangling before deferred warmup.
  [[nodiscard]] bool compositorUsesRetainedDocumentForTesting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return compositor_ != nullptr && compositorDocument_.has_value() &&
           &compositor_->document() == &*compositorDocument_;
  }

  /**
   * Inject a fixed delay into each worker render attempt for replay tests.
   *
   * @param delay Delay duration. Negative durations are clamped to zero.
   */
  void setReplayRenderDelayForTesting(std::chrono::milliseconds delay);

  /**
   * Hold each staged result for a fixed number of poll attempts in replay tests.
   *
   * @param frameCount Number of poll attempts to withhold a newly staged result.
   */
  void setReplayResultHoldFramesForTesting(int frameCount);

  /**
   * Stage a synthetic direct-surface result for state-machine tests.
   *
   * @param result Result to install in `DoneState` until it is polled or cancelled.
   */
  void stageDirectSurfaceResultForTesting(RenderResult result);

  /** Stage a synthetic direct-surface result before its worker task-boundary acknowledgment. */
  void stageDirectSurfaceResultPendingTaskBoundaryForTesting(RenderResult result);

  /// Install a synthetic low-priority warmup state for document-access gate tests.
  void stageCompositorWarmupForTesting(bool pending, bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::holds_alternative<ShutdownState>(workerState_)) {
      return;
    }
    pendingCompositorWarmup_ = pending;
    compositorWarmupActive_ = active;
  }

  /// Simulate the active warmup releasing its document guard.
  void completeCompositorWarmupForTesting() {
    std::function<void()> wake;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pendingCompositorWarmup_ = false;
      compositorWarmupActive_ = false;
      if (std::exchange(compositorWarmupReleaseWakePending_, false)) {
        wake = wakeCallback_;
      }
    }
    cv_.notify_all();
    if (wake) {
      wake();
    }
  }

  /** Acknowledge a synthetic direct-surface task boundary and make its result pollable. */
  [[nodiscard]] bool acknowledgeDirectSurfaceTaskBoundaryForTesting();

  /**
   * Acknowledge a synthetic boundary only when it still belongs to @p frameToken.
   *
   * Models a delayed browser callback after a newer direct-surface frame has replaced the
   * pending state.
   */
  [[nodiscard]] bool acknowledgeDirectSurfaceTaskBoundaryForTesting(std::uint64_t frameToken);

  /// Number of poll attempts that intentionally withheld a staged result for replay tests.
  [[nodiscard]] std::uint64_t replayResultHoldPollCountForTesting() const {
    return replayResultHoldPollCount_.load(std::memory_order_acquire);
  }

  /// Post a render request to the worker. Non-blocking. If the worker is busy,
  /// this cancels the in-flight render at the next compositor safe point and
  /// replaces the pending request slot with the latest request.
  void requestRender(const RenderRequest& request);

  /// Cancel an in-flight render without posting a replacement. Use when the
  /// current render is dispensable and the UI needs the worker to become idle
  /// before dispatching registry-touching input.
  /// Safe to call from any thread.
  void cancelInFlight();

  /// Design doc 0033 §M4 - count of renders that were cancelled
  /// mid-flight by a subsequent `requestRender`. Exposed for tests
  /// to assert preemption is engaging (vs. the worker silently
  /// queueing requests). Incremented under the internal mutex; safe
  /// to read from any thread.
  [[nodiscard]] std::uint64_t cancelledRenderCount() const {
    return cancelledRenderCount_.load(std::memory_order_acquire);
  }

  /// If a render has completed since the last call, returns the
  /// resulting bitmap and transitions the worker back to idle. Returns
  /// `std::nullopt` if no render is pending-ready (either still busy
  /// or idle with nothing to hand off).
  std::optional<RenderResult> pollResult();

  /**
   * Queue one low-priority SVG thumbnail on this renderer's existing worker.
   *
   * The lane has exactly one slot spanning pending, active, and completed-but-unpolled work. Main
   * document renders always take priority and cancel an active thumbnail at the next safe point.
   *
   * @return True when the request was accepted, false when the bounded slot is occupied or the
   * renderer is shutting down.
   */
  [[nodiscard]] bool requestSampleThumbnail(SampleThumbnailRenderRequest request);

  /// Poll one completed sample-thumbnail result without changing main-document busy state.
  [[nodiscard]] std::optional<SampleThumbnailRenderResult> pollSampleThumbnailResult();

  /// Drop queued/unpolled sample-thumbnail work and cancel an active attempt.
  void cancelSampleThumbnailWork();

  /// Snapshot low-priority worker counters and slot state.
  [[nodiscard]] SampleThumbnailRenderStats sampleThumbnailRenderStats() const;

  /// Inject a cancellation-aware delay before thumbnail parsing for deterministic priority tests.
  void setSampleThumbnailRenderDelayForTesting(std::chrono::milliseconds delay);

  /// Install a callback that the worker thread invokes when a render
  /// result or cancellation completes. Used by the editor's on-demand
  /// render loop to wake the UI thread (e.g. via `glfwPostEmptyEvent`)
  /// so fresh results or newly-idle deferred input get picked up
  /// without continuous polling.
  ///
  /// The callback runs on the worker thread. It must be thread-safe
  /// and must NOT re-enter the renderer - a simple wake-up post into
  /// the window's event queue is the intended use.
  void setWakeCallback(std::function<void()> callback);

  /// Toggle whether the compositor uses tight-bounded segment
  /// rasterization (design doc 0027). The change applies at the start
  /// of the next worker iteration - `renderFrame` calls
  /// `CompositorController::setTightBoundedSegmentsEnabled` before
  /// compositing, which marks all cached segments dirty so the flip
  /// takes full effect that frame.
  ///
  /// Safe to call from the UI thread while a render is in flight; the
  /// flag is stored in an `std::atomic<bool>`, and the worker reads it
  /// at a well-defined point in each iteration.
  void setTightBoundedSegmentsEnabled(bool enabled) {
    tightBoundedSegments_.store(enabled, std::memory_order_release);
  }

  /// Mirror of the current toggle state. UI reads this to render the
  /// correct check state in the View menu without racing the worker.
  [[nodiscard]] bool tightBoundedSegmentsEnabled() const {
    return tightBoundedSegments_.load(std::memory_order_acquire);
  }

  /// Toggle the Geode geometry debug overlay
  /// (`RendererInterface::setDebugGeometryOverlay`) on the root document
  /// renderer. The change applies at the start of the next worker iteration.
  /// Each state transition clears retained compositor state once. While
  /// enabled, selection promotion/prewarm remains suppressed and every render
  /// presents one flat full-document root frame so retained tiles cannot crop
  /// or cover the frame-final wireframe. Disabling performs one transition
  /// reset, then normal retained promotion resumes.
  ///
  /// Same threading contract as \ref setTightBoundedSegmentsEnabled:
  /// safe to call from the UI thread while a render is in flight.
  void setGeometryDebugOverlayEnabled(bool enabled) {
    geometryDebugOverlay_.store(enabled, std::memory_order_release);
  }

  /// Mirror of the current overlay state. UI reads this to render the
  /// correct check state in the View menu without racing the worker.
  [[nodiscard]] bool geometryDebugOverlayEnabled() const {
    return geometryDebugOverlay_.load(std::memory_order_acquire);
  }

  /// Number of times the worker has called `CompositorController::resetAllLayers()`
  /// since construction. Tests use this to assert that frame-version mutations
  /// do not masquerade as document replacements.
  ///
  /// Counts resets driven by a `request.documentGeneration` mismatch and
  /// geometry-debug state transitions; not the implicit reset performed on
  /// first compositor construction.
  ///
  /// Safe to read from the UI thread; incremented under the internal mutex on
  /// the worker.
  [[nodiscard]] std::uint64_t compositorResetCountForTesting() const {
    return compositorResetCount_.load(std::memory_order_acquire);
  }

  /// Number of times the worker has reconstructed `compositor_` from scratch.
  /// First construction counts as one. Tests use this to verify that structural
  /// remaps preserve cached layer state across drag-writeback reparses.
  ///
  /// Safe to read from the UI thread; incremented under the internal mutex
  /// on the worker.
  [[nodiscard]] std::uint64_t compositorReconstructCountForTesting() const {
    return compositorReconstructCount_.load(std::memory_order_acquire);
  }

  /// Snapshot of the compositor's fast-path counters. Read-only - the worker
  /// writes them under the mutex when transitioning to Done. Returns zeros
  /// before the compositor is constructed (first render not yet requested).
  /// UI-thread safe.
  [[nodiscard]] svg::compositor::CompositorController::FastPathCounters
  compositorFastPathCountersForTesting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastFastPathCounters_;
  }

  /// Snapshot of the worker compositor's immediate-vs-cached raster costs from the latest
  /// completed render.
  [[nodiscard]] svg::compositor::CompositorController::RenderFrameStats compositorRenderFrameStats()
      const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastCompositorRenderFrameStats_;
  }

  /// Snapshot of the compositor's per-layer diagnostic rows (design doc
  /// 0033 M1). Captured under the worker mutex at every Done transition;
  /// the UI thread copies the cached vector out under the lock. Empty
  /// before the first render lands or when the compositor isn't
  /// instantiated.
  [[nodiscard]] std::vector<svg::compositor::CompositorController::LayerInspectorRow>
  compositorLayerInspectorRows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastLayerInspectorRows_;
  }

  /// Snapshot of the compositor's per-segment diagnostic rows. Same
  /// capture point and locking as the per-layer rows.
  [[nodiscard]] std::vector<svg::compositor::CompositorController::SegmentInspectorRow>
  compositorSegmentInspectorRows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastSegmentInspectorRows_;
  }

  /// Unified in-paint-order snapshot of every tile the compositor
  /// blits to produce the final composite (design doc 0033 §M1++).
  /// The editor's layer-inspector panel renders this list with
  /// thumbnails for every tile so the operator can see the
  /// comprehensive composite at a glance.
  [[nodiscard]] std::vector<svg::compositor::CompositorController::CompositeTileSnapshot>
  compositorCompositeTiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastCompositeTiles_;
  }

  /// Compositor-wide diagnostic state: active-hints count, layer
  /// count, split-path active flag, drag-target entity, canvas
  /// size. The editor's layer-inspector panel renders this as a
  /// state header so the operator can spot mismatches between the
  /// editor's idea of the drag target and the compositor's.
  [[nodiscard]] svg::compositor::CompositorController::StateSnapshot compositorState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastStateSnapshot_;
  }

  /// The worker's current view of which entity is promoted. Read on
  /// the UI thread; the worker updates it under the same mutex as the
  /// other snapshot fields when transitioning to Done. `entt::null`
  /// when the worker hasn't promoted anything (e.g. promotion was
  /// refused). Compare against the editor's selection to spot races
  /// between the editor's `selectedEntity` and what the compositor
  /// actually holds.
  [[nodiscard]] Entity workerCompositorEntity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastWorkerCompositorEntity_;
  }

  /// Output raster size from the worker's last-completed render. This is the
  /// presentation epoch: at high zoom it can be smaller than the SVG
  /// document's semantic canvas size because the worker rendered only the
  /// visible viewport plus margin.
  [[nodiscard]] Vector2i lastDocumentCanvasSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastDocumentCanvasSize_;
  }

  /// Latest worker-owned browser-surface presentation geometry.
  [[nodiscard]] DirectSurfacePresentationState directSurfacePresentation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastDirectSurfacePresentation_;
  }

  /**
   * Invalidate the browser surface during a full document replacement.
   *
   * Results from older document generations remain ineligible even when their surface handoff
   * finishes after this call.
   *
   * @param documentGeneration Generation of the newly loaded document.
   */
  void invalidateDirectSurfacePresentation(std::uint64_t documentGeneration);

  /// Replace the cached direct-surface state for coordinator reset tests.
  void setDirectSurfacePresentationForTesting(DirectSurfacePresentationState presentation) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastDirectSurfacePresentation_ = std::move(presentation);
  }

  /// Enable expensive compositor inspector snapshots for the developer panel.
  /// Scalar render timing and fast-path counters remain available when disabled.
  void setCompositorDiagnosticsEnabled(bool enabled) {
    compositorDiagnosticsEnabled_.store(enabled, std::memory_order_release);
  }

private:
  void workerLoop();

#ifdef DONNER_WASM_WORKER_SURFACE
  static void* workerThreadEntry(void* self);
  static void completeWasmWorkerDeviceInitialization(std::unique_ptr<geode::GeodeDevice> device,
                                                     void* userdata);
  static void runWorkerTask(void* self);
  static void acknowledgeDirectSurfaceTaskBoundary(void* self);
  [[noreturn]] void exitWasmWorker();
  void beginWasmWorkerRuntimeInitialization();
  WasmWorkerRuntimeFinishAction finishWasmWorkerRuntimeInitialization(
      std::unique_ptr<geode::GeodeDevice> device, bool usingBgra8PrimaryFormat,
      std::chrono::steady_clock::time_point initializationStart, int workerDeviceCreations);
  WorkerTaskScheduleResult scheduleWorkerTask();
  void handleWasmWorkerRuntimeUnavailable();
  void handleWorkerTaskEnqueueFailure();
  pthread_t thread_{};
  bool threadStarted_ = false;
  em_proxying_queue* proxyQueue_ = nullptr;
  bool useBitmapWorkerSurfaceBridge_ = false;
  bool publishWorkerSurfaceDiagnostic_ = false;
  bool workerSurfaceDiagnosticAttempted_ = false;
  bool workerSurfaceDiagnosticPublished_ = false;
  int lastPublishedDirectSurfaceSlot_ = 1;
  std::unique_ptr<WasmWorkerRuntime> wasmWorkerRuntime_;
  std::shared_ptr<WasmWorkerRuntimeInitControl> wasmWorkerRuntimeInitControl_;
  WasmWorkerRuntimeInitializationStatus wasmWorkerRuntimeInitializationStatus_ =
      WasmWorkerRuntimeInitializationStatus::Initializing;
  bool wasmWorkerRuntimeFailureReported_ = false;
  int wasmWorkerRuntimeInitializationCount_ = 0;
  std::atomic_uint64_t workerTaskWakeFailureCount_{0};
  WorkerTaskWakeGate workerTaskWakeGate_;
#else
  std::thread thread_;
#endif
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  struct IdleState {};
  struct RenderingState {
    /// Latest request waiting for the worker. Empty while the worker is
    /// actively rendering the request it already dequeued.
    std::optional<RenderRequest> pendingRequest;
  };
  struct CancellingState {};
  struct DoneState {
    /// Render result. Draining this transitions the worker state to idle.
    RenderResult result;
    /// UI poll attempts remaining before this staged result becomes visible.
    ///
    /// Replay tests use this to model delayed delivery. Direct Wasm surfaces also use the same
    /// existing storage for one browser-compositor warmup frame, so accepting a result cannot
    /// expose a surface whose WebGPU presentation has not reached the screen yet.
    int presentationHoldPollsRemaining = 0;
    /// True only after a later worker event turn acknowledges direct-surface presentation.
    bool directSurfaceTaskBoundaryAcknowledged = false;
  };
  struct PendingDirectSurfaceTaskBoundaryState {
    // Move the existing DoneState behind the fence; never allocate a second RenderResult buffer.
    DoneState done;
  };
  static_assert(sizeof(PendingDirectSurfaceTaskBoundaryState) == sizeof(DoneState));
  struct ShutdownState {};
  using WorkerState = std::variant<IdleState, RenderingState, CancellingState, DoneState,
                                   PendingDirectSurfaceTaskBoundaryState, ShutdownState>;

  [[nodiscard]] static bool workerStateBusy(const WorkerState& state);
  [[nodiscard]] static bool workerStateRenderInFlight(const WorkerState& state);
  bool acknowledgeDirectSurfaceTaskBoundaryLocked(std::uint64_t frameToken,
                                                  std::function<void()>& wake);

  WorkerState workerState_;
  /// Single low-priority slot. It remains independent from `WorkerState` so thumbnail work never
  /// makes `isBusy()` gate editor input or document mutation.
  std::optional<SampleThumbnailRenderRequest> pendingSampleThumbnail_;
  std::optional<SampleThumbnailRenderResult> sampleThumbnailResult_;
  bool sampleThumbnailActive_ = false;
  bool discardActiveSampleThumbnailResult_ = false;
  SampleThumbnailRenderStats sampleThumbnailCounters_;
  svg::compositor::CancellationToken cancelSampleThumbnail_;
  std::atomic<std::chrono::milliseconds::rep> sampleThumbnailRenderDelayMsForTesting_{0};
  /// Offscreen-only cache preparation left over after publishing a correct first document frame.
  /// It shares the worker but not `WorkerState`, so input can post a foreground render immediately;
  /// that request cancels this work at the compositor's existing safe points.
  bool pendingCompositorWarmup_ = false;
  bool compositorWarmupActive_ = false;
  /// A caller deferred document access while warmup owned the write guard. Keep this independent
  /// from the cancellation token so a cancel arriving after warmup's final token poll still wakes
  /// the caller at the actual guard-release edge.
  bool compositorWarmupReleaseWakePending_ = false;
  svg::compositor::CancellationToken cancelCompositorWarmup_;
  /// Structural remaps retained by document generation until the worker has
  /// actually advanced the compositor to that generation. A request can be
  /// canceled before the worker consumes its remap; without this cache, the
  /// next request for the same replacement document would see an empty remap
  /// and fall back to `resetAllLayers(documentReplaced=true)`.
  std::unordered_map<std::uint64_t, std::unordered_map<Entity, Entity>> retainedStructuralRemaps_;
  /// Optional UI-thread wake-up hook, invoked by the worker when a
  /// render finishes. Set once at editor startup; owned by the
  /// installer. Held under `mutex_` so mutation vs. invocation races
  /// are impossible.
  std::function<void()> wakeCallback_;
  /// The `SVGDocument` this compositor is currently configured for. Stored by
  /// value - `SVGDocument` is a thin value-facade over a `std::shared_ptr<Registry>`
  /// (see `SVGDocumentHandle`), so copying is a refcount bump, not a deep copy
  /// of the document state. `nullopt` before the first render request; set to
  /// a copy of the request's document lease on the first iteration and any time the
  /// underlying document handle changes.
  ///
  /// Identity comparison uses `handle().get()` against the incoming request's
  /// document - two `SVGDocument` values wrapping the same `std::shared_ptr<
  /// Registry>` compare equal, which is the right "same document" semantic.
  std::optional<svg::SVGDocument> compositorDocument_;
  /// Declared after the retained document so reverse member destruction tears the controller down
  /// first while its referenced SVGDocument value is still alive.
  std::unique_ptr<svg::compositor::CompositorController> compositor_;
  svg::Renderer* compositorRenderer_ = nullptr;
  Entity compositorEntity_ = entt::null;
  /// Full set of explicit editor-promoted entities currently tracked by the worker compositor.
  std::vector<Entity> compositorEntities_;
  /// Kind under which `compositorEntity_` is currently promoted. Tracked
  /// alongside the entity so a Selection→ActiveDrag transition refreshes
  /// the hint in place instead of demote-then-re-promote (which would
  /// drop the layer's cached bitmap and tank fast-path engagement on the
  /// first drag move after a click).
  svg::compositor::InteractionHint compositorInteractionKind_ =
      svg::compositor::InteractionHint::Selection;
  /// Document generation at the time the compositor was last configured.
  /// Generation, not frame version, is the document-replacement signal that
  /// invalidates entity handles.
  std::uint64_t compositorDocumentGeneration_ = 0;

  struct PublishedCompositedTile {
    RenderResult::CompositedTile::Kind kind = RenderResult::CompositedTile::Kind::Segment;
    std::uint64_t generation = 0;
    Vector2i bitmapDims = Vector2i::Zero();
    Vector2i rasterCanvasSize = Vector2i::Zero();
  };

  /// Split-tile textures the UI has received from previous composited
  /// previews. The worker uses this to send metadata-only entries for
  /// unchanged tiles instead of copying and shipping full bitmaps on every
  /// selection / drag frame.
  std::unordered_map<std::string, PublishedCompositedTile> publishedCompositedTiles_;

  /// Update `publishedCompositedTiles_` after a result is actually handed
  /// to the UI thread.
  void notePublishedCompositedPreview(
      const std::optional<RenderResult::CompositedPreview>& compositedPreview);

  /// Commit an accepted direct-surface result while holding `mutex_`.
  void commitDirectSurfacePresentation(RenderResult& result);

  /// Counter of worker-side `resetAllLayers()` invocations. Document-generation
  /// changes and supported geometry-overlay state transitions increment it;
  /// ordinary frame-version changes do not.
  std::atomic<std::uint64_t> compositorResetCount_{0};

  /// Counter of worker-side `compositor_ = make_unique<...>(...)` reconstructs.
  /// Bumps once per session at first construction. Structural remaps should
  /// keep it there across drag-release-and-reparse cycles.
  std::atomic<std::uint64_t> compositorReconstructCount_{0};

  /// Design doc 0033 §M4 - cancellation token threaded into
  /// `CompositorController::renderFrame(viewport, token)`. The UI
  /// thread sets `cancelRender_` via `requestRender` when posting a
  /// new request while the worker is busy; the worker polls the
  /// token at coarse safe points and bails. Reset to non-cancelled
  /// at the start of every worker iteration so a fresh request
  /// doesn't inherit a stale cancel signal.
  svg::compositor::CancellationToken cancelRender_;

  /// Count of worker iterations that returned early due to
  /// cancellation. Exposed via `cancelledRenderCount()` so tests can
  /// pin the preemption behavior.
  std::atomic<std::uint64_t> cancelledRenderCount_{0};

  /// Most recent snapshot of the compositor's fast-path counters, copied
  /// under `mutex_` when the worker finishes each render. UI-thread reads
  /// this via `compositorFastPathCountersForTesting`. Mutable because we
  /// lock in a const method.
  svg::compositor::CompositorController::FastPathCounters lastFastPathCounters_;

  /// Most recent compositor render-cost split captured at the Done transition.
  svg::compositor::CompositorController::RenderFrameStats lastCompositorRenderFrameStats_;

  /// Most recent per-layer diagnostic snapshot, captured under `mutex_`
  /// at every Done transition. UI-thread reads this via
  /// `compositorLayerInspectorRows()`. Empty before the first render.
  std::vector<svg::compositor::CompositorController::LayerInspectorRow> lastLayerInspectorRows_;

  /// Most recent per-segment diagnostic snapshot. Captured / read on
  /// the same code path as `lastLayerInspectorRows_`.
  std::vector<svg::compositor::CompositorController::SegmentInspectorRow> lastSegmentInspectorRows_;

  /// Most recent unified composite-tile snapshot (in paint order).
  std::vector<svg::compositor::CompositorController::CompositeTileSnapshot> lastCompositeTiles_;

  /// Most recent compositor state snapshot (active-hints count,
  /// split-path active flag, etc.). Captured under `mutex_` at the
  /// Done-transition site alongside the other snapshots.
  svg::compositor::CompositorController::StateSnapshot lastStateSnapshot_;

  /// Snapshot of the worker's `compositorEntity_` at the last Done
  /// transition. Surfaces the worker's view of "which entity is
  /// currently promoted" to the editor's diagnostic panel.
  Entity lastWorkerCompositorEntity_ = entt::null;

  /// Canvas size from the request's document lease at the last
  /// completed render. Diagnostic - compared against the
  /// compositor's `staticSegmentsCanvas_` to spot "document was
  /// re-sized but compositor hasn't caught up yet".
  Vector2i lastDocumentCanvasSize_ = Vector2i::Zero();

  DirectSurfacePresentationState lastDirectSurfacePresentation_;
  /// Oldest document generation whose worker-surface handoff may become visible.
  std::uint64_t minimumDirectSurfaceDocumentGeneration_ = 0;
  std::uint64_t directSurfaceFrameCount_ = 0;

  /// Runtime kill-switch for tight-bounded segment rasterization. Pushed
  /// into `CompositorController` at the start of each worker iteration.
  /// Default-true matches `CompositorConfig::tightBoundedSegments`. See
  /// `setTightBoundedSegmentsEnabled`.
  std::atomic<bool> tightBoundedSegments_{true};

  /// Geode geometry debug overlay request from the UI thread. See
  /// `setGeometryDebugOverlayEnabled`. Default-off matches the
  /// renderer's default.
  std::atomic<bool> geometryDebugOverlay_{false};

  /// Whether completed renders should materialize developer inspector snapshots.
  std::atomic<bool> compositorDiagnosticsEnabled_{true};

  /// Worker-thread-only: the overlay state last applied to the root request
  /// renderer. Transitions trigger one retained-state reset; the enabled state
  /// additionally keeps every debug frame in flat full-document mode.
  bool appliedGeometryDebugOverlay_ = false;

  /// Replay/test-only fixed delay injected into each worker render attempt.
  std::atomic<std::chrono::milliseconds::rep> replayRenderDelayMsForTesting_{0};

  /// Replay/test-only number of poll attempts to hold each newly staged result.
  int replayResultHoldFramesForTesting_ = 0;

  /// Replay/test-only count of poll attempts that withheld a staged result.
  std::atomic<std::uint64_t> replayResultHoldPollCount_{0};
};

}  // namespace donner::editor
