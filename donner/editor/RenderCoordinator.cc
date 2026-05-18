#include "donner/editor/RenderCoordinator.h"

#include <chrono>
#include <cstdlib>

#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TracyWrapper.h"

namespace donner::editor {

namespace {

/// During continuous pinch-zoom, the viewport's `desiredCanvasSize` changes
/// every wheel event (~60 Hz). Each commit through
/// `SVGDocument::setCanvasSize` calls `invalidateRenderTree`, which forces
/// the compositor to re-rasterize every promoted layer at the new canvas
/// size — at high zoom on the splash that's 7–8 layers × seconds each, so
/// the editor freezes for the whole gesture. Debouncing the commit lets
/// the previously-rendered bitmap (at the prior canvas size) keep displaying
/// stretched until the user stops zooming; the high-quality re-rasterize
/// then happens once.
constexpr std::chrono::milliseconds kCanvasSizeCommitDelay{120};

}  // namespace

void RenderCoordinator::resetForLoadedDocument() {
  experimentalDragPresentation_ = ExperimentalDragPresentation{};
  selectionBoundsCache_ = SelectionBoundsCache{};
  pendingOverlayBitmap_.reset();
  pendingOverlayVersion_ = 0;
  displayedDocVersion_ = 0;
  lastOverlaySelectionVec_.clear();
  lastOverlayCanvasSize_ = Vector2i::Zero();
  lastOverlayVersion_ = std::numeric_limits<std::uint64_t>::max();
  lastOverlayMarqueeRectDoc_.reset();
  lastRenderedVersion_ = 0;
  lastRenderedCanvasSize_ = Vector2i::Zero();
}

void RenderCoordinator::refreshSelectionBoundsCache(EditorApp& app) {
  if (!app.hasDocument()) {
    selectionBoundsCache_ = SelectionBoundsCache{};
    return;
  }

  RefreshSelectionBoundsCache(selectionBoundsCache_,
                              std::span<const svg::SVGElement>(app.selectedElements()),
                              app.document().currentFrameVersion(), displayedDocVersion_);
}

void RenderCoordinator::promoteSelectionBoundsIfReady() {
  PromoteSelectionBoundsIfReady(selectionBoundsCache_, displayedDocVersion_);
}

bool RenderCoordinator::rasterizeOverlayForCurrentSelection(
    EditorApp& app, const ViewportState& viewport, GlTextureCache& textures,
    const std::optional<Box2d>& marqueeRectDoc) {
  ZoneScopedN("RenderCoordinator::rasterizeOverlay");
  if (!app.hasDocument()) {
    return false;
  }

  const Vector2i currentCanvasSize = app.document().document().canvasSize();
  const auto currentVersion = app.document().currentFrameVersion();

  svg::RenderViewport overlayViewport;
  overlayViewport.size =
      Vector2d(static_cast<double>(currentCanvasSize.x) / viewport.devicePixelRatio,
               static_cast<double>(currentCanvasSize.y) / viewport.devicePixelRatio);
  overlayViewport.devicePixelRatio = viewport.devicePixelRatio;
  overlayRenderer_.beginFrame(overlayViewport);
  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  const auto& overlaySelection = app.selectedElements();
  // Bounds are now computed inline by `OverlayRenderer::drawChromeWithTransform`
  // from the live `overlaySelection`, so the AABB and the path outline share a
  // single DOM snapshot. The `selectionBoundsCache_` is still maintained for
  // main-loop selection-change detection elsewhere, but no longer gates the
  // overlay's AABBs — that was the source of the drag-time shear where the
  // cached bounds lagged the live path outline by a frame or two.
  OverlayRenderer::drawChromeWithTransform(overlayRenderer_,
                                           std::span<const svg::SVGElement>(overlaySelection),
                                           marqueeRectDoc, canvasFromDoc);
  overlayRenderer_.endFrame();
  pendingOverlayBitmap_ = overlayRenderer_.takeSnapshot();
  pendingOverlayVersion_ = currentVersion;

  if (currentVersion == displayedDocVersion_ && pendingOverlayBitmap_.has_value() &&
      !pendingOverlayBitmap_->empty()) {
    textures.uploadOverlay(*pendingOverlayBitmap_);
    pendingOverlayBitmap_.reset();
    pendingOverlayVersion_ = 0;
  } else if (currentVersion == displayedDocVersion_) {
    textures.clearOverlay();
    pendingOverlayBitmap_.reset();
    pendingOverlayVersion_ = 0;
  }

  lastOverlaySelectionVec_ = overlaySelection;
  lastOverlayCanvasSize_ = currentCanvasSize;
  lastOverlayVersion_ = currentVersion;
  lastOverlayMarqueeRectDoc_ = marqueeRectDoc;
  return true;
}

void RenderCoordinator::pollRenderResult(EditorApp& app, const ViewportState& viewport,
                                         GlTextureCache& textures, FrameHistory* frameHistory) {
  ZoneScopedN("RenderCoordinator::pollRenderResult");
  auto resultOpt = asyncRenderer_.pollResult();
  if (!resultOpt.has_value()) {
    return;
  }

  const auto& result = *resultOpt;
  // Forward the worker-measured backend time to the frame history so
  // `RenderFrameGraph` can overlay backend render time on the UI frame
  // graph. The frame history's latest slot corresponds to the current
  // UI frame (pushed at the top of `EditorShell::runFrame` before
  // poll) — a landed result belongs to that frame.
  if (frameHistory != nullptr) {
    frameHistory->setLatestBackendMs(static_cast<float>(result.workerMs));
  }
  const bool hasComposited =
      result.compositedPreview.has_value() && result.compositedPreview->valid();
  if (hasComposited) {
    textures.uploadComposited(*result.compositedPreview);
    const bool allowIdleDisplay =
        result.compositedPreview->interactionKind == svg::compositor::InteractionHint::ActiveDrag;
    // Pass the document's actual canvas size, NOT the promoted
    // bitmap's intrinsic dimensions. Pre-M2 the promoted bitmap was
    // canvas-sized so the two were equivalent, but after M2A/M2B the
    // bitmap covers only the entity bbox + filter halo. Using the
    // bitmap dims here permanently mismatches `cachedCanvasSize !=
    // currentCanvasSize` in `shouldPrewarm` / `needsExperimentalLayer
    // Capture`, which dispatches a render every UI frame. The
    // resulting busy state blocks `onMouseMove` (gated on `!isBusy()`
    // via `ShouldPostDragMove`) → drag of any successfully-promoted
    // entity produces zero motion. Symptom the user observed:
    // promoted letters can't drag, but letters whose promotion was
    // rejected work via the slow path.
    experimentalDragPresentation_.noteCachedTextures(
        result.compositedPreview->entity, result.version, app.document().document().canvasSize(),
        allowIdleDisplay);
  }

  if (!result.bitmap.empty()) {
    textures.uploadFlat(result.bitmap);
    if (!hasComposited) {
      experimentalDragPresentation_.noteFullRenderLanded(result.version);
    }
  }

  // Mark this `(entity, version, canvasSize)` as "prewarm attempted"
  // even when the result didn't carry a composited preview. Closes
  // the prewarm dispatch loop that would otherwise fire every frame
  // when the selected entity is refused by
  // `CompositorController::promoteEntity` (e.g.
  // `HasCompositingBreakingAncestor`): without this, the worker stays
  // continuously busy and the editor's click handler can never
  // process a new selection. See `ExperimentalDragPresentation::
  // shouldPrewarm` for the matching guard.
  if (app.hasDocument() && app.selectedElement().has_value()) {
    const auto& selected = *app.selectedElement();
    if (selected.isa<svg::SVGGraphicsElement>()) {
      experimentalDragPresentation_.notePrewarmAttempted(
          selected.entityHandle().entity(), result.version, app.document().document().canvasSize());
    }
  }

  displayedDocVersion_ = result.version;
  if (hasComposited && experimentalDragPresentation_.waitingForChromeRefresh && app.hasDocument()) {
    refreshSelectionBoundsCache(app);
    // Preserve the last-known marquee rect across this internal
    // chrome-refresh path. Composited drag lands here; SelectTool isn't
    // reachable, so we reuse whatever the most recent main-thread
    // rasterize baked in.
    rasterizeOverlayForCurrentSelection(app, viewport, textures, lastOverlayMarqueeRectDoc_);
    experimentalDragPresentation_.noteChromeRefreshCompleted(displayedDocVersion_);
  }
  if (pendingOverlayBitmap_.has_value() && pendingOverlayVersion_ == displayedDocVersion_) {
    if (!pendingOverlayBitmap_->empty()) {
      textures.uploadOverlay(*pendingOverlayBitmap_);
    } else {
      textures.clearOverlay();
    }
    pendingOverlayBitmap_.reset();
    pendingOverlayVersion_ = 0;
  }
  promoteSelectionBoundsIfReady();
}

void RenderCoordinator::maybeRequestRender(EditorApp& app, SelectTool& selectTool,
                                           const ViewportState& viewport, bool experimentalMode,
                                           GlTextureCache& textures) {
  ZoneScopedN("RenderCoordinator::maybeRequestRender");
  if (asyncRenderer_.isBusy() || !app.hasDocument() || viewport.paneSize.x <= 0.0 ||
      viewport.paneSize.y <= 0.0) {
    return;
  }

  const Vector2i desiredCanvasSize = viewport.desiredCanvasSize();
  const Vector2i actualDocumentCanvas = app.document().document().canvasSize();
  // Compare desired against what the document *actually reports* now,
  // not against a separate `lastSetCanvasSize_` tracker:
  //   - Avoids the "document forgot the canvas size" bug where a
  //     `ReplaceDocument` (drag-release writeback → source-pane
  //     reparse → `setDocumentMaybeStructural` → fresh SVGDocument
  //     with `canvasSize = nullopt`) leaves the registry reading
  //     `canvasSize()` as viewBox-sized while our cached `lastSet`
  //     still holds the old big value. Symptom: "after a drag the
  //     view goes low-res and stays" (panel shows
  //     `viewport: zoom=… → desired 3954×2269`,
  //     `document canvas: 892×512 ← commit stalled vs desired`).
  //   - 1-pixel tolerance absorbs the aspect-preserving min-scale
  //     rounding `LayoutSystem::calculateCanvasScaledDocumentSize`
  //     applies on read-back; without it, setting `(3954, 2269)`
  //     reads back as `(3953, 2269)` and the next commit refires
  //     with the same value forever at the 120 ms throttle rate.
  pendingCanvasSize_ = desiredCanvasSize;
  const auto now = std::chrono::steady_clock::now();
  const bool throttleElapsed = (now - pendingCanvasSizeSince_) >= kCanvasSizeCommitDelay;
  const bool firstCommit = actualDocumentCanvas == Vector2i::Zero();
  const bool closeEnough = std::abs(pendingCanvasSize_.x - actualDocumentCanvas.x) <= 1 &&
                           std::abs(pendingCanvasSize_.y - actualDocumentCanvas.y) <= 1;
  const bool wouldChange = !closeEnough;
  if (pendingCanvasSize_ != Vector2i::Zero() && wouldChange && (firstCommit || throttleElapsed)) {
    app.document().document().setCanvasSize(pendingCanvasSize_.x, pendingCanvasSize_.y);
    lastSetCanvasSize_ = app.document().document().canvasSize();
    pendingCanvasSizeSince_ = now;
  }

  const Vector2i currentCanvasSize = app.document().document().canvasSize();
  const auto currentVersion = app.document().currentFrameVersion();
  const auto dragPreview = selectTool.activeDragPreview();
  const Entity prewarmEntity = selectedExperimentalEntity(app, experimentalMode);
  experimentalDragPresentation_.clearSettlingIfSelectionChanged(prewarmEntity,
                                                                dragPreview.has_value());

  const bool selectionBoundsChanged = app.selectedElements() != selectionBoundsCache_.lastSelection;
  if (!experimentalMode || !experimentalDragPresentation_.waitingForFullRender ||
      dragPreview.has_value()) {
    if (selectionBoundsChanged || currentVersion != selectionBoundsCache_.lastRefreshVersion) {
      refreshSelectionBoundsCache(app);
    }
  }

  const auto& overlaySelection = app.selectedElements();
  const bool selectionDiffers = overlaySelection != lastOverlaySelectionVec_;
  const std::optional<Box2d> marqueeRectDoc = selectTool.marqueeRect();
  // The marquee rect updates every mouse-move during a marquee drag and
  // doesn't bump the document version (no DOM mutation), so it needs
  // its own invalidation check. Comparing via `!=` on the optional<Box2d>
  // covers "marquee appeared", "marquee moved", and "marquee ended".
  const bool marqueeDiffers = marqueeRectDoc != lastOverlayMarqueeRectDoc_;
  if ((!experimentalMode || !experimentalDragPresentation_.waitingForFullRender ||
       dragPreview.has_value() || marqueeRectDoc.has_value()) &&
      (selectionDiffers || marqueeDiffers || currentCanvasSize != lastOverlayCanvasSize_ ||
       currentVersion != lastOverlayVersion_)) {
    rasterizeOverlayForCurrentSelection(app, viewport, textures, marqueeRectDoc);
  }

  // A prior dispatch for this exact `(entity, version, canvasSize)`
  // combination that landed *without* a composited preview means the
  // compositor can't produce one — typically because the entity has
  // a compositing-breaking ancestor and `promoteEntity` is refusing.
  // Re-dispatching for the same combo would just reproduce the same
  // empty result, holding the worker busy and starving the editor's
  // mouse-move handler (which is gated on `!isBusy()`) — symptom:
  // dragging a letter inside a `<g filter>` produces no visible
  // motion, even though the click selects the element correctly.
  // Same guard as the prewarm path, applied to the drag path. See
  // `ExperimentalDragPresentation::shouldPrewarm` for the prewarm
  // mirror.
  const bool hasCachedTexturesForDrag =
      dragPreview.has_value() && experimentalDragPresentation_.hasCachedTextures &&
      experimentalDragPresentation_.cachedEntity == dragPreview->entity &&
      experimentalDragPresentation_.cachedVersion == currentVersion &&
      experimentalDragPresentation_.cachedCanvasSize == currentCanvasSize;
  const bool sameAsLastFailedDispatch =
      dragPreview.has_value() && !hasCachedTexturesForDrag &&
      experimentalDragPresentation_.lastPrewarmEntity == dragPreview->entity &&
      experimentalDragPresentation_.lastPrewarmVersion == currentVersion &&
      experimentalDragPresentation_.lastPrewarmCanvasSize == currentCanvasSize;

  const bool needsExperimentalLayerCapture =
      experimentalMode && dragPreview.has_value() && !sameAsLastFailedDispatch &&
      (!experimentalDragPresentation_.hasCachedTextures ||
       experimentalDragPresentation_.cachedEntity != dragPreview->entity ||
       experimentalDragPresentation_.cachedVersion != currentVersion ||
       experimentalDragPresentation_.cachedCanvasSize != currentCanvasSize);
  const bool needsSettledSelectionRefresh =
      experimentalMode && !dragPreview.has_value() && prewarmEntity != entt::null &&
      experimentalDragPresentation_.waitingForFullRender &&
      experimentalDragPresentation_.settlingPreview.has_value() &&
      experimentalDragPresentation_.settlingPreview->entity == prewarmEntity &&
      currentVersion >= experimentalDragPresentation_.settlingTargetVersion;
  const bool needsExperimentalPrewarm =
      needsSettledSelectionRefresh ||
      experimentalDragPresentation_.shouldPrewarm(prewarmEntity, currentVersion, currentCanvasSize,
                                                  /*dragActive=*/false);
  const bool needsRegularRender =
      (!experimentalMode || prewarmEntity == entt::null) &&
      (currentVersion != lastRenderedVersion_ || currentCanvasSize != lastRenderedCanvasSize_);

  if (!needsExperimentalLayerCapture && !needsExperimentalPrewarm && !needsRegularRender) {
    return;
  }

  RenderRequest req;
  req.renderer = &renderer_;
  req.document = &app.document().document();
  req.version = currentVersion;
  req.documentGeneration = app.document().documentGeneration();
  // Drain any pending structural remap from a recent `setDocumentMaybe
  // Structural` call. Non-empty remap lets the worker preserve the
  // compositor's cached state across the document swap instead of
  // falling into the full-reset path. Must be consumed on every render
  // request — a second render without consumption would re-apply a
  // stale remap against an already-remapped compositor.
  req.structuralRemap = app.document().consumePendingStructuralRemap();
  req.selection = std::nullopt;
  // Carry the current selection on every render so the compositor can keep
  // the selected entity promoted across drag → idle → drag transitions.
  // Deliberately not gated on experimental mode: the compositor stays
  // warmed against the selection, but only promotes when the entity is
  // actually drag-capable. The pre-warm render that first triggers
  // promotion is still gated separately below.
  if (app.selectedElement().has_value()) {
    const auto& selected = *app.selectedElement();
    if (selected.isa<svg::SVGGraphicsElement>()) {
      req.selectedEntity = selected.entityHandle().entity();
    }
  }
  if (dragPreview.has_value()) {
    req.dragPreview = RenderRequest::DragPreview{
        .entity = dragPreview->entity,
        .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
    };
  } else if (needsExperimentalPrewarm) {
    req.dragPreview = RenderRequest::DragPreview{
        .entity = prewarmEntity,
        .interactionKind = svg::compositor::InteractionHint::Selection,
    };
  }
  asyncRenderer_.requestRender(req);

  if (needsRegularRender) {
    lastRenderedVersion_ = currentVersion;
    lastRenderedCanvasSize_ = currentCanvasSize;
  }
}

Entity RenderCoordinator::selectedExperimentalEntity(EditorApp& app, bool experimentalMode) const {
  if (!experimentalMode || !app.selectedElement().has_value()) {
    return entt::null;
  }

  const auto& selected = *app.selectedElement();
  if (!selected.isa<svg::SVGGraphicsElement>()) {
    return entt::null;
  }

  return selected.entityHandle().entity();
}

}  // namespace donner::editor
