#include "donner/editor/AsyncRenderer.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <thread>
#include <utility>

#include "donner/base/Utils.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

namespace {

RenderResult::CompositedPreview BuildFullCanvasCompositedPreview(
    svg::SVGDocument& document, const svg::RendererBitmap& bitmap,
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
  const std::optional<Box2d> viewBox = document.svgElement().viewBox();
  const Box2d documentViewBox =
      viewBox.value_or(Box2d::FromXYWH(0, 0, static_cast<double>(rasterViewport.outputSizePx.x),
                                       static_cast<double>(rasterViewport.outputSizePx.y)));
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

}  // namespace

PresentationSnapshotPlan ChoosePresentationSnapshotPlan(bool hasCompositedPreview,
                                                        bool requiresTextureSnapshotPresentation) {
  if (requiresTextureSnapshotPresentation) {
    return PresentationSnapshotPlan{
        .captureTextureSnapshot = !hasCompositedPreview,
    };
  }

  return PresentationSnapshotPlan{
      .captureCpuSnapshot = true,
  };
}

AsyncRenderer::AsyncRenderer() {
  thread_ = std::thread([this] { workerLoop(); });
}

AsyncRenderer::~AsyncRenderer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    workerState_ = ShutdownState{};
  }
  cv_.notify_all();
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
         std::holds_alternative<CancellingState>(state) || std::holds_alternative<DoneState>(state);
}

bool AsyncRenderer::workerStateRenderInFlight(const WorkerState& state) {
  return std::holds_alternative<RenderingState>(state) ||
         std::holds_alternative<CancellingState>(state);
}

bool AsyncRenderer::isBusy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workerStateBusy(workerState_);
}

bool AsyncRenderer::hasRenderInFlightForTesting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workerStateRenderInFlight(workerState_);
}

bool AsyncRenderer::waitUntilNoRenderInFlightForTesting(
    std::chrono::steady_clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_until(lock, deadline,
                        [this] { return !workerStateRenderInFlight(workerState_); });
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
    RenderRequest stagedRequest = request;
    if (!request.structuralRemap.empty()) {
      retainedStructuralRemaps_[request.documentGeneration] = request.structuralRemap;
    } else {
      const auto retainedIt = retainedStructuralRemaps_.find(request.documentGeneration);
      if (retainedIt != retainedStructuralRemaps_.end()) {
        stagedRequest.structuralRemap = retainedIt->second;
      }
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
  }
  cv_.notify_one();
}

void AsyncRenderer::cancelInFlight() {
  bool signalCancel = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
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
  }
  if (signalCancel) {
    // Notify in case the worker was still in `cv_.wait` when we
    // landed — its updated predicate also wakes on `Cancelling`.
    cv_.notify_one();
  }
}

std::optional<RenderResult> AsyncRenderer::pollResult() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto* done = std::get_if<DoneState>(&workerState_)) {
    if (done->replayHoldPollsRemaining > 0) {
      --done->replayHoldPollsRemaining;
      replayResultHoldPollCount_.fetch_add(1, std::memory_order_release);
      return std::nullopt;
    }

    RenderResult result = std::move(done->result);
    workerState_ = IdleState{};
    cv_.notify_all();
    return result;
  }
  return std::nullopt;
}

void AsyncRenderer::setWakeCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  wakeCallback_ = std::move(callback);
}

void AsyncRenderer::workerLoop() {
#ifdef __EMSCRIPTEN__
  // Emscripten's WebGPU object table is per-worker. Construct and use the
  // renderer on this pthread so wgpu handles never cross JS worker boundaries.
  svg::Renderer workerRenderer;
#endif

  while (true) {
    std::optional<RenderRequest> requestStorage;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] {
        return std::holds_alternative<RenderingState>(workerState_) ||
               std::holds_alternative<CancellingState>(workerState_) ||
               std::holds_alternative<ShutdownState>(workerState_);
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
      auto* rendering = std::get_if<RenderingState>(&workerState_);
      assert(rendering != nullptr);
      assert(rendering->pendingRequest.has_value() &&
             "Rendering worker state requires a pending request while waiting");
      requestStorage.emplace(std::move(*rendering->pendingRequest));
      rendering->pendingRequest.reset();
    }
    RenderRequest& request = *requestStorage;
#ifdef __EMSCRIPTEN__
    svg::Renderer& requestRenderer = workerRenderer;
#else
    // Geode editor builds intentionally use the request renderer so worker texture snapshots are
    // created on the same WGPU device as ImGui presentation.
    svg::Renderer& requestRenderer = request.lease.renderer();
#endif
    svg::SVGDocument& requestDocument = request.lease.document();

    // §M4: every iteration starts with a fresh (non-cancelled) token.
    // The UI thread sets cancel via `requestRender` ONLY when posting
    // while busy, and we're idle here right before the render runs —
    // so any cancel signal from a previous iteration is stale.
    cancelRender_.reset();

    // Execute the render outside the lock so the UI thread can poll
    // `isBusy()` / `pollResult()` while we work.
    ZoneScopedN("AsyncRenderer::workerIteration");
    const auto workerStart = std::chrono::steady_clock::now();
    const auto elapsedSince = [](std::chrono::steady_clock::time_point start) {
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
          .count();
    };
    RenderResult::WorkerTimingBreakdown workerTiming;
    std::optional<RenderResult::CompositedPreview> compositedPreview;

    // §concurrent-dom: serialize this worker render against UI-thread DOM reads. The lease shares
    // the live registry (it does not snapshot), and the worker cannot touch the document in
    // SingleThreaded mode (owner-thread assert). The document is flipped to ConcurrentDom on first
    // render and stays there for the editor's lifetime — UI-thread reads are responsible for
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
    //      try the structural-remap path FIRST — it preserves cached
    //      filter / bucket bitmaps, `canvasFromBitmap` stamps, and
    //      the pre-warmed bg/fg pair — and only fall back to a
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
      compositor_ =
          std::make_unique<svg::compositor::CompositorController>(requestDocument, requestRenderer);
      compositorDocument_ = requestDocument;  // cheap: refcount bump on the Registry handle.
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
            // The drag/selection target didn't survive the remap — fall
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
      compositorDocument_ = requestDocument;
      compositorDocumentGeneration_ = request.documentGeneration;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        retainedStructuralRemaps_.erase(request.documentGeneration);
      }
    }

    // Resolve what the compositor should be promoted on this render.
    // Priority: explicit drag targets win over the persistent selection hint;
    // otherwise we keep the selected entity promoted so the next drag
    // arrives with everything pre-warmed. Multi-select drags intentionally
    // promote every selected participant: the presenter applies one shared
    // document-space transform to every drag-target tile, keeping the path
    // overlay and cached content in lockstep while avoiding a full DOM render
    // on each pointer frame.
    const std::vector<Entity> desiredEntities = DesiredCompositorEntities(request);
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
    // post-zoom — sustained > 1 s/frame on the splash.
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
    const bool desiredPromotionIncomplete =
        !desiredEntities.empty() && !ContainsAllEntities(compositorEntities_, desiredEntities);

    // The DOM is the sole source of truth for the dragged entity's
    // position — `SelectTool` mutates the `transform` attribute every
    // drag frame, so by the time we reach here the compositor's fast
    // path will diff the new DOM transform against the cached bitmap's
    // rasterize-time transform and either reuse the bitmap via a
    // pure-translation compose offset or mark it dirty for re-rasterize.
    // No emulation layer on top of the DOM.
    svg::RenderViewport viewport;
    const Vector2i semanticCanvasSize = requestDocument.canvasSize();
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
    compositor_->setSkipMainComposeDuringSplit(activeDragRequest && splitPreviewSafe);
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
      const Box2d viewBox = requestDocument.svgElement().viewBox().value_or(
          Box2d::FromXYWH(0, 0, static_cast<double>(semanticCanvasSize.x),
                          static_cast<double>(semanticCanvasSize.y)));
      const Transform2d documentFromOutput = rasterViewport.outputFromDocument.inverse();
      const auto outputPointToPresentedDoc = [&](const Vector2d& outputPoint) {
        return documentFromOutput.transformPosition(outputPoint) - viewBox.topLeft;
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
      renderCompleted = compositor_->renderFrame(viewport, cancelRender_, surfaceFromCanvas);
      workerTiming.renderFrameMs = elapsedSince(renderFrameStart);
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
      compositedPreview = buildCompositedPreview();
      workerTiming.buildPreviewMs = elapsedSince(buildPreviewStart);
    }

    // Selection chrome is no longer baked into the bitmap — main.cc
    // draws it via the ImGui draw list every frame so clicks don't
    // pay the SVG re-rasterize cost. The `request.selection` field
    // is left in place for back-compat callers but ignored here.
    (void)request.selection;
    svg::RendererBitmap bitmap;
    std::shared_ptr<const svg::RendererTextureSnapshot> fullCanvasTexture;
    const PresentationSnapshotPlan snapshotPlan = ChoosePresentationSnapshotPlan(
        compositedPreview.has_value(), requestRenderer.requiresTextureSnapshotPresentation());
    {
      const auto finalSnapshotStart = std::chrono::steady_clock::now();
      if (snapshotPlan.captureTextureSnapshot) {
        ZoneScopedN("Renderer::takeTextureSnapshot");
        fullCanvasTexture = requestRenderer.takeTextureSnapshot();
        UTILS_RELEASE_ASSERT_MSG(
            fullCanvasTexture != nullptr,
            "Geode full-canvas presentation did not produce a GPU texture. Refusing CPU "
            "readback/upload fallback in Geode presentation mode.");
      }
      if (snapshotPlan.captureCpuSnapshot) {
        ZoneScopedN("Renderer::takeSnapshot");
        bitmap = requestRenderer.takeSnapshot();
      }
      workerTiming.finalSnapshotMs = elapsedSince(finalSnapshotStart);
    }
    if (!compositedPreview.has_value() && (!bitmap.empty() || fullCanvasTexture != nullptr)) {
      const Entity previewEntity =
          request.dragPreview.has_value() ? request.dragPreview->entity : request.selectedEntity;
      const svg::compositor::InteractionHint interactionKind =
          request.dragPreview.has_value() ? request.dragPreview->interactionKind
                                          : svg::compositor::InteractionHint::Selection;
      compositedPreview = BuildFullCanvasCompositedPreview(
          requestDocument, bitmap, std::move(fullCanvasTexture), request.version, previewEntity,
          interactionKind, rasterViewport, request.dragPreview);
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
          continue;
        }

        if (!request.overviewInfillOnly) {
          notePublishedCompositedPreview(compositedPreview);
        }

        DoneState done;
        done.result.bitmap = std::move(bitmap);
        done.result.compositedPreview = std::move(compositedPreview);
        done.result.rasterViewport = rasterViewport;
        done.result.overviewInfillOnly = request.overviewInfillOnly;
        done.result.version = request.version;
        done.replayHoldPollsRemaining = replayResultHoldFramesForTesting_;
        const auto diagnosticsStart = std::chrono::steady_clock::now();
        const auto thumbnailMode =
            activeDragRequest ? svg::compositor::CompositorController::SnapshotThumbnails::Omit
                              : svg::compositor::CompositorController::SnapshotThumbnails::Include;
        lastFastPathCounters_ = compositor_->fastPathCountersForTesting();
        lastCompositorRenderFrameStats_ = compositor_->lastRenderFrameStats();
        lastLayerInspectorRows_ = compositor_->snapshotLayerInspectorRows(thumbnailMode);
        lastSegmentInspectorRows_ = compositor_->snapshotSegmentInspectorRows();
        lastCompositeTiles_ = compositor_->snapshotCompositeTiles(thumbnailMode);
        lastStateSnapshot_ = compositor_->snapshotState();
        workerTiming.diagnosticsMs = elapsedSince(diagnosticsStart);
        lastWorkerCompositorEntity_ = compositorEntity_;
        lastDocumentCanvasSize_ = outputCanvasSize;
        const auto workerEnd = std::chrono::steady_clock::now();
        const double workerMs =
            std::chrono::duration<double, std::milli>(workerEnd - workerStart).count();
        done.result.workerMs = workerMs;
        done.result.workerTiming = workerTiming;
        workerState_ = std::move(done);
        notifyStateChange = true;
        // Snapshot the callback under the lock so a concurrent
        // `setWakeCallback` swap can't tear the invocation. Fire it
        // outside the lock to keep the hook cheap and avoid any
        // chance of deadlock if the caller re-enters AsyncRenderer.
        wake = wakeCallback_;
      } else if (std::holds_alternative<CancellingState>(workerState_)) {
        // `cancelInFlight` raced with the worker's final lap —
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
