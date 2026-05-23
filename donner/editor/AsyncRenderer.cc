#include "donner/editor/AsyncRenderer.h"

#include <cassert>
#include <chrono>
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
    std::optional<RenderRequest::DragPreview> representedDragPreview) {
  RenderResult::CompositedTile tile;
  tile.kind = RenderResult::CompositedTile::Kind::Segment;
  tile.id = "full-canvas";
  tile.generation = generation;
  tile.bitmap = bitmap;
  tile.textureSnapshot = std::move(textureSnapshot);
  tile.canvasOffsetDoc = Vector2d::Zero();
  const Vector2i payloadDims =
      !bitmap.empty() ? bitmap.dimensions
                      : (tile.textureSnapshot != nullptr ? tile.textureSnapshot->dimensions()
                                                         : Vector2i::Zero());
  tile.bitmapDimsPx = payloadDims;
  tile.rasterCanvasSize = payloadDims;
  if (const std::optional<Box2d> viewBox = document.svgElement().viewBox();
      viewBox.has_value() && viewBox->size().x > 0.0 && viewBox->size().y > 0.0) {
    tile.bitmapDimsDoc = viewBox->size();
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

bool AsyncRenderer::isBusy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workerStateBusy(workerState_);
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
    RenderResult result = std::move(done->result);
    workerState_ = IdleState{};
    return result;
  }
  return std::nullopt;
}

void AsyncRenderer::setWakeCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  wakeCallback_ = std::move(callback);
}

void AsyncRenderer::workerLoop() {
  // Construct the Renderer on the worker thread so all its backend-owned
  // resources (WebGPU device/queue/textures under Geode, etc.) are
  // created and used from a single thread. Under
  // Emscripten pthreads this is load-bearing: `WebGPU.Internals.jsObjects`
  // is per-worker, and crossing thread boundaries triggers a
  // `getJsObject` assert on the first `wgpuDeviceCreateTexture`.
  svg::Renderer renderer;

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
        // `renderer` is destroyed here as the local unwinds, i.e. on
        // the worker thread — mirroring its construction.
        return;
      }
      if (std::holds_alternative<CancellingState>(workerState_)) {
        // `cancelInFlight` raced with the worker before it could
        // start renderFrame. Transition to Idle and loop back to cv_.wait.
        std::function<void()> wake = wakeCallback_;
        workerState_ = IdleState{};
        lock.unlock();
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
    svg::Renderer& requestRenderer = request.lease.renderer();
    svg::SVGDocument& requestDocument = request.lease.document();

    // §M4: every iteration starts with a fresh (non-cancelled) token.
    // The UI thread sets cancel via `requestRender` ONLY when posting
    // while busy, and we're idle here right before the render runs —
    // so any cancel signal from a previous iteration is stale.
    cancelRender_.reset();

    // Execute the render outside the lock so the UI thread can poll
    // `isBusy()` / `pollResult()` while we work.
    ZoneScopedN("AsyncRenderer::workerIteration");
    std::optional<RenderResult::CompositedPreview> compositedPreview;

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
    // Priority: an explicit drag wins over the persistent selection hint;
    // otherwise we keep the selected entity promoted so the next drag
    // arrives with everything pre-warmed.
    const Entity desiredEntity =
        request.dragPreview.has_value() ? request.dragPreview->entity : request.selectedEntity;
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
    const bool entityChanged = compositorEntity_ != desiredEntity;
    // Keep a selected entity in ActiveDrag mode after mouse-up so the
    // layer/segment caches stay hot for release-to-drag cycles. The
    // interaction kind changes back to Selection only when a different
    // entity is promoted.
    const bool kindUpgrade =
        desiredEntity != entt::null &&
        compositorInteractionKind_ == svg::compositor::InteractionHint::Selection &&
        desiredKind == svg::compositor::InteractionHint::ActiveDrag;
    if (entityChanged || kindUpgrade) {
      if (entityChanged && compositorEntity_ != entt::null) {
        compositor_->demoteEntity(compositorEntity_);
      }
      if (entityChanged) {
        compositorEntity_ = entt::null;
        compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
      }
      if (desiredEntity != entt::null) {
        const svg::compositor::CompositorController::PromoteResult promoteResult =
            compositor_->promoteEntity(desiredEntity, desiredKind);
        if (promoteResult.promotedLayer()) {
          compositorEntity_ = desiredEntity;
          compositorInteractionKind_ = desiredKind;
        } else if (promoteResult.fullCanvasPreviewRequired()) {
          // Valid renderable content under a filter, clip-path, or mask is presented through the
          // full-canvas composited tile built from the final snapshot below.
        }
      }
    }

    // The DOM is the sole source of truth for the dragged entity's
    // position — `SelectTool` mutates the `transform` attribute every
    // drag frame, so by the time we reach here the compositor's fast
    // path will diff the new DOM transform against the cached bitmap's
    // rasterize-time transform and either reuse the bitmap via a
    // pure-translation compose offset or mark it dirty for re-rasterize.
    // No emulation layer on top of the DOM.
    svg::RenderViewport viewport;
    const Vector2i canvasSize = requestDocument.canvasSize();
    viewport.size = Vector2d(canvasSize.x, canvasSize.y);
    viewport.devicePixelRatio = 1.0;
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
    compositor_->setSkipMainComposeDuringSplit(activeDragRequest);

    // Build a CompositedPreview from the compositor's current tile state.
    // Tiles whose id/generation/dimensions were already published carry
    // metadata only; the GL cache keeps the existing texture and applies
    // updated presentation geometry.
    const auto buildCompositedPreview = [&]() -> std::optional<RenderResult::CompositedPreview> {
      if (!request.dragPreview.has_value() || compositorEntity_ == entt::null ||
          compositor_->layerCount() == 0u) {
        return std::nullopt;
      }
      const Box2d viewBox = requestDocument.svgElement().viewBox().value_or(Box2d::FromXYWH(
          0, 0, static_cast<double>(canvasSize.x), static_cast<double>(canvasSize.y)));
      const double viewBoxWidth = viewBox.size().x;
      const double viewBoxHeight = viewBox.size().y;
      const double scaleX =
          viewBoxWidth > 0.0 ? static_cast<double>(canvasSize.x) / viewBoxWidth : 1.0;
      const double scaleY =
          viewBoxHeight > 0.0 ? static_cast<double>(canvasSize.y) / viewBoxHeight : 1.0;
      const auto canvasToDoc = [&](const Vector2d& canvas) {
        return Vector2d(scaleX != 0.0 ? canvas.x / scaleX : 0.0,
                        scaleY != 0.0 ? canvas.y / scaleY : 0.0);
      };
      const Transform2d canvasFromDocument = requestDocument.canvasFromDocumentTransform();
      const Transform2d documentFromCanvas = canvasFromDocument.inverse();
      const auto documentFromCachedDocument = [&](const Transform2d& canvasFromCachedCanvas) {
        return canvasFromDocument * canvasFromCachedCanvas * documentFromCanvas;
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

      using svg::compositor::CompositorTileBitmapPayload;
      auto compositorTiles =
          compositor_->snapshotTilesForUpload(CompositorTileBitmapPayload::MetadataOnly);
      bool canReuseNonDragTextures = !publishedCompositedTiles_.empty();
      bool activeDragTileWillHaveBitmap = !activeDragRequest;
      for (const auto& ct : compositorTiles) {
        if (ct.bitmapDims.x <= 0 || ct.bitmapDims.y <= 0) {
          continue;
        }
        using OutKind = RenderResult::CompositedTile::Kind;
        const OutKind kind = ct.layerEntity == entt::null ? OutKind::Segment : OutKind::Layer;
        const bool currentActiveDragLayer = activeDragRequest && request.dragPreview.has_value() &&
                                            ct.layerEntity == request.dragPreview->entity;
        if (currentActiveDragLayer) {
          activeDragTileWillHaveBitmap =
              ct.isDragTarget || publishedTextureMatches(std::to_string(ct.tileId), kind,
                                                         ct.generation, ct.bitmapDims, canvasSize);
          if (!activeDragTileWillHaveBitmap) {
            canReuseNonDragTextures = false;
            break;
          }
          continue;
        }
        if (ct.isDragTarget && activeDragRequest) continue;
        if (!publishedTextureMatches(std::to_string(ct.tileId), kind, ct.generation, ct.bitmapDims,
                                     canvasSize)) {
          canReuseNonDragTextures = false;
          break;
        }
      }
      if (!activeDragTileWillHaveBitmap) {
        canReuseNonDragTextures = false;
      }
      const CompositorTileBitmapPayload payload =
          canReuseNonDragTextures ? (activeDragRequest ? CompositorTileBitmapPayload::DragTargetOnly
                                                       : CompositorTileBitmapPayload::MetadataOnly)
                                  : CompositorTileBitmapPayload::All;
      if (payload != CompositorTileBitmapPayload::MetadataOnly) {
        compositorTiles = compositor_->snapshotTilesForUpload(payload);
      }
      std::vector<RenderResult::CompositedTile> previewTiles;
      previewTiles.reserve(compositorTiles.size());
      for (auto& ct : compositorTiles) {
        if (ct.bitmapDims.x <= 0 || ct.bitmapDims.y <= 0) continue;
        using OutKind = RenderResult::CompositedTile::Kind;
        const std::string tileId = std::to_string(ct.tileId);
        const OutKind kind = ct.layerEntity == entt::null ? OutKind::Segment : OutKind::Layer;
        const bool metadataOnly =
            (!ct.isDragTarget || !activeDragRequest) &&
            publishedTextureMatches(tileId, kind, ct.generation, ct.bitmapDims, canvasSize);
        if (!metadataOnly && ct.bitmap.empty() && ct.textureSnapshot == nullptr) continue;
        RenderResult::CompositedTile tile;
        tile.kind = kind;
        tile.id = tileId;
        tile.generation = ct.generation;
        tile.bitmapDimsPx = ct.bitmapDims;
        tile.rasterCanvasSize = canvasSize;
        tile.canvasOffsetDoc = canvasToDoc(ct.canvasOffsetPx);
        tile.bitmapDimsDoc = canvasToDoc(
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

    double workerMs = 0.0;
    bool renderCompleted = true;
    const auto renderStart = std::chrono::steady_clock::now();
    {
      ZoneScopedN("Compositor::renderFrame");
      // Wall-clock the core backend render so the editor can plot it
      // on the frame graph next to the ImGui frame time. This measures
      // the actual work the compositor performs per request, not the
      // total worker iteration (which also pays for takeSnapshot and
      // the result handoff). Scoped here so Tracy's zone cost isn't
      // included either.
      renderCompleted = compositor_->renderFrame(viewport, cancelRender_);
      const auto renderEnd = std::chrono::steady_clock::now();
      workerMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
    }

    // §M4: a cancelled render leaves compositor dirty flags ready for the next
    // pass. Do not publish a partial result; either loop into the superseding
    // request or park after a cancel-without-replacement.
    if (!renderCompleted) {
      cancelledRenderCount_.fetch_add(1, std::memory_order_release);
      std::function<void()> wake;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::holds_alternative<CancellingState>(workerState_)) {
          workerState_ = IdleState{};
          wake = wakeCallback_;
        }
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
    compositedPreview = buildCompositedPreview();

    // Selection chrome is no longer baked into the bitmap — main.cc
    // draws it via the ImGui draw list every frame so clicks don't
    // pay the SVG re-rasterize cost. The `request.selection` field
    // is left in place for back-compat callers but ignored here.
    (void)request.selection;
    svg::RendererBitmap bitmap;
    std::shared_ptr<const svg::RendererTextureSnapshot> fullCanvasTexture;
    if (requestRenderer.requiresTextureSnapshotPresentation()) {
      if (!compositedPreview.has_value()) {
        ZoneScopedN("Renderer::takeTextureSnapshot");
        fullCanvasTexture = requestRenderer.takeTextureSnapshot();
        UTILS_RELEASE_ASSERT_MSG(
            fullCanvasTexture != nullptr,
            "Geode full-canvas presentation did not produce a GPU texture. Refusing CPU "
            "readback/upload fallback in Geode presentation mode.");
      }
    } else {
      ZoneScopedN("Renderer::takeSnapshot");
      bitmap = requestRenderer.takeSnapshot();
    }
    if (!compositedPreview.has_value() && (!bitmap.empty() || fullCanvasTexture != nullptr)) {
      const Entity previewEntity =
          request.dragPreview.has_value() ? request.dragPreview->entity : request.selectedEntity;
      const svg::compositor::InteractionHint interactionKind =
          request.dragPreview.has_value() ? request.dragPreview->interactionKind
                                          : svg::compositor::InteractionHint::Selection;
      compositedPreview = BuildFullCanvasCompositedPreview(
          requestDocument, bitmap, std::move(fullCanvasTexture), request.version, previewEntity,
          interactionKind, request.dragPreview);
    }

    std::function<void()> wake;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Only transition to Done if we were not shut down, cancelled, or
      // superseded mid-render.
      if (auto* rendering = std::get_if<RenderingState>(&workerState_)) {
        if (rendering->pendingRequest.has_value()) {
          continue;
        }

        notePublishedCompositedPreview(compositedPreview);

        DoneState done;
        done.result.bitmap = std::move(bitmap);
        done.result.compositedPreview = std::move(compositedPreview);
        done.result.version = request.version;
        done.result.workerMs = workerMs;
        lastFastPathCounters_ = compositor_->fastPathCountersForTesting();
        lastLayerInspectorRows_ = compositor_->snapshotLayerInspectorRows();
        lastSegmentInspectorRows_ = compositor_->snapshotSegmentInspectorRows();
        lastCompositeTiles_ = compositor_->snapshotCompositeTiles();
        lastStateSnapshot_ = compositor_->snapshotState();
        lastWorkerCompositorEntity_ = compositorEntity_;
        lastDocumentCanvasSize_ = canvasSize;
        workerState_ = std::move(done);
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
      }
    }
    if (wake) {
      wake();
    }
  }
}

}  // namespace donner::editor
