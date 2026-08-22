#include "donner/editor/AsyncRenderer.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#endif

#include "donner/base/MemoryAttribution.h"
#include "donner/base/Utils.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

namespace {

// ---------------------------------------------------------------------------
// WEBKIT BITMAP BRIDGE - RETAINED PENDING THE DEFERRED WEBKIT DECISION
//
// `DONNER_WASM_WORKER_SURFACE` is defined by no build configuration, so nothing
// below is compiled anywhere. It is the complete C++ dependency list of the
// WebKit bitmap bridge - the two alternating document-canvas selectors, the
// worker-canvas selector, the mode probe, and the three ImageBitmap handoff
// entry points - held out of the single-canvas architecture deletion series so
// the retire-or-rebuild decision for WebKit is made against the real code
// rather than a changelog.
//
// This block is NOT a supported configuration and must not be revived as-is:
// it presents into a second DOM canvas, which the "no CSS in presentation"
// invariant forbids. That decision either deletes it (see the series' optional
// bridge-deletion patch, which does exactly that and nothing else) or rebuilds
// an ImageBitmap handoff against the single canvas. Retained-but-unused code is
// not an outcome.
// ---------------------------------------------------------------------------
#ifdef DONNER_WASM_WORKER_SURFACE
constexpr const char* kDirectWorkerDocumentCanvasSelector = "#donner-document-canvas";
constexpr const char* kDirectWorkerDocumentBackCanvasSelector = "#donner-document-canvas-back";
constexpr const char* kDirectWorkerDocumentCanvasSelectors =
    "#donner-document-canvas,#donner-document-canvas-back";
constexpr const char* kBitmapWorkerDocumentCanvasSelector = "#donner-worker-document-canvas";

EM_JS(int, UseBitmapWorkerSurfaceBridge, (),
      { return globalThis['__donnerWorkerSurfaceMode'] == 'bitmap-bridge' ? 1 : 0; });

EM_JS(int, StageWorkerDocumentBitmap,
      (const char* selector, int width, int height, double frameToken, int surfaceSlot), {
        try {
          const canvasTarget = findCanvasEventTarget(UTF8ToString(selector));
          const canvas = canvasTarget && (canvasTarget['offscreenCanvas'] || canvasTarget);
          if (!canvas || typeof canvas.transferToImageBitmap != 'function') {
            Module['printErr'](
                'Donner bitmap bridge: transferred canvas cannot create an ImageBitmap');
            return 0;
          }
          const bitmap = canvas.transferToImageBitmap();
          postMessage({
            'cmd' : 'callHandler',
            'handler' : 'stageDonnerDocumentBitmap',
            'args' : [ frameToken, surfaceSlot, bitmap, width, height ],
          },
                      [bitmap]);
          return 1;
        } catch (error) {
          Module['printErr']('Donner bitmap bridge failed: ' + error);
          return 0;
        }
      });

EM_JS(void, CommitWorkerDocumentBitmap, (double frameToken, int surfaceSlot), {
  if (typeof Module['commitDonnerDocumentBitmap'] == 'function') {
    Module['commitDonnerDocumentBitmap'](frameToken, surfaceSlot);
  }
});

EM_JS(void, DiscardWorkerDocumentBitmap, (double frameToken), {
  if (typeof document != 'undefined') {
    if (typeof Module['discardDonnerDocumentBitmap'] == 'function') {
      Module['discardDonnerDocumentBitmap'](frameToken);
    }
    return;
  }
  postMessage({
    'cmd' : 'callHandler',
    'handler' : 'discardDonnerDocumentBitmap',
    'args' : [frameToken],
  });
});
#endif  // DONNER_WASM_WORKER_SURFACE

RenderResult::CompositedPreview BuildFullCanvasCompositedPreview(
    const Box2d& documentViewBox, const svg::RendererBitmap& bitmap,
    std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot, std::uint64_t generation,
    Entity entity, svg::compositor::InteractionHint interactionKind,
    const EditorRasterViewport& rasterViewport,
    std::optional<RenderRequest::DragPreview> representedDragPreview) {
  RenderResult::CompositedTile tile;
  tile.kind = RenderResult::CompositedTile::Kind::Segment;
  tile.id = "full-canvas";
  tile.generation = generation;
  tile.bitmap = bitmap;
  tile.textureSnapshot = std::move(textureSnapshot);
  tile.canvasOffsetDoc = rasterViewport.documentRect.topLeft - documentViewBox.topLeft;
  const Vector2i payloadDims =
      !bitmap.empty() ? bitmap.dimensions
                      : (tile.textureSnapshot != nullptr ? tile.textureSnapshot->dimensions()
                                                         : Vector2i::Zero());
  tile.bitmapDimsPx = payloadDims;
  tile.rasterCanvasSize = rasterViewport.outputSizePx;
  if (rasterViewport.documentRect.width() > 0.0 && rasterViewport.documentRect.height() > 0.0) {
    tile.bitmapDimsDoc = rasterViewport.documentRect.size();
  } else {
    tile.bitmapDimsDoc =
        Vector2d(static_cast<double>(payloadDims.x), static_cast<double>(payloadDims.y));
  }

  return RenderResult::CompositedPreview{
      .tiles = {std::move(tile)},
      .entity = entity,
      .interactionKind = interactionKind,
      .representedDragPreview = std::move(representedDragPreview),
  };
}

}  // namespace

namespace {

EditorRasterViewport EffectiveRasterViewportForRequest(svg::SVGDocument& document,
                                                       const EditorRasterViewport& requested) {
  if (requested.outputSizePx.x > 0 && requested.outputSizePx.y > 0 &&
      requested.semanticCanvasSizePx.x > 0 && requested.semanticCanvasSizePx.y > 0) {
    return requested;
  }

  EditorRasterViewport fallback;
  fallback.outputSizePx = document.canvasSize();
  fallback.semanticCanvasSizePx = fallback.outputSizePx;
  if (const std::optional<Box2d> viewBox = document.svgElement().viewBox()) {
    fallback.documentRect = *viewBox;
  } else {
    fallback.documentRect = Box2d::FromXYWH(0.0, 0.0, static_cast<double>(fallback.outputSizePx.x),
                                            static_cast<double>(fallback.outputSizePx.y));
  }
  fallback.outputFromDocument = document.canvasFromDocumentTransform();
  return fallback;
}

bool ContainsEntity(const std::vector<Entity>& entities, Entity entity) {
  return std::ranges::find(entities, entity) != entities.end();
}

void AppendUniqueEntity(std::vector<Entity>* entities, Entity entity) {
  if (entity != entt::null && !ContainsEntity(*entities, entity)) {
    entities->push_back(entity);
  }
}

std::vector<Entity> DragPreviewEntities(const RenderRequest::DragPreview& preview) {
  std::vector<Entity> entities;
  entities.reserve(1u + preview.extraEntities.size());
  AppendUniqueEntity(&entities, preview.entity);
  for (Entity entity : preview.extraEntities) {
    AppendUniqueEntity(&entities, entity);
  }
  return entities;
}

std::vector<Entity> DesiredCompositorEntities(const RenderRequest& request) {
  if (request.dragPreview.has_value()) {
    return DragPreviewEntities(*request.dragPreview);
  }

  std::vector<Entity> entities;
  AppendUniqueEntity(&entities, request.selectedEntity);
  return entities;
}

bool SameEntityList(const std::vector<Entity>& lhs, const std::vector<Entity>& rhs) {
  return lhs == rhs;
}

bool WaitForSampleThumbnailDelay(const svg::compositor::CancellationToken& cancellation,
                                 std::chrono::milliseconds delay) {
  const auto deadline = std::chrono::steady_clock::now() + delay;
  while (!cancellation.isCancelled()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return true;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(1)));
  }
  return false;
}

SampleThumbnailRenderResult RenderSampleThumbnail(
    SampleThumbnailRenderRequest request, svg::RendererInterface& renderer,
    const svg::compositor::CancellationToken& cancellation, std::chrono::milliseconds delay) {
  SampleThumbnailRenderResult result{
      .kind = request.kind,
      .key = request.key,
      .outcome = SampleThumbnailRenderOutcome::RenderError,
  };
  if (request.dimensions.x <= 0 || request.dimensions.y <= 0) {
    return result;
  }
  if (delay.count() > 0 && !WaitForSampleThumbnailDelay(cancellation, delay)) {
    result.outcome = SampleThumbnailRenderOutcome::Cancelled;
    return result;
  }
  if (cancellation.isCancelled()) {
    result.outcome = SampleThumbnailRenderOutcome::Cancelled;
    return result;
  }

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(request.source, warnings);
  if (parsed.hasError()) {
    result.outcome = SampleThumbnailRenderOutcome::ParseError;
    return result;
  }
  if (cancellation.isCancelled()) {
    result.outcome = SampleThumbnailRenderOutcome::Cancelled;
    return result;
  }

  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(request.dimensions.x, request.dimensions.y);

  svg::RenderViewport viewport;
  viewport.size = Vector2d(request.dimensions.x, request.dimensions.y);
  viewport.devicePixelRatio = 1.0;
  svg::RendererDriver driver(renderer);
  const bool completed = driver.drawInterruptibly(
      document, viewport, Transform2d(), [&cancellation] { return cancellation.isCancelled(); });
  if (!completed || cancellation.isCancelled()) {
    result.outcome = SampleThumbnailRenderOutcome::Cancelled;
    return result;
  }

  result.bitmap =
      renderer.takeSnapshotInterruptibly([&cancellation] { return cancellation.isCancelled(); });
  if (cancellation.isCancelled()) {
    result.bitmap = {};
    result.outcome = SampleThumbnailRenderOutcome::Cancelled;
    return result;
  }
  if (!result.bitmap.empty()) {
    result.outcome = SampleThumbnailRenderOutcome::Rendered;
  }
  return result;
}

}  // namespace

PresentationSnapshotPlan ChoosePresentationSnapshotPlan(bool hasCompositedPreview,
                                                        bool requiresTextureSnapshotPresentation,
                                                        bool captureCpuSnapshot) {
  if (requiresTextureSnapshotPresentation) {
    return PresentationSnapshotPlan{
        .captureCpuSnapshot = captureCpuSnapshot,
        .captureTextureSnapshot = !hasCompositedPreview,
    };
  }

  // Without a composited preview the CPU bitmap *is* the presented frame, so it is always read
  // back. When a preview covers the frame the readback is pure overhead for presentation, but
  // `captureCpuSnapshot` callers (replay harnesses, goldens, thumbnail and diagnostic captures)
  // consume `RenderResult::bitmap` directly and must still receive one.
  return PresentationSnapshotPlan{
      .captureCpuSnapshot = captureCpuSnapshot || !hasCompositedPreview,
  };
}

AsyncRenderer::AsyncRenderer(AsyncRendererStartMode startMode) {
  if (startMode == AsyncRendererStartMode::Immediate) {
    start();
  }
}

void AsyncRenderer::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::holds_alternative<ShutdownState>(workerState_)) {
    return;
  }
  if (thread_.joinable()) {
    return;
  }
  thread_ = std::thread([this] { workerLoop(); });
}

AsyncRenderer::~AsyncRenderer() {
  shutdown();
}

void AsyncRenderer::shutdown() {
  bool initiatedShutdown = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Detach first so no later worker completion can copy a callback into its local wake slot. A
    // callback copied before this lock is still safe: the join below waits for it to return while
    // the owner and its window remain alive.
    wakeCallback_ = {};
    if (std::holds_alternative<ShutdownState>(workerState_)) {
      return;
    }
    pendingSampleThumbnail_.reset();
    sampleThumbnailResult_.reset();
    cancelSampleThumbnail_.cancel();
    pendingCompositorWarmup_ = false;
    cancelCompositorWarmup_.cancel();
    cancelRender_.cancel();
    workerState_ = ShutdownState{};
    initiatedShutdown = true;
  }
  if (initiatedShutdown) {
    cv_.notify_all();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

void AsyncRenderer::noteGpuWaitOutcome(const svg::RendererReadbackStats& readbackStats) {
  if (!readbackStats.deviceLost) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (gpuWaitFailure_.deviceLost &&
      gpuWaitFailure_.timedOutWaitSite == readbackStats.timedOutWaitSite &&
      gpuWaitFailure_.timedOutWaitMs == readbackStats.timedOutWaitMs) {
    // Device loss is sticky, so every later frame re-reports it. Leave the
    // generation alone for a repeat so a reader publishes each distinct
    // failure once instead of on every poll.
    return;
  }
  gpuWaitFailure_.deviceLost = true;
  gpuWaitFailure_.timedOutWaitSite = readbackStats.timedOutWaitSite;
  gpuWaitFailure_.timedOutWaitMs = readbackStats.timedOutWaitMs;
  ++gpuWaitFailure_.generation;
}

void AsyncRenderer::notePublishedCompositedPreview(
    const std::optional<RenderResult::CompositedPreview>& compositedPreview) {
  if (!compositedPreview.has_value() || !compositedPreview->valid()) {
    return;
  }

  // A full-canvas fallback replaces the split tile set in `GlTextureCache`,
  // so future split previews must resend pixels before switching back to
  // metadata-only updates.
  if (compositedPreview->tiles.size() == 1u &&
      compositedPreview->tiles.front().id == "full-canvas") {
    publishedCompositedTiles_.clear();
    return;
  }

  std::unordered_map<std::string, PublishedCompositedTile> nextPublished;
  nextPublished.reserve(compositedPreview->tiles.size());
  for (const RenderResult::CompositedTile& tile : compositedPreview->tiles) {
    if (!tile.bitmap.empty() || tile.textureSnapshot != nullptr) {
      const Vector2i bitmapDims =
          !tile.bitmap.empty() ? tile.bitmap.dimensions : tile.textureSnapshot->dimensions();
      nextPublished[tile.id] = PublishedCompositedTile{
          .kind = tile.kind,
          .generation = tile.generation,
          .bitmapDims = bitmapDims,
          .rasterCanvasSize = tile.rasterCanvasSize,
      };
    } else if (const auto it = publishedCompositedTiles_.find(tile.id);
               it != publishedCompositedTiles_.end()) {
      nextPublished[tile.id] = it->second;
    }
  }
  publishedCompositedTiles_ = std::move(nextPublished);
}

bool AsyncRenderer::workerStateBusy(const WorkerState& state) {
  return std::holds_alternative<RenderingState>(state) ||
         std::holds_alternative<CancellingState>(state) || std::holds_alternative<DoneState>(state);
}

bool AsyncRenderer::workerStateRenderInFlight(const WorkerState& state) {
  return std::holds_alternative<RenderingState>(state) ||
         std::holds_alternative<CancellingState>(state);
}

bool AsyncRenderer::isBusy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workerStateBusy(workerState_) || pendingCompositorWarmup_ || compositorWarmupActive_;
}

bool AsyncRenderer::hasRenderInFlightForTesting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workerStateRenderInFlight(workerState_) || pendingCompositorWarmup_ ||
         compositorWarmupActive_;
}

bool AsyncRenderer::hasPendingRenderForTesting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto* rendering = std::get_if<RenderingState>(&workerState_);
  return rendering != nullptr && rendering->pendingRequest.has_value();
}

bool AsyncRenderer::waitUntilNoRenderInFlightForTesting(
    std::chrono::steady_clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_until(lock, deadline, [this] {
    return !workerStateRenderInFlight(workerState_) && !pendingCompositorWarmup_ &&
           !compositorWarmupActive_;
  });
}

void AsyncRenderer::setReplayRenderDelayForTesting(std::chrono::milliseconds delay) {
  const std::chrono::milliseconds clampedDelay = std::max(delay, std::chrono::milliseconds(0));
  replayRenderDelayMsForTesting_.store(clampedDelay.count(), std::memory_order_release);
}

void AsyncRenderer::setReplayResultHoldFramesForTesting(int frameCount) {
  std::lock_guard<std::mutex> lock(mutex_);
  replayResultHoldFramesForTesting_ = std::max(frameCount, 0);
}

void AsyncRenderer::requestRender(const RenderRequest& request) {
  bool signalCancel = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::holds_alternative<ShutdownState>(workerState_)) {
      return;
    }
    RenderRequest stagedRequest = request;
    stagedRequest.queuedAt = std::chrono::steady_clock::now();
    if (!request.structuralRemap.empty()) {
      retainedStructuralRemaps_[request.documentGeneration] = request.structuralRemap;
    } else {
      const auto retainedIt = retainedStructuralRemaps_.find(request.documentGeneration);
      if (retainedIt != retainedStructuralRemaps_.end()) {
        stagedRequest.structuralRemap = retainedIt->second;
      }
    }

    // Foreground interaction always outranks speculative cache work. The compositor's normal
    // render path will finish any still-missing payloads against the newest DOM and viewport.
    pendingCompositorWarmup_ = false;
    if (compositorWarmupActive_) {
      cancelCompositorWarmup_.cancel();
    }

    if (auto* rendering = std::get_if<RenderingState>(&workerState_)) {
      // The newest request wins.
      rendering->pendingRequest.emplace(std::move(stagedRequest));
      signalCancel = true;
    } else {
      RenderingState nextRendering;
      nextRendering.pendingRequest.emplace(std::move(stagedRequest));
      signalCancel = std::holds_alternative<CancellingState>(workerState_);
      workerState_ = std::move(nextRendering);
    }
    if (signalCancel) {
      // Tell the in-flight render to bail. Set this while the mutex still
      // exposes the superseding state so the worker cannot start the
      // replacement request and then receive a stale cancel.
      cancelRender_.cancel();
    }
    if (sampleThumbnailActive_) {
      // Main-document presentation always preempts background preview work. The thumbnail
      // traversal polls its independent token between rendered entities. Worker-local offscreen
      // renderer construction cannot poll, so let that one first-use operation return before
      // applying the cancellation signal.
      if (sampleThumbnailRendererCreationActive_) {
        cancelSampleThumbnailAfterRendererCreation_ = true;
        if (!foregroundHandoffCountedForRendererCreation_) {
          foregroundHandoffCountedForRendererCreation_ = true;
          ++sampleThumbnailCounters_.foregroundHandoffWaits;
        }
      } else {
        cancelSampleThumbnail_.cancel();
      }
    }
  }
  cv_.notify_one();
}

void AsyncRenderer::cancelInFlight() {
  bool signalCancel = false;
  std::function<void()> wakeAfterSettledCancellation;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingCompositorWarmup_ = false;
    if (compositorWarmupActive_) {
      // Cancellation-token sampling is not an acknowledgment. The worker may have completed its
      // final poll while it still owns DocumentWriteAccess, so remember the waiter until the
      // active flag and guard are released together below.
      compositorWarmupReleaseWakePending_ = true;
      cancelCompositorWarmup_.cancel();
    }
    if (std::holds_alternative<RenderingState>(workerState_)) {
      // Worker is mid-renderFrame. Transition to `Cancelling` (not
      // `Idle`) so the editor's `!isBusy()` gates keep gating registry reads
      // until the worker actually observes the cancel and bails.
      workerState_ = CancellingState{};
      cancelRender_.cancel();
      signalCancel = true;
    } else if (std::holds_alternative<DoneState>(workerState_)) {
      // Worker raced to completion before we got here. The result
      // is already staged, but the user-input event that triggered this cancel
      // supersedes it. Drop the result and transition to Idle directly.
      workerState_ = IdleState{};
    }

    // `isBusy()` and `cancelInFlight()` cannot form an atomic check-then-act pair. If the worker
    // released its guard between those calls, cancellation is already settled and the deferred
    // input still needs a retry frame. Active render/cancellation states provide their own later
    // completion wake; an active compositor warmup uses the durable waiter above.
    const bool cancellationSettled =
        !compositorWarmupActive_ && (std::holds_alternative<IdleState>(workerState_) ||
                                     std::holds_alternative<DoneState>(workerState_));
    if (cancellationSettled) {
      wakeAfterSettledCancellation = wakeCallback_;
    }
  }
  if (signalCancel) {
    // Notify in case the worker was still in `cv_.wait` when we
    // landed - its updated predicate also wakes on `Cancelling`.
    cv_.notify_one();
  }
  if (wakeAfterSettledCancellation) {
    wakeAfterSettledCancellation();
  }
}

std::optional<RenderResult> AsyncRenderer::pollResult() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (auto* done = std::get_if<DoneState>(&workerState_)) {
    if (done->presentationHoldPollsRemaining > 0) {
      --done->presentationHoldPollsRemaining;
      replayResultHoldPollCount_.fetch_add(1, std::memory_order_release);
      const std::function<void()> wake = wakeCallback_;
      lock.unlock();
      // Wasm's expensive main-frame gate is event-driven even though its lightweight scheduler
      // runs every rAF. Request the next UI frame explicitly; otherwise the staged surface could
      // remain behind the prior epoch until another input event arrives.
      if (wake) {
        wake();
      }
      return std::nullopt;
    }

    RenderResult result = std::move(done->result);
    const HandoffTimings handoff =
        ComputeHandoffTimings(result.workerCompletedAt, std::chrono::steady_clock::now());
    result.workerTiming.pollDelayMs = handoff.pollDelayMs;
    result.workerTiming.wakeToPollMs = handoff.wakeToPollMs;
    if (!result.overviewInfillOnly) {
      notePublishedCompositedPreview(result.compositedPreview);
    }
    workerState_ = IdleState{};
    if (compositor_ != nullptr && compositor_->hasPendingFirstFrameWarmup()) {
      pendingCompositorWarmup_ = true;
    }
    const bool scheduleLowPriorityWork =
        pendingCompositorWarmup_ || pendingSampleThumbnail_.has_value();
    lock.unlock();
    cv_.notify_all();
    if (scheduleLowPriorityWork) {
      cv_.notify_one();
    }
    return result;
  }
  return std::nullopt;
}

bool AsyncRenderer::requestSampleThumbnail(SampleThumbnailRenderRequest request) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::holds_alternative<ShutdownState>(workerState_) ||
        pendingSampleThumbnail_.has_value() || sampleThumbnailActive_ ||
        sampleThumbnailResult_.has_value()) {
      return false;
    }
    pendingSampleThumbnail_.emplace(std::move(request));
    ++sampleThumbnailCounters_.requested;
  }
  cv_.notify_one();
  return true;
}

std::optional<SampleThumbnailRenderResult> AsyncRenderer::pollSampleThumbnailResult() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!sampleThumbnailResult_.has_value()) {
    return std::nullopt;
  }
  std::optional<SampleThumbnailRenderResult> result = std::move(sampleThumbnailResult_);
  sampleThumbnailResult_.reset();
  return result;
}

void AsyncRenderer::cancelSampleThumbnailWork() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pendingSampleThumbnail_.has_value()) {
    pendingSampleThumbnail_.reset();
    ++sampleThumbnailCounters_.completed;
    ++sampleThumbnailCounters_.cancelled;
  }
  sampleThumbnailResult_.reset();
  if (sampleThumbnailActive_) {
    discardActiveSampleThumbnailResult_ = true;
    if (sampleThumbnailRendererCreationActive_) {
      cancelSampleThumbnailAfterRendererCreation_ = true;
    } else {
      cancelSampleThumbnail_.cancel();
    }
  }
}

SampleThumbnailRenderStats AsyncRenderer::sampleThumbnailRenderStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  SampleThumbnailRenderStats stats = sampleThumbnailCounters_;
  stats.pending = pendingSampleThumbnail_.has_value();
  stats.active = sampleThumbnailActive_;
  stats.resultReady = sampleThumbnailResult_.has_value();
  return stats;
}

void AsyncRenderer::setSampleThumbnailRenderDelayForTesting(std::chrono::milliseconds delay) {
  const std::chrono::milliseconds clamped = std::max(delay, std::chrono::milliseconds(0));
  sampleThumbnailRenderDelayMsForTesting_.store(clamped.count(), std::memory_order_release);
}

void AsyncRenderer::setWakeCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::holds_alternative<ShutdownState>(workerState_)) {
    return;
  }
  wakeCallback_ = std::move(callback);
}

void AsyncRenderer::setSampleThumbnailRendererCreationPlanForTesting(
    int requestNumber, std::chrono::milliseconds delay) {
  constexpr int kMaximumRequestNumber = 64;
  constexpr std::chrono::milliseconds kMaximumDelay(5000);
  sampleThumbnailRendererCreationRequestForTesting_.store(
      std::clamp(requestNumber, 0, kMaximumRequestNumber), std::memory_order_release);
  sampleThumbnailRendererCreationDelayMsForTesting_.store(
      std::clamp(delay, std::chrono::milliseconds(0), kMaximumDelay).count(),
      std::memory_order_release);
}

bool AsyncRenderer::prepareSampleThumbnailRendererForRequest(
    std::unique_ptr<svg::RendererInterface>& renderer, svg::RendererInterface*& rendererRoot,
    svg::RendererInterface* requestedRoot) {
  const int nextRequest = static_cast<int>(sampleThumbnailCounters_.started + 1);
  const int recreateRequest =
      sampleThumbnailRendererCreationRequestForTesting_.load(std::memory_order_acquire);
  const bool forceRecreate = recreateRequest > 0 && nextRequest == recreateRequest;
  if (forceRecreate) {
    renderer.reset();
    rendererRoot = nullptr;
  }
  sampleThumbnailRendererCreationActive_ = renderer == nullptr || rendererRoot != requestedRoot;
  return forceRecreate;
}

void AsyncRenderer::delaySampleThumbnailRendererCreationForTesting(bool shouldDelay,
                                                                   int constructionStart) const {
  if (!shouldDelay) {
    return;
  }
  const auto delay = std::chrono::milliseconds(
      sampleThumbnailRendererCreationDelayMsForTesting_.load(std::memory_order_acquire));
  if (delay.count() <= 0) {
    return;
  }
#ifdef __EMSCRIPTEN__
  MAIN_THREAD_EM_ASM(
      {
        window['__donnerSampleThumbnailRendererCreationBlocked'] = ({
          'blocked' : true,
          'constructionStarts' : $0,
        });
      },
      constructionStart);
#endif
  std::this_thread::sleep_for(delay);
#ifdef __EMSCRIPTEN__
  MAIN_THREAD_EM_ASM(
      { window['__donnerSampleThumbnailRendererCreationBlocked']['blocked'] = false; });
#endif
}

void AsyncRenderer::finishSampleThumbnailRendererCreation() {
  bool cancelAfterRendererCreation = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sampleThumbnailRendererCreationActive_) {
      sampleThumbnailRendererCreationActive_ = false;
      sampleThumbnailCounters_.firstAttemptCompleted = true;
      cancelAfterRendererCreation = cancelSampleThumbnailAfterRendererCreation_;
      cancelSampleThumbnailAfterRendererCreation_ = false;
      foregroundHandoffCountedForRendererCreation_ = false;
    }
  }
  if (cancelAfterRendererCreation) {
    cancelSampleThumbnail_.cancel();
  }
}

void AsyncRenderer::workerLoop() {
#if defined(__EMSCRIPTEN__)
  // Emscripten's WebGPU object table is per-worker. Construct and use the
  // renderer on this pthread so wgpu handles never cross JS worker boundaries.
  svg::Renderer workerRenderer;
#endif
  std::unique_ptr<svg::RendererInterface> sampleThumbnailRenderer;
  svg::RendererInterface* sampleThumbnailRendererRoot = nullptr;

  while (true) {
    std::optional<RenderRequest> requestStorage;
    std::optional<SampleThumbnailRenderRequest> sampleThumbnailStorage;
    bool runCompositorWarmup = false;
    [[maybe_unused]] bool delaySampleThumbnailRendererCreation = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] {
        return std::holds_alternative<RenderingState>(workerState_) ||
               std::holds_alternative<CancellingState>(workerState_) ||
               std::holds_alternative<ShutdownState>(workerState_) ||
               (std::holds_alternative<IdleState>(workerState_) &&
                (pendingCompositorWarmup_ || pendingSampleThumbnail_.has_value()));
      });
      if (std::holds_alternative<ShutdownState>(workerState_)) {
#ifdef __EMSCRIPTEN__
        // `workerRenderer` is destroyed here in Emscripten builds, i.e. on
        // the worker thread, mirroring its construction.
#endif
        return;
      }
      if (std::holds_alternative<CancellingState>(workerState_)) {
        // `cancelInFlight` raced with the worker before it could
        // start renderFrame. Transition to Idle and loop back to cv_.wait.
        std::function<void()> wake = wakeCallback_;
        workerState_ = IdleState{};
        lock.unlock();
        cv_.notify_all();
        if (wake) {
          wake();
        }
        continue;
      }
      if (auto* rendering = std::get_if<RenderingState>(&workerState_)) {
        assert(rendering->pendingRequest.has_value() &&
               "Rendering worker state requires a pending request while waiting");
        requestStorage.emplace(std::move(*rendering->pendingRequest));
        rendering->pendingRequest.reset();
      } else {
        assert(std::holds_alternative<IdleState>(workerState_));
        if (pendingCompositorWarmup_) {
          pendingCompositorWarmup_ = false;
          cancelCompositorWarmup_.reset();
          compositorWarmupActive_ = true;
          runCompositorWarmup = true;
        } else {
          assert(pendingSampleThumbnail_.has_value());
          sampleThumbnailStorage.emplace(std::move(*pendingSampleThumbnail_));
          pendingSampleThumbnail_.reset();
          cancelSampleThumbnail_.reset();
          sampleThumbnailActive_ = true;
          discardActiveSampleThumbnailResult_ = false;
          cancelSampleThumbnailAfterRendererCreation_ = false;
          foregroundHandoffCountedForRendererCreation_ = false;
#if defined(__EMSCRIPTEN__)
          delaySampleThumbnailRendererCreation = prepareSampleThumbnailRendererForRequest(
              sampleThumbnailRenderer, sampleThumbnailRendererRoot, &workerRenderer);
#else
          delaySampleThumbnailRendererCreation = prepareSampleThumbnailRendererForRequest(
              sampleThumbnailRenderer, sampleThumbnailRendererRoot,
              sampleThumbnailStorage->nativeRenderer);
#endif
          ++sampleThumbnailCounters_.started;
        }
      }
    }

    if (runCompositorWarmup) {
      // Re-check the cancel signal after dequeue: a document swap or
      // cancelInFlight between the dequeue above and this call tears down the
      // lease renderer the compositor is bound to, and the warmup's first
      // renderer dereference happens before any in-pass cancellation poll.
      if (compositor_ != nullptr && compositorDocument_.has_value() &&
          !cancelCompositorWarmup_.isCancelled()) {
        svg::SVGDocument& warmupDocument = *compositorDocument_;
        std::optional<svg::DocumentWriteAccess> documentAccess;
        documentAccess.emplace(warmupDocument.writeAccess());
        (void)compositor_->warmPendingFirstFrameCaches(cancelCompositorWarmup_);
      }

      std::function<void()> wake;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        compositorWarmupActive_ = false;
        const bool releaseWakePending = std::exchange(compositorWarmupReleaseWakePending_, false);
        if (releaseWakePending && !std::holds_alternative<ShutdownState>(workerState_)) {
          // Speculative work changes no visible state, including when a foreground render
          // preempts it. Wake only a caller that explicitly registered against the document
          // guard's release; the foreground render supplies its own completion wake.
          wake = wakeCallback_;
        }
      }
      cv_.notify_all();
      if (wake) {
        wake();
      }
      continue;
    }

    if (sampleThumbnailStorage.has_value()) {
      svg::RendererInterface* offscreenRenderer = nullptr;
#if defined(__EMSCRIPTEN__)
      if (sampleThumbnailRenderer == nullptr || sampleThumbnailRendererRoot != &workerRenderer) {
        workerRenderer.setOffscreenCreationHookForTesting([this,
                                                           delaySampleThumbnailRendererCreation] {
          int constructionStart = 0;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            constructionStart =
                static_cast<int>(++sampleThumbnailCounters_.offscreenRendererConstructionStarts);
            sampleThumbnailCounters_.offscreenRendererConstructionBlocked =
                delaySampleThumbnailRendererCreation;
          }
          delaySampleThumbnailRendererCreationForTesting(delaySampleThumbnailRendererCreation,
                                                         constructionStart);
          {
            std::lock_guard<std::mutex> lock(mutex_);
            sampleThumbnailCounters_.offscreenRendererConstructionBlocked = false;
          }
        });
        sampleThumbnailRenderer = workerRenderer.createOffscreenInstance();
        workerRenderer.setOffscreenCreationHookForTesting({});
        sampleThumbnailRendererRoot = &workerRenderer;
        if (sampleThumbnailRenderer != nullptr) {
          std::lock_guard<std::mutex> lock(mutex_);
          ++sampleThumbnailCounters_.offscreenRendererCreations;
        }
      }
      offscreenRenderer = sampleThumbnailRenderer.get();
#else
      svg::RendererInterface* requestedRoot = sampleThumbnailStorage->nativeRenderer;
      if (requestedRoot != nullptr &&
          (sampleThumbnailRenderer == nullptr || sampleThumbnailRendererRoot != requestedRoot)) {
        sampleThumbnailRenderer = requestedRoot->createOffscreenInstance();
        sampleThumbnailRendererRoot = requestedRoot;
        if (sampleThumbnailRenderer != nullptr) {
          std::lock_guard<std::mutex> lock(mutex_);
          ++sampleThumbnailCounters_.offscreenRendererCreations;
        }
      }
      offscreenRenderer = requestedRoot != nullptr ? sampleThumbnailRenderer.get() : nullptr;
#endif

      finishSampleThumbnailRendererCreation();

      SampleThumbnailRenderResult result;
      if (offscreenRenderer == nullptr) {
        result.kind = sampleThumbnailStorage->kind;
        result.key = sampleThumbnailStorage->key;
        result.outcome = SampleThumbnailRenderOutcome::RendererUnavailable;
      } else {
        const std::chrono::milliseconds delay(
            sampleThumbnailRenderDelayMsForTesting_.load(std::memory_order_acquire));
        result = RenderSampleThumbnail(std::move(*sampleThumbnailStorage), *offscreenRenderer,
                                       cancelSampleThumbnail_, delay);
      }

      std::function<void()> wake;
      bool notifyStateChange = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        sampleThumbnailActive_ = false;
        if (!std::holds_alternative<ShutdownState>(workerState_)) {
          ++sampleThumbnailCounters_.completed;
          if (discardActiveSampleThumbnailResult_) {
            ++sampleThumbnailCounters_.cancelled;
          } else {
            if (result.outcome == SampleThumbnailRenderOutcome::Rendered) {
              ++sampleThumbnailCounters_.rendered;
            } else if (result.outcome == SampleThumbnailRenderOutcome::Cancelled) {
              ++sampleThumbnailCounters_.cancelled;
            }
            sampleThumbnailResult_.emplace(std::move(result));
          }
          discardActiveSampleThumbnailResult_ = false;
          wake = wakeCallback_;
          notifyStateChange = true;
        }
      }
      if (notifyStateChange) {
        cv_.notify_all();
      }
      if (wake) {
        wake();
      }
      continue;
    }

    assert(requestStorage.has_value());
    RenderRequest& request = *requestStorage;
    const auto workerDequeuedAt = std::chrono::steady_clock::now();
    const double queueWaitMs =
        request.queuedAt.time_since_epoch().count() == 0
            ? 0.0
            : std::chrono::duration<double, std::milli>(workerDequeuedAt - request.queuedAt)
                  .count();
#if defined(__EMSCRIPTEN__)
    svg::Renderer& requestRenderer = workerRenderer;
#else
    // Geode editor builds intentionally use the request renderer so worker texture snapshots are
    // created on the same WGPU device as ImGui presentation.
    svg::Renderer& requestRenderer = request.lease.renderer();
#endif
    // Readback counters live on the backend device shared by the root renderer
    // and compositor offscreens. Start each worker iteration from a clean epoch.
    (void)requestRenderer.consumeReadbackStats();
    svg::SVGDocument& requestDocument = request.lease.document();

    // Every iteration starts with a fresh (non-cancelled) token.
    // The UI thread sets cancel via `requestRender` ONLY when posting
    // while busy, and we're idle here right before the render runs -
    // so any cancel signal from a previous iteration is stale.
    cancelRender_.reset();

    // Execute the render outside the lock so the UI thread can poll
    // `isBusy()` / `pollResult()` while we work.
    ZoneScopedN("AsyncRenderer::workerIteration");
    // Stage brackets for the byte attribution (see
    // `donner/base/MemoryAttribution.h`). `WorkerOther` spans the whole
    // iteration; the three inner stages are disjoint and are subtracted from it
    // when the numbers are read, so a stage that retains is named exactly.
    const ScopedHeapDelta workerIterationHeapDelta(MemoryStage::WorkerOther);
    const auto workerStart = std::chrono::steady_clock::now();
    const auto elapsedSince = [](std::chrono::steady_clock::time_point start) {
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
          .count();
    };
    RenderResult::WorkerTimingBreakdown workerTiming;
    workerTiming.queueWaitMs = queueWaitMs;
    workerTiming.dequeueToStartMs =
        std::chrono::duration<double, std::milli>(workerStart - workerDequeuedAt).count();
    std::optional<RenderResult::CompositedPreview> compositedPreview;

    // §concurrent-dom: serialize this worker render against UI-thread DOM reads. The lease shares
    // the live registry (it does not snapshot), and the worker cannot touch the document in
    // SingleThreaded mode (owner-thread assert). The document is flipped to ConcurrentDom on first
    // render and stays there for the editor's lifetime - UI-thread reads are responsible for
    // holding their own access guard (`withReadAccess` / a scoped `DocumentReadAccess`) where they
    // touch the live document. The worker holds a write guard across the document-reading render
    // work and releases it via `releaseDocumentAccess()` before every `mutex_` section below to
    // avoid a lock-order inversion against UI threads holding `mutex_` while reading the DOM.
    if (requestDocument.threadingMode() != svg::ThreadingMode::ConcurrentDom) {
      requestDocument.setThreadingMode(svg::ThreadingMode::ConcurrentDom);
    }
    std::optional<svg::DocumentWriteAccess> documentAccess;
    documentAccess.emplace(requestDocument.writeAccess());
    const auto releaseDocumentAccess = [&]() { documentAccess.reset(); };
    const EditorRasterViewport rasterViewport =
        EffectiveRasterViewportForRequest(requestDocument, request.rasterViewport);

    // Compositor lifecycle is split into two independent decisions:
    //
    //   1. Do we need a fresh `CompositorController` instance? Only
    //      on first construction or when the renderer pointer changes
    //      (e.g. backend swap). The renderer is owned by the worker
    //      and constructed at the top of `workerLoop`, so in steady
    //      state this is just the first-frame case.
    //
    //   2. Did the document space swap underneath us? `setDocument`
    //      and `setDocumentMaybeStructural` both bump
    //      `documentGeneration` and produce a fresh `Registry` (the
    //      `SVGDocumentHandle` pointer changes). When that happens we
    //      try the structural-remap path FIRST - it preserves cached
    //      filter / bucket bitmaps, `canvasFromBitmap` stamps, and
    //      the pre-warmed bg/fg pair - and only fall back to a
    //      destructive `resetAllLayers(documentReplaced=true)` when
    //      no remap is available or the remap itself fails an
    //      invariant check.
    //
    // The previous implementation collapsed step 1 onto a pointer-
    // identity check that fired on every `setDocumentMaybeStructural`
    // (since the new doc carries a new `Registry` handle), making
    // step 2's structural-remap branch unreachable on the
    // drag-release writeback path. The user-visible symptom was a
    // filter-group "snap back to original position" on drag release:
    // the freshly-reconstructed compositor blitted its zero-offset
    // bitmap of the pre-drag layer state while the editor's cached
    // GL textures still showed the dragged element at its rasterize-
    // time position. Pinned by
    // `RnrReplayTest::FilterSnapbackReproPreservesCompositorAcrossWriteback`.
    // A third lifecycle input joins the two above: the UI-selected composited
    // rendering mode. `Off` runs with no controller at all (every
    // `compositor_` consumer below is null-guarded); the other modes bake
    // their policy into `CompositorConfig` at construction, so any mode
    // change reconstructs.
    const CompositedRenderingMode renderMode =
        compositedRenderingMode_.load(std::memory_order_acquire);
    const bool renderModeChanged = renderMode != appliedCompositedRenderingMode_;
    appliedCompositedRenderingMode_ = renderMode;
    if (renderMode == CompositedRenderingMode::Off && compositor_ != nullptr) {
      // Entering Off: destroy the controller and every retained cache with it.
      // `compositorDocument_` exists only to give the controller a stable
      // SVGDocument reference, so it is released too.
      compositor_.reset();
      compositorDocument_.reset();
      compositorRenderer_ = nullptr;
      compositorEntity_ = entt::null;
      compositorEntities_.clear();
      compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
      publishedCompositedTiles_.clear();
      // The per-generation remap history exists only to preserve compositor
      // caches across structural replaces; with no compositor the erase sites
      // in the swap path are unreachable, so drop the history here or a long
      // Off session accumulates one map per structural replace. Leaving Off
      // always reconstructs and needs no remap history.
      {
        std::lock_guard<std::mutex> lock(mutex_);
        retainedStructuralRemaps_.clear();
      }
    }
    const bool needsFreshCompositor =
        renderMode != CompositedRenderingMode::Off &&
        (!compositor_ || compositorRenderer_ != &requestRenderer || renderModeChanged);
    if (needsFreshCompositor) {
      svg::compositor::CompositorConfig compositorConfig;
      if (renderMode == CompositedRenderingMode::FilterOnly) {
        // Filter-only: cached isolated layers for SVG filters (the expensive
        // re-render case) and nothing else. Opacity groups, blend modes, and
        // masks render inline; animation subtrees are not promoted and the
        // document is not pre-split into bucket layers. Selection/drag layers
        // remain active because they are structural editor presentation, not
        // optional retained-content caching.
        compositorConfig.mandatoryHintScope = svg::compositor::MandatoryHintScope::FilterOnly;
        compositorConfig.autoPromoteAnimations = false;
        compositorConfig.complexityBucketing = false;
      }
      // Every non-Off editor frame is presented from the compositor's paint-order tile set.
      // Preserve the normal immediate-span policy so cheap background/selection/foreground spans
      // remain layers without retaining needless textures, and finish the first-frame warmup before
      // publishing so a cold frame cannot fall back to a monolithic full-canvas payload.
      compositorConfig.deferFirstFrameWarmup = false;
      // CompositorController stores its SVGDocument by reference. Bind that reference to the
      // AsyncRenderer-owned value before constructing the controller: RenderLease is destroyed
      // after this request, while deferred warmup and later frames intentionally outlive it.
      compositor_.reset();
      compositorDocument_.emplace(requestDocument);  // Cheap: refcount bump on the Registry.
      compositor_ = std::make_unique<svg::compositor::CompositorController>(
          *compositorDocument_, requestRenderer, compositorConfig);
      compositorRenderer_ = &requestRenderer;
      compositorEntity_ = entt::null;
      compositorEntities_.clear();
      compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
      compositorDocumentGeneration_ = request.documentGeneration;
      publishedCompositedTiles_.clear();
      compositorReconstructCount_.fetch_add(1, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        retainedStructuralRemaps_.erase(request.documentGeneration);
      }
    }

    const bool documentSwapDetected =
        compositor_ != nullptr && !needsFreshCompositor &&
        (request.documentGeneration != compositorDocumentGeneration_ ||
         (compositorDocument_.has_value() &&
          compositorDocument_->handle().get() != requestDocument.handle().get()));
    if (documentSwapDetected) {
      // Preserve the SVGDocument object's address because CompositorController references it, but
      // update its shared Registry handle before asking the compositor to remap/reset against the
      // replacement entity space.
      assert(compositorDocument_.has_value());
      *compositorDocument_ = requestDocument;
      bool remapped = false;
      if (!request.structuralRemap.empty()) {
        remapped = compositor_->remapAfterStructuralReplace(request.structuralRemap);
        if (remapped && compositorEntity_ != entt::null) {
          const auto it = request.structuralRemap.find(compositorEntity_);
          if (it != request.structuralRemap.end()) {
            compositorEntity_ = it->second;
            std::vector<Entity> remappedEntities;
            remappedEntities.reserve(compositorEntities_.size());
            for (Entity entity : compositorEntities_) {
              const auto entityIt = request.structuralRemap.find(entity);
              if (entityIt != request.structuralRemap.end()) {
                AppendUniqueEntity(&remappedEntities, entityIt->second);
              }
            }
            compositorEntities_ = std::move(remappedEntities);
          } else {
            // The drag/selection target didn't survive the remap - fall
            // through to the reset branch so subsequent promote calls
            // start clean.
            compositorEntity_ = entt::null;
            remapped = false;
          }
        }
      }
      if (!remapped) {
        compositor_->resetAllLayers(/*documentReplaced=*/true);
        compositorEntity_ = entt::null;
        compositorEntities_.clear();
        compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
        compositorResetCount_.fetch_add(1, std::memory_order_release);
      }
      publishedCompositedTiles_.clear();
      compositorDocumentGeneration_ = request.documentGeneration;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        retainedStructuralRemaps_.erase(request.documentGeneration);
      }
    }

    // Geometry debug is a frame-global final pass. Retained segmented tiles can
    // crop or cover its wireframe, so every debug frame stays flat and carries
    // no selection prewarm/promotion state. Toggling back off resets once more,
    // then the normal promotion path below rebuilds the selected layer.
    const bool geometryDebugOverlayRequested =
        geometryDebugOverlay_.load(std::memory_order_acquire);
    requestRenderer.setDebugGeometryOverlay(geometryDebugOverlayRequested);
    // Unsupported backends intentionally ignore the request and report false.
    // Keep their normal retained-presentation path active instead of clearing
    // selection prewarm for a debug pass they cannot draw.
    const bool geometryDebugOverlay = requestRenderer.debugGeometryOverlay();
    const bool geometryDebugOverlayChanged = geometryDebugOverlay != appliedGeometryDebugOverlay_;
    if (geometryDebugOverlayChanged) {
      appliedGeometryDebugOverlay_ = geometryDebugOverlay;
      if (compositor_ != nullptr) {
        compositor_->resetAllLayers();
        compositorResetCount_.fetch_add(1, std::memory_order_release);
      }
      compositorEntity_ = entt::null;
      compositorEntities_.clear();
      compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
      publishedCompositedTiles_.clear();
    }

    // Resolve what the compositor should be promoted on this render.
    // Priority: explicit drag targets win over the persistent selection hint;
    // otherwise we keep the selected entity promoted so the next drag
    // arrives with everything pre-warmed. Multi-select drags intentionally
    // promote every selected participant: the presenter applies one shared
    // document-space transform to every drag-target tile, keeping the path
    // overlay and cached content in lockstep while avoiding a full DOM render
    // on each pointer frame.
    // Selection/drag promotion is structural editor presentation in both non-Off modes. FilterOnly
    // narrows automatic retained-content layers; it does not collapse selected content back into a
    // root render.
    const bool editorPromotionEnabled = compositor_ != nullptr;
    const std::vector<Entity> desiredEntities = (geometryDebugOverlay || !editorPromotionEnabled)
                                                    ? std::vector<Entity>()
                                                    : DesiredCompositorEntities(request);
    const Entity desiredEntity = desiredEntities.empty() ? entt::null : desiredEntities.front();
    const svg::compositor::InteractionHint desiredKind =
        request.dragPreview.has_value() ? request.dragPreview->interactionKind
                                        : svg::compositor::InteractionHint::Selection;

    // Re-promote when EITHER the entity changes OR the kind changes (the
    // editor flips Selection → ActiveDrag at drag start without changing
    // the entity). The compositor's `promoteEntity` refreshes the kind
    // in place for an already-promoted entity instead of demoting and
    // re-promoting, so the layer's cached bitmap survives the
    // transition. Skipping the kind-change re-promote left the
    // compositor treating an active drag as a Selection prewarm and
    // tripped the descendant-segment dirty cascade every drag frame
    // post-zoom - sustained > 1 s/frame on the splash.
    const bool entityChanged = !SameEntityList(compositorEntities_, desiredEntities);
    // Keep a selected entity in ActiveDrag mode after mouse-up so the
    // layer/segment caches stay hot for release-to-drag cycles. The
    // interaction kind changes back to Selection only when a different
    // entity is promoted.
    const bool kindUpgrade =
        desiredEntity != entt::null &&
        compositorInteractionKind_ == svg::compositor::InteractionHint::Selection &&
        desiredKind == svg::compositor::InteractionHint::ActiveDrag;
    if (entityChanged || kindUpgrade) {
      if (entityChanged) {
        for (Entity oldEntity : compositorEntities_) {
          if (!ContainsEntity(desiredEntities, oldEntity)) {
            compositor_->demoteEntity(oldEntity);
          }
        }
        compositorEntities_.clear();
        compositorEntity_ = entt::null;
        compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
      }

      for (Entity entity : desiredEntities) {
        const svg::compositor::CompositorController::PromoteResult promoteResult =
            compositor_->promoteEntity(entity, desiredKind);
        if (promoteResult.promotedLayer()) {
          AppendUniqueEntity(&compositorEntities_, entity);
          if (compositorEntity_ == entt::null) {
            compositorEntity_ = entity;
          }
          compositorInteractionKind_ = desiredKind;
        } else if (promoteResult.owningTilesRequired()) {
          // The requested entity remains inside the paint-order tiles that own its ancestor
          // compositing context. Only the interaction split is unavailable.
        }
      }
      if (compositorEntities_.empty()) {
        compositorEntity_ = entt::null;
      }
    }
    if (editorPromotionEnabled && request.dragPreview.has_value() &&
        request.dragPreview->forceLayerRasterization) {
      for (Entity entity : DragPreviewEntities(*request.dragPreview)) {
        compositor_->markPromotedLayerDirty(entity);
      }
    }
    const bool desiredPromotionIncomplete =
        !desiredEntities.empty() &&
        !compositor_->interactionLayersCover(compositorEntities_, desiredEntities);

    // The DOM is the sole source of truth for the dragged entity's
    // position - `SelectTool` mutates the `transform` attribute every
    // drag frame, so by the time we reach here the compositor's fast
    // path will diff the new DOM transform against the cached bitmap's
    // rasterize-time transform and either reuse the bitmap via a
    // pure-translation compose offset or mark it dirty for re-rasterize.
    // No emulation layer on top of the DOM.
    svg::RenderViewport viewport;
    const Vector2i semanticCanvasSize = requestDocument.canvasSize();
    [[maybe_unused]] const Box2d documentViewBox = requestDocument.svgElement().viewBox().value_or(
        Box2d::FromXYWH(0, 0, static_cast<double>(semanticCanvasSize.x),
                        static_cast<double>(semanticCanvasSize.y)));
    const Vector2i outputCanvasSize = rasterViewport.outputSizePx;
    viewport.size = Vector2d(outputCanvasSize.x, outputCanvasSize.y);
    viewport.devicePixelRatio = 1.0;
    const Transform2d semanticCanvasFromDocument = requestDocument.canvasFromDocumentTransform();
    const Transform2d surfaceFromCanvas =
        semanticCanvasFromDocument.inverse() * rasterViewport.outputFromDocument;
    // Push the current UI-thread setting for tight-bounded segments
    // into the compositor. Setter is a no-op when unchanged; otherwise
    // it marks all segments dirty so the flip takes effect this frame.
    if (compositor_ != nullptr) {
      compositor_->setTightBoundedSegmentsEnabled(
          tightBoundedSegments_.load(std::memory_order_acquire));
    }

    // Keep the compositor hint in ActiveDrag across mouse-up so the
    // layer/segment caches survive quick release->drag-again cycles, but
    // only skip the main-renderer compose while an actual drag request is
    // in flight. Post-release and Selection-prewarm renders must refresh
    // the final CPU snapshot so the full-canvas composited tile, when
    // needed, matches the DOM and tile metadata.
    const bool activeDragRequest =
        request.dragPreview.has_value() &&
        request.dragPreview->interactionKind == svg::compositor::InteractionHint::ActiveDrag;
    const bool splitPreviewSafe = !desiredPromotionIncomplete;
    if (compositor_ != nullptr) {
      // `!desiredEntities.empty()` protects requests without editor selection/drag promotion:
      // `desiredPromotionIncomplete` is vacuously false when nothing is requested, but skipping the
      // main compose would leave diagnostic snapshots stale.
      compositor_->setSkipMainComposeDuringSplit(activeDragRequest && splitPreviewSafe &&
                                                 !desiredEntities.empty() &&
                                                 !request.captureCpuSnapshot);
    }
    workerTiming.setupMs = elapsedSince(workerStart);

    // Build a CompositedPreview from the compositor's current tile state.
    // Tiles whose id/generation/dimensions were already published carry
    // metadata only; the GL cache keeps the existing texture and applies
    // updated presentation geometry.
    bool desiredPromotionCoverageCompleteAfterRender = false;
    const auto buildCompositedPreview = [&]() -> std::optional<RenderResult::CompositedPreview> {
      if (request.overviewInfillOnly) {
        return std::nullopt;
      }
      if (compositor_ == nullptr) {
        return std::nullopt;
      }
      const std::vector<Entity> dragPreviewEntities =
          request.dragPreview.has_value() ? DragPreviewEntities(*request.dragPreview)
                                          : std::vector<Entity>();
      const Entity previewEntity =
          desiredPromotionCoverageCompleteAfterRender ? compositorEntity_ : entt::null;
      const svg::compositor::InteractionHint previewInteractionKind =
          request.dragPreview.has_value() ? request.dragPreview->interactionKind
                                          : svg::compositor::InteractionHint::Selection;
      const Transform2d documentFromOutput = rasterViewport.outputFromDocument.inverse();
      const auto outputPointToPresentedDoc = [&](const Vector2d& outputPoint) {
        return documentFromOutput.transformPosition(outputPoint) - documentViewBox.topLeft;
      };
      const auto outputVectorToDoc = [&](const Vector2d& outputVector) {
        return documentFromOutput.transformVector(outputVector);
      };
      const auto documentFromCachedDocument = [&](const Transform2d& outputFromCachedOutput) {
        return rasterViewport.outputFromDocument * outputFromCachedOutput * documentFromOutput;
      };
      const auto publishedTextureMatches = [this](const std::string& tileId,
                                                  RenderResult::CompositedTile::Kind kind,
                                                  std::uint64_t generation,
                                                  const Vector2i& bitmapDims,
                                                  const Vector2i& rasterCanvasSize) {
        const auto publishedIt = publishedCompositedTiles_.find(tileId);
        return publishedIt != publishedCompositedTiles_.end() && publishedIt->second.kind == kind &&
               publishedIt->second.generation == generation &&
               publishedIt->second.bitmapDims.x == bitmapDims.x &&
               publishedIt->second.bitmapDims.y == bitmapDims.y &&
               publishedIt->second.rasterCanvasSize.x == rasterCanvasSize.x &&
               publishedIt->second.rasterCanvasSize.y == rasterCanvasSize.y;
      };
      const auto outputTileId = [](const svg::compositor::CompositorTile& ct) {
        // Immediate (direct-rendered) static segments share the same stable tile
        // identity as composited static segments. The identity must NOT encode
        // the generation: a steady drag frame leaves the underlying segment
        // unchanged, so a generation-suffixed id would make every frame look
        // like a brand-new tile and defeat texture/metadata reuse. Generation
        // is tracked separately on the output tile.
        return std::to_string(ct.tileId);
      };
      const auto outputTileKind = [](const svg::compositor::CompositorTile& ct) {
        using OutKind = RenderResult::CompositedTile::Kind;
        if (ct.layerEntity != entt::null) {
          return OutKind::Layer;
        }
        return ct.immediate ? OutKind::Immediate : OutKind::Segment;
      };

      using svg::compositor::CompositorTileBitmapPayload;
      auto compositorTiles =
          compositor_->snapshotTilesForUpload(CompositorTileBitmapPayload::MetadataOnly);
      const bool metadataReuseRequest =
          activeDragRequest ||
          compositorInteractionKind_ == svg::compositor::InteractionHint::ActiveDrag;
      bool canReuseNonDragTextures = !publishedCompositedTiles_.empty();
      std::size_t activeDragTilesAvailable = 0u;
      bool activeDragTileNeedsPayload = false;
      bool hasImmediateTile = false;
      for (const auto& ct : compositorTiles) {
        if (ct.bitmapDims.x <= 0 || ct.bitmapDims.y <= 0) {
          continue;
        }
        using OutKind = RenderResult::CompositedTile::Kind;
        const OutKind kind = outputTileKind(ct);
        const bool currentActiveDragLayer =
            activeDragRequest && ContainsEntity(dragPreviewEntities, ct.layerEntity);
        const std::string tileId = outputTileId(ct);
        if (kind == OutKind::Immediate) {
          hasImmediateTile = true;
          if (metadataReuseRequest && !publishedTextureMatches(tileId, kind, ct.generation,
                                                               ct.bitmapDims, outputCanvasSize)) {
            canReuseNonDragTextures = false;
            break;
          }
          if (currentActiveDragLayer) {
            ++activeDragTilesAvailable;
          }
          continue;
        }
        if (currentActiveDragLayer) {
          ++activeDragTilesAvailable;
          activeDragTileNeedsPayload = !publishedTextureMatches(tileId, kind, ct.generation,
                                                                ct.bitmapDims, outputCanvasSize);
          continue;
        }
        if (ct.isDragTarget && activeDragRequest) continue;
        if (!publishedTextureMatches(tileId, kind, ct.generation, ct.bitmapDims,
                                     outputCanvasSize)) {
          canReuseNonDragTextures = false;
          break;
        }
      }
      if (activeDragRequest && activeDragTilesAvailable < dragPreviewEntities.size()) {
        canReuseNonDragTextures = false;
      }
      CompositorTileBitmapPayload payload = CompositorTileBitmapPayload::All;
      if (canReuseNonDragTextures) {
        if (metadataReuseRequest && activeDragTileNeedsPayload) {
          payload = CompositorTileBitmapPayload::DragTargetOnly;
        } else if (metadataReuseRequest) {
          payload = CompositorTileBitmapPayload::MetadataOnly;
        } else if (hasImmediateTile) {
          payload = CompositorTileBitmapPayload::ImmediateOnly;
        } else if (activeDragTileNeedsPayload) {
          payload = CompositorTileBitmapPayload::DragTargetOnly;
        } else {
          payload = CompositorTileBitmapPayload::MetadataOnly;
        }
      }
      if (payload != CompositorTileBitmapPayload::MetadataOnly) {
        compositorTiles = compositor_->snapshotTilesForUpload(payload);
      }
      std::vector<RenderResult::CompositedTile> previewTiles;
      previewTiles.reserve(compositorTiles.size());
      for (auto& ct : compositorTiles) {
        if (ct.bitmapDims.x <= 0 || ct.bitmapDims.y <= 0) continue;
        using OutKind = RenderResult::CompositedTile::Kind;
        const std::string tileId = outputTileId(ct);
        const OutKind kind = outputTileKind(ct);
        const bool hasPayload = !ct.bitmap.empty() || ct.textureSnapshot != nullptr;
        const bool metadataOnly =
            !hasPayload &&
            publishedTextureMatches(tileId, kind, ct.generation, ct.bitmapDims, outputCanvasSize);
        if (!metadataOnly && !hasPayload) continue;
        RenderResult::CompositedTile tile;
        tile.kind = kind;
        tile.id = tileId;
        tile.layerEntity = ct.layerEntity;
        tile.generation = ct.generation;
        tile.bitmapDimsPx = ct.bitmapDims;
        tile.rasterCanvasSize = outputCanvasSize;
        tile.canvasOffsetDoc = outputPointToPresentedDoc(ct.canvasOffsetPx);
        tile.bitmapDimsDoc = outputVectorToDoc(
            Vector2d(static_cast<double>(ct.bitmapDims.x), static_cast<double>(ct.bitmapDims.y)));
        if (ct.layerEntity != entt::null) {
          tile.documentFromCachedDocument = documentFromCachedDocument(ct.canvasFromBitmap);
          tile.dragTranslationDoc = tile.documentFromCachedDocument.translation();
        }
        tile.isDragTarget = ct.isDragTarget;
        if (!metadataOnly) {
          tile.bitmap = std::move(ct.bitmap);
          tile.textureSnapshot = std::move(ct.textureSnapshot);
        }
        previewTiles.push_back(std::move(tile));
      }
      if (previewTiles.empty()) {
        return std::nullopt;
      }
      return RenderResult::CompositedPreview{
          .tiles = std::move(previewTiles),
          .entity = previewEntity,
          .interactionKind = previewInteractionKind,
          .representedDragPreview = request.dragPreview,
      };
    };

    bool renderCompleted = true;
    const std::chrono::milliseconds replayDelay(
        replayRenderDelayMsForTesting_.load(std::memory_order_acquire));
    if (replayDelay.count() > 0) {
      const auto delayDeadline = std::chrono::steady_clock::now() + replayDelay;
      while (!cancelRender_.isCancelled()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= delayDeadline) {
          break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(delayDeadline - now);
        std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(1)));
      }
      renderCompleted = !cancelRender_.isCancelled();
    }
    if (renderCompleted) {
      ZoneScopedN("Compositor::renderFrame");
      // The final worker timing below intentionally covers the whole
      // presentation-gating iteration, including any readback or tile
      // snapshot work after renderFrame. Keep this scoped timing in
      // Tracy only for drilling into the compositor itself.
      const auto renderFrameStart = std::chrono::steady_clock::now();
      {
        const ScopedHeapDelta renderFrameHeapDelta(MemoryStage::WorkerRenderFrame);
        if (compositor_ != nullptr) {
          const auto syncWorkerPromotions = [&]() {
            std::erase_if(compositorEntities_,
                          [this](Entity entity) { return !compositor_->isPromoted(entity); });
            compositorEntity_ =
                compositorEntities_.empty() ? entt::null : compositorEntities_.front();
            if (compositorEntity_ == entt::null) {
              compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
            }
          };
          renderCompleted = compositor_->renderFrame(viewport, cancelRender_, surfaceFromCanvas);
          if (renderCompleted) {
            syncWorkerPromotions();
            bool promotionAddedAfterPrepare = false;
            for (Entity entity : desiredEntities) {
              if (compositor_->isPromoted(entity)) {
                continue;
              }
              const svg::compositor::CompositorController::PromoteResult promoteResult =
                  compositor_->promoteEntity(entity, desiredKind);
              if (promoteResult.promotedLayer()) {
                AppendUniqueEntity(&compositorEntities_, entity);
                promotionAddedAfterPrepare = true;
              }
            }
            if (promotionAddedAfterPrepare) {
              compositorEntity_ = compositorEntities_.front();
              compositorInteractionKind_ = desiredKind;
              renderCompleted =
                  compositor_->renderFrame(viewport, cancelRender_, surfaceFromCanvas);
              if (renderCompleted) {
                syncWorkerPromotions();
              }
            }
          }
        } else {
          // Composited rendering Off: flat full-document render with no
          // retained state, presented through the full-canvas snapshot path
          // below. This is the same direct path the compositor's
          // `verifyPixelIdentity` reference render uses, so pixels match the
          // composited modes by construction.
          svg::RendererDriver directDriver(requestRenderer);
          renderCompleted =
              directDriver.drawInterruptibly(requestDocument, viewport, surfaceFromCanvas,
                                             [this]() { return cancelRender_.isCancelled(); });
        }
      }
      workerTiming.renderFrameMs = elapsedSince(renderFrameStart);
    }
    if (renderCompleted && compositor_ != nullptr && !desiredEntities.empty()) {
      desiredPromotionCoverageCompleteAfterRender =
          compositor_->interactionLayersCover(compositorEntities_, desiredEntities);
    }

    // A superseding request can arrive after the compositor's final internal cancellation point.
    // Recheck at the presentation boundary so a stale pointer frame never enters a browser surface
    // handoff that cannot itself be cancelled.
    if (renderCompleted && cancelRender_.isCancelled()) {
      renderCompleted = false;
    }

    if (renderCompleted) {
      // SVG traversal is complete. Snapshot/readback, browser presentation, and diagnostic
      // packaging below use renderer/compositor-owned state only, so release the live DOM before
      // those potentially slow operations. UI input can then acquire the document without waiting
      // for a browser surface handoff to finish.
      releaseDocumentAccess();
    }

    // A cancelled render leaves compositor dirty flags ready for the next
    // pass. Do not publish a partial result; either loop into the superseding
    // request or park after a cancel-without-replacement.
    if (!renderCompleted) {
      // Release document access before taking `mutex_` to avoid a lock-order inversion.
      releaseDocumentAccess();
      // An abandoned iteration still observed whatever the backend device did.
      // A bounded GPU wait that burns its deadline declares the device lost
      // and then ends the frame with nothing to present, so this is the only
      // place that failure can be recorded: the completed-frame stats below
      // are never reached.
      noteGpuWaitOutcome(requestRenderer.consumeReadbackStats());
      cancelledRenderCount_.fetch_add(1, std::memory_order_release);
      std::function<void()> wake;
      bool notifyStateChange = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::holds_alternative<CancellingState>(workerState_)) {
          workerState_ = IdleState{};
          wake = wakeCallback_;
          notifyStateChange = true;
        }
      }
      if (notifyStateChange) {
        cv_.notify_all();
      }
      if (wake) {
        wake();
      }
      continue;
    }

    // Every non-Off editor frame publishes the compositor's paint-order tile set. Promotion
    // refusal only disables the drag skip-compose optimization; the remaining mandatory layers
    // and static segments are still the correct presentation topology.
    {
      const auto buildPreviewStart = std::chrono::steady_clock::now();
      const ScopedHeapDelta buildPreviewHeapDelta(MemoryStage::WorkerBuildPreview);
      compositedPreview = buildCompositedPreview();
      workerTiming.buildPreviewMs = elapsedSince(buildPreviewStart);
    }
    // Selection chrome is no longer baked into the bitmap - main.cc
    // draws it via the ImGui draw list every frame so clicks don't
    // pay the SVG re-rasterize cost. The `request.selection` field
    // is left in place for back-compat callers but ignored here.
    (void)request.selection;
    svg::RendererBitmap bitmap;
    std::shared_ptr<const svg::RendererTextureSnapshot> fullCanvasTexture;
    PresentationSnapshotPlan snapshotPlan;
    // Worker-surface builds present GPU-native frames and ignore
    // request.captureCpuSnapshot; browser diagnostics read pixels through the
    // async smoke-readback path instead.
    snapshotPlan = ChoosePresentationSnapshotPlan(
        compositedPreview.has_value(), requestRenderer.requiresTextureSnapshotPresentation(),
        request.captureCpuSnapshot);
    {
      const auto finalSnapshotStart = std::chrono::steady_clock::now();
      const ScopedHeapDelta finalSnapshotHeapDelta(MemoryStage::WorkerFinalSnapshot);
      // Read before exporting the texture because texture export detaches the renderer target.
      if (snapshotPlan.captureCpuSnapshot) {
        ZoneScopedN("Renderer::takeSnapshot");
        bitmap = requestRenderer.takeSnapshot();
      }
      if (snapshotPlan.captureTextureSnapshot) {
        ZoneScopedN("Renderer::takeTextureSnapshot");
        fullCanvasTexture = requestRenderer.takeTextureSnapshot();
        UTILS_RELEASE_ASSERT_MSG(
            fullCanvasTexture != nullptr,
            "Geode full-canvas presentation did not produce a GPU texture. Refusing CPU "
            "readback/upload fallback in Geode presentation mode.");
      }
      workerTiming.finalSnapshotMs = elapsedSince(finalSnapshotStart);
    }
    const svg::RendererReadbackStats readbackStats = requestRenderer.consumeReadbackStats();
    workerTiming.readbackCount = readbackStats.count;
    workerTiming.readbackPollIterations = readbackStats.pollIterations;
    workerTiming.usedTimedWaitAny = readbackStats.usedTimedWaitAny;
    workerTiming.deviceLost = readbackStats.deviceLost;
    workerTiming.timedOutWaitSite = readbackStats.timedOutWaitSite;
    workerTiming.timedOutWaitMs = readbackStats.timedOutWaitMs;
    noteGpuWaitOutcome(readbackStats);
    // Only the explicit Off mode, overview infill, and the geometry-debug diagnostic use a flat
    // payload. Normal On and FilterOnly presentation must have produced compositor tiles above.
    if (!compositedPreview.has_value() && (!bitmap.empty() || fullCanvasTexture != nullptr)) {
      UTILS_RELEASE_ASSERT_MSG(
          compositor_ == nullptr || request.overviewInfillOnly || geometryDebugOverlay,
          "A non-Off editor render produced no compositor tiles. Refusing monolithic full-canvas "
          "presentation.");
      const Entity previewEntity = compositor_ != nullptr ? compositorEntity_ : entt::null;
      const svg::compositor::InteractionHint interactionKind =
          request.dragPreview.has_value() ? request.dragPreview->interactionKind
                                          : svg::compositor::InteractionHint::Selection;
      UTILS_RELEASE_ASSERT_MSG(
          fullCanvasPayloadGeneration_ != std::numeric_limits<std::uint64_t>::max(),
          "Full-canvas payload generation exhausted; refusing to alias a fresh raster payload.");
      compositedPreview = BuildFullCanvasCompositedPreview(
          documentViewBox, bitmap, std::move(fullCanvasTexture), ++fullCanvasPayloadGeneration_,
          previewEntity, interactionKind, rasterViewport, request.dragPreview);
      if (geometryDebugOverlay) {
        compositedPreview->tiles.front().kind = RenderResult::CompositedTile::Kind::Immediate;
        compositedPreview->tiles.front().id = "geometry-debug-flat";
      }
    }
    UTILS_RELEASE_ASSERT_MSG(
        compositor_ == nullptr || request.overviewInfillOnly || geometryDebugOverlay ||
            compositedPreview.has_value(),
        "A non-Off editor render produced no compositor tiles. Refusing monolithic full-canvas "
        "presentation.");

    // Attribute what this render iteration is holding, before the result leaves
    // the worker. The compositor caches are a level (they persist across
    // frames); the full-canvas snapshot is a flow (a fresh allocation every
    // frame that presentation consumes and drops), and the two grow linear
    // memory in different ways, so they are published as different counter
    // kinds. See `donner/base/MemoryAttribution.h`.
    {
      // With composited rendering Off there is no controller; publish zeroed
      // compositor categories so the memory panel reflects the freed caches.
      const auto breakdown = compositor_ != nullptr
                                 ? compositor_->bitmapMemoryBreakdown()
                                 : svg::compositor::CompositorController::BitmapMemoryBreakdown{};
      SetRetainedBytes(MemoryCategory::CompositorSegmentBitmaps, breakdown.segmentBitmapBytes);
      SetRetainedBytes(MemoryCategory::CompositorSegmentTextures, breakdown.segmentTextureBytes);
      SetRetainedBytes(MemoryCategory::CompositorLayerBitmaps, breakdown.layerBitmapBytes);
      SetRetainedBytes(MemoryCategory::CompositorLayerTextures, breakdown.layerTextureBytes);
      SetEntryCount(MemoryCategory::CompositorSegmentBitmaps, breakdown.segmentCount);
      SetEntryCount(MemoryCategory::CompositorLayerBitmaps, breakdown.layerCount);

      std::uint64_t previewTileBytes = 0;
      std::uint64_t previewTileCount = 0;
      if (compositedPreview.has_value()) {
        for (const RenderResult::CompositedTile& tile : compositedPreview->tiles) {
          previewTileBytes += tile.bitmap.pixels.size();
          if (tile.textureSnapshot != nullptr) {
            const Vector2i dims = tile.textureSnapshot->dimensions();
            previewTileBytes +=
                static_cast<std::uint64_t>(dims.x) * static_cast<std::uint64_t>(dims.y) * 4u;
          }
          ++previewTileCount;
        }
      }
      SetRetainedBytes(MemoryCategory::RenderResultTiles, previewTileBytes);
      SetEntryCount(MemoryCategory::RenderResultTiles, previewTileCount);
      AddTransientBytes(MemoryCategory::RenderResultTiles, previewTileBytes);

      std::uint64_t snapshotBytes = bitmap.pixels.size();
      if (fullCanvasTexture != nullptr) {
        const Vector2i dims = fullCanvasTexture->dimensions();
        snapshotBytes +=
            static_cast<std::uint64_t>(dims.x) * static_cast<std::uint64_t>(dims.y) * 4u;
      }
      SetRetainedBytes(MemoryCategory::WorkerFrameSnapshot, snapshotBytes);
      AddTransientBytes(MemoryCategory::WorkerFrameSnapshot, snapshotBytes);
    }

    // All document reads for this iteration are done; release write access before taking `mutex_`
    // to avoid a lock-order inversion against UI-thread DOM reads.
    releaseDocumentAccess();

    std::function<void()> wake;
    bool notifyStateChange = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Only transition to Done if we were not shut down, cancelled, or
      // superseded mid-render.
      if (auto* rendering = std::get_if<RenderingState>(&workerState_)) {
        if (rendering->pendingRequest.has_value()) {
        } else {
          DoneState done;
          done.result.bitmap = std::move(bitmap);
          done.result.compositedPreview = std::move(compositedPreview);
          done.result.rasterViewport = rasterViewport;
          done.result.viewport = request.viewport;
          done.result.overviewInfillOnly = request.overviewInfillOnly;
          done.result.version = request.version;
          done.result.documentGeneration = request.documentGeneration;
          done.presentationHoldPollsRemaining = replayResultHoldFramesForTesting_;
          lastFastPathCounters_ = compositor_ != nullptr
                                      ? compositor_->fastPathCountersForTesting()
                                      : svg::compositor::CompositorController::FastPathCounters{};
          lastCompositorRenderFrameStats_ =
              compositor_ != nullptr ? compositor_->lastRenderFrameStats()
                                     : svg::compositor::CompositorController::RenderFrameStats{};
          if (compositorDiagnosticsEnabled_.load(std::memory_order_acquire)) {
            const auto diagnosticsStart = std::chrono::steady_clock::now();
            if (compositor_ != nullptr) {
              const auto thumbnailMode =
                  activeDragRequest
                      ? svg::compositor::CompositorController::SnapshotThumbnails::Omit
                      : svg::compositor::CompositorController::SnapshotThumbnails::Include;
              lastLayerInspectorRows_ = compositor_->snapshotLayerInspectorRows(thumbnailMode);
              lastSegmentInspectorRows_ = compositor_->snapshotSegmentInspectorRows();
              lastCompositeTiles_ = compositor_->snapshotCompositeTiles(thumbnailMode);
              lastStateSnapshot_ = compositor_->snapshotState();
            } else {
              // Composited rendering Off: clear the inspector surfaces so the
              // debug panel shows the compositor as absent, not stale.
              lastLayerInspectorRows_.clear();
              lastSegmentInspectorRows_.clear();
              lastCompositeTiles_.clear();
              lastStateSnapshot_ = {};
            }
            workerTiming.diagnosticsMs = elapsedSince(diagnosticsStart);
          }
          lastWorkerCompositorEntity_ = compositorEntity_;
          lastDocumentCanvasSize_ = outputCanvasSize;
          const auto workerEnd = std::chrono::steady_clock::now();
          const double workerMs =
              std::chrono::duration<double, std::milli>(workerEnd - workerStart).count();
          done.result.workerMs = workerMs;
          done.result.workerTiming = workerTiming;
          done.result.workerCompletedAt = workerEnd;
          workerState_ = std::move(done);
          // Snapshot the callback under the lock so a concurrent
          // `setWakeCallback` swap can't tear the invocation. Fire it
          // outside the lock to keep the hook cheap and avoid any
          // chance of deadlock if the caller re-enters AsyncRenderer.
          wake = wakeCallback_;
          notifyStateChange = true;
        }
      } else if (std::holds_alternative<CancellingState>(workerState_)) {
        // `cancelInFlight` raced with the worker's final lap -
        // renderFrame finished naturally but the user-input event
        // wants the result dropped. Drop it and transition to
        // Idle so the worker's cv_.wait at the top of the loop
        // doesn't deadlock.
        workerState_ = IdleState{};
        wake = wakeCallback_;
        notifyStateChange = true;
      }
    }
    if (notifyStateChange) {
      cv_.notify_all();
    }
    if (wake) {
      wake();
    }
  }
}

}  // namespace donner::editor
