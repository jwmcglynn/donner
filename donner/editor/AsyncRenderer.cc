#include "donner/editor/AsyncRenderer.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <thread>
#include <utility>


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
// entry points - held out of the the single-canvas architecture deletion series so the phase 4
// retire-or-rebuild decision for WebKit is made against the real code rather
// than a changelog.
//
// This block is NOT a supported configuration and must not be revived as-is:
// it presents into a second DOM canvas, which the "no CSS in presentation"
// invariant forbids. Phase 4 either deletes it (see the series' optional
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

bool ContainsAllEntities(const std::vector<Entity>& haystack, const std::vector<Entity>& needles) {
  return std::ranges::all_of(needles,
                             [&](Entity entity) { return ContainsEntity(haystack, entity); });
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
         std::holds_alternative<CancellingState>(state) ||
         std::holds_alternative<DoneState>(state);
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
      // §M4: tell the in-flight render to bail. Set this while the mutex still
      // exposes the superseding state so the worker cannot start the
      // replacement request and then receive a stale cancel.
      cancelRender_.cancel();
    }
    if (sampleThumbnailActive_) {
      // Main-document presentation always preempts background preview work. The thumbnail
      // traversal polls its independent token between rendered entities.
      cancelSampleThumbnail_.cancel();
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
        !compositorWarmupActive_ &&
        (std::holds_alternative<IdleState>(workerState_) ||
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
    cancelSampleThumbnail_.cancel();
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

void AsyncRenderer::workerLoop() {
#if   defined(__EMSCRIPTEN__)
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
#if   defined(__EMSCRIPTEN__)
      if (sampleThumbnailRenderer == nullptr || sampleThumbnailRendererRoot != &workerRenderer) {
        sampleThumbnailRenderer = workerRenderer.createOffscreenInstance();
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

      SampleThumbnailRenderResult result;
      if (offscreenRenderer == nullptr) {
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
#if   defined(__EMSCRIPTEN__)
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

    // §M4: every iteration starts with a fresh (non-cancelled) token.
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
    const bool needsFreshCompositor = !compositor_ || compositorRenderer_ != &requestRenderer;
    if (needsFreshCompositor) {
      svg::compositor::CompositorConfig compositorConfig;
      // The editor retains compositor textures across frames, so even a geometrically cheap span
      // is less expensive to upload once than to rerasterize during every pointer update. Browser
      // WebGPU makes the difference especially pronounced, but the same policy removes the native
      // renderer's dominant steady-drag CPU cost as well.
      compositorConfig.immediateStaticSpans = false;
      compositorConfig.dynamicImmediateStaticSpans = false;
      // The first full-document draw is already correct. Publish it first, then warm retained
      // caches from the worker's independent low-priority lane after the result is accepted.
      compositorConfig.deferFirstFrameWarmup = true;
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
        !needsFreshCompositor &&
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
      compositor_->resetAllLayers();
      compositorResetCount_.fetch_add(1, std::memory_order_release);
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
    const std::vector<Entity> desiredEntities =
        geometryDebugOverlay ? std::vector<Entity>() : DesiredCompositorEntities(request);
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
        } else if (promoteResult.fullCanvasPreviewRequired()) {
          // Valid renderable content under a filter, clip-path, or mask is presented through the
          // full-canvas composited tile built from the final snapshot below.
        }
      }
      if (compositorEntities_.empty()) {
        compositorEntity_ = entt::null;
      }
    }
    if (request.dragPreview.has_value() && request.dragPreview->forceLayerRasterization) {
      for (Entity entity : DragPreviewEntities(*request.dragPreview)) {
        compositor_->markPromotedLayerDirty(entity);
      }
    }
    const bool desiredPromotionIncomplete =
        !desiredEntities.empty() && !ContainsAllEntities(compositorEntities_, desiredEntities);

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
    compositor_->setTightBoundedSegmentsEnabled(
        tightBoundedSegments_.load(std::memory_order_acquire));

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
    compositor_->setSkipMainComposeDuringSplit(activeDragRequest && splitPreviewSafe &&
                                               !request.captureCpuSnapshot);
    workerTiming.setupMs = elapsedSince(workerStart);

    // Build a CompositedPreview from the compositor's current tile state.
    // Tiles whose id/generation/dimensions were already published carry
    // metadata only; the GL cache keeps the existing texture and applies
    // updated presentation geometry.
    const auto buildCompositedPreview = [&]() -> std::optional<RenderResult::CompositedPreview> {
      if (request.overviewInfillOnly) {
        return std::nullopt;
      }
      if (!splitPreviewSafe || !request.dragPreview.has_value() ||
          compositorEntity_ == entt::null || compositor_->layerCount() == 0u) {
        return std::nullopt;
      }
      const std::vector<Entity> dragPreviewEntities = DragPreviewEntities(*request.dragPreview);
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
          .entity = compositorEntity_,
          .interactionKind = request.dragPreview->interactionKind,
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
        renderCompleted = compositor_->renderFrame(viewport, cancelRender_, surfaceFromCanvas);
      }
      workerTiming.renderFrameMs = elapsedSince(renderFrameStart);
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

    // §M4: a cancelled render leaves compositor dirty flags ready for the next
    // pass. Do not publish a partial result; either loop into the superseding
    // request or park after a cancel-without-replacement.
    if (!renderCompleted) {
      // Release document access before taking `mutex_` to avoid a lock-order inversion.
      releaseDocumentAccess();
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

    // Build a CompositedPreview from the compositor tile set when available.
    // If the splitter cannot provide tiles for this frame, the final snapshot
    // below is wrapped as a single full-canvas tile so presentation still goes
    // through the compositor path.
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
    if (!compositedPreview.has_value() && (!bitmap.empty() || fullCanvasTexture != nullptr)) {
      const Entity previewEntity =
          request.dragPreview.has_value() ? request.dragPreview->entity : request.selectedEntity;
      const svg::compositor::InteractionHint interactionKind =
          request.dragPreview.has_value() ? request.dragPreview->interactionKind
                                          : svg::compositor::InteractionHint::Selection;
      compositedPreview = BuildFullCanvasCompositedPreview(
          documentViewBox, bitmap, std::move(fullCanvasTexture), request.version, previewEntity,
          interactionKind, rasterViewport, request.dragPreview);
    }

    // Attribute what this render iteration is holding, before the result leaves
    // the worker. The compositor caches are a level (they persist across
    // frames); the full-canvas snapshot is a flow (a fresh allocation every
    // frame that presentation consumes and drops), and the two grow linear
    // memory in different ways, so they are published as different counter
    // kinds. See `donner/base/MemoryAttribution.h`.
    {
      const auto breakdown = compositor_->bitmapMemoryBreakdown();
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
          lastFastPathCounters_ = compositor_->fastPathCountersForTesting();
          lastCompositorRenderFrameStats_ = compositor_->lastRenderFrameStats();
          if (compositorDiagnosticsEnabled_.load(std::memory_order_acquire)) {
            const auto diagnosticsStart = std::chrono::steady_clock::now();
            const auto thumbnailMode =
                activeDragRequest
                    ? svg::compositor::CompositorController::SnapshotThumbnails::Omit
                    : svg::compositor::CompositorController::SnapshotThumbnails::Include;
            lastLayerInspectorRows_ = compositor_->snapshotLayerInspectorRows(thumbnailMode);
            lastSegmentInspectorRows_ = compositor_->snapshotSegmentInspectorRows();
            lastCompositeTiles_ = compositor_->snapshotCompositeTiles(thumbnailMode);
            lastStateSnapshot_ = compositor_->snapshotState();
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
