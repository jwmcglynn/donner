#include "donner/editor/RenderCoordinator.h"

#include <chrono>
#include <cstdlib>
#include <utility>

#include "donner/base/Utils.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/core/Display.h"

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

svg::Renderer CreateRenderer(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice) {
  if (geodeDevice != nullptr) {
    return svg::Renderer(std::move(geodeDevice));
  }
  return svg::Renderer();
}

bool CanvasSizeCloseEnough(const Vector2i& lhs, const Vector2i& rhs) {
  return std::abs(lhs.x - rhs.x) <= 1 && std::abs(lhs.y - rhs.y) <= 1;
}

std::optional<SelectTool::ActiveDragPreview> DragPreviewFromRenderRequest(
    const std::optional<RenderRequest::DragPreview>& preview) {
  if (!preview.has_value() || preview->entity == entt::null) {
    return std::nullopt;
  }

  return SelectTool::ActiveDragPreview{
      .entity = preview->entity,
      .translation = preview->translation,
      .documentFromCachedDocument = preview->documentFromCachedDocument,
      .dragGeneration = preview->dragGeneration,
  };
}

std::optional<SelectTool::ActiveDragPreview> OverlayDragPreviewForSelection(
    EditorApp& app, std::optional<SelectTool::ActiveDragPreview> representedDragPreview) {
  if (representedDragPreview.has_value()) {
    return representedDragPreview;
  }

  if (app.selectedElements().size() != 1u || !app.selectedElement().has_value() ||
      !app.selectedElement()->isa<svg::SVGGraphicsElement>()) {
    return std::nullopt;
  }

  return SelectTool::ActiveDragPreview{
      .entity = app.selectedElement()->unsafeEntityHandle().entity(),
      .translation = Vector2d::Zero(),
  };
}

bool SameTransform(const Transform2d& lhs, const Transform2d& rhs) {
  for (std::size_t i = 0; i < 6; ++i) {
    if (lhs.data[i] != rhs.data[i]) {
      return false;
    }
  }
  return true;
}

bool SameActiveBoundsPreview(const std::optional<SelectTool::ActiveTransformBoundsPreview>& lhs,
                             const std::optional<SelectTool::ActiveTransformBoundsPreview>& rhs) {
  if (lhs.has_value() != rhs.has_value()) {
    return false;
  }
  if (!lhs.has_value()) {
    return true;
  }

  return lhs->startBoundsDoc == rhs->startBoundsDoc &&
         SameTransform(lhs->documentFromStartDocument, rhs->documentFromStartDocument);
}

std::optional<svg::SVGElement> SelectedGraphicsElement(EditorApp& app) {
  if (!app.selectedElement().has_value()) {
    return std::nullopt;
  }

  const auto& selected = *app.selectedElement();
  if (!selected.isa<svg::SVGGraphicsElement>()) {
    return std::nullopt;
  }

  return selected;
}

bool IsDisplayNone(const svg::SVGElement& element) {
  return element.getComputedStyle().display.getRequired() == svg::Display::None;
}

}  // namespace

bool ShouldPresentCompositedPreviewForViewport(const RenderResult::CompositedPreview& preview,
                                               const Vector2i& viewportDesiredCanvas) {
  if (!preview.valid()) {
    return false;
  }

  if (preview.tiles.size() == 1u && preview.tiles.front().id == "full-canvas") {
    return true;
  }

  if (viewportDesiredCanvas.x <= 0 || viewportDesiredCanvas.y <= 0) {
    return false;
  }

  for (const RenderResult::CompositedTile& tile : preview.tiles) {
    if (!CanvasSizeCloseEnough(tile.rasterCanvasSize, viewportDesiredCanvas)) {
      return false;
    }
  }
  return true;
}

bool ShouldUploadImmediateOverlayForPresentedTiles(std::span<const GlTextureCache::TileView> tiles,
                                                   const Vector2i& currentCanvasSize) {
  if (tiles.empty()) {
    return true;
  }

  if (tiles.size() == 1u && tiles.front().id == "full-canvas") {
    return true;
  }

  if (currentCanvasSize.x <= 0 || currentCanvasSize.y <= 0) {
    return false;
  }

  for (const GlTextureCache::TileView& tile : tiles) {
    if (!CanvasSizeCloseEnough(tile.rasterCanvasSize, currentCanvasSize)) {
      return false;
    }
  }
  return true;
}

RenderCoordinator::RenderWorkerBundle::RenderWorkerBundle(
    std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice)
    : renderer(CreateRenderer(std::move(geodeDevice))) {}

RenderCoordinator::RenderCoordinator(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice)
    : renderWorker_(geodeDevice), overlayRenderer_(CreateRenderer(std::move(geodeDevice))) {}

void RenderCoordinator::resetForLoadedDocument() {
  compositedPresentation_ = CompositedPresentation{};
  selectionBoundsCache_ = SelectionBoundsCache{};
  pendingOverlayBitmap_.reset();
  pendingOverlayTexture_.reset();
  pendingOverlayDragPreview_.reset();
  presentedOverlayDragPreview_.reset();
  pendingOverlayVersion_ = 0;
  displayedDocVersion_ = 0;
  lastOverlaySelectionVec_.clear();
  sourceHoverElements_.clear();
  lastOverlaySourceHoverVec_.clear();
  lastOverlayCanvasSize_ = Vector2i::Zero();
  lastOverlayVersion_ = std::numeric_limits<std::uint64_t>::max();
  lastOverlayMarqueeRectDoc_.reset();
  lastOverlayActiveBoundsPreview_.reset();
  renderScheduler_.reset();
  displayNoneSuppressedSelectionEntity_ = entt::null;
  displayNoneSuppressedLayerEntity_ = entt::null;
}

bool RenderCoordinator::setSourceHoverElements(std::vector<svg::SVGElement> elements) {
  if (sourceHoverElements_ == elements) {
    return false;
  }

  sourceHoverElements_ = std::move(elements);
  return true;
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
    const std::optional<Box2d>& marqueeRectDoc, OverlayUploadMode uploadMode,
    std::optional<SelectTool::ActiveDragPreview> representedDragPreview,
    std::optional<SelectTool::ActiveTransformBoundsPreview> activeBoundsPreview) {
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
  std::optional<SelectionChromeBoundsPreview> chromeBoundsPreview;
  if (activeBoundsPreview.has_value()) {
    chromeBoundsPreview = SelectionChromeBoundsPreview{
        .startBoundsDoc = activeBoundsPreview->startBoundsDoc,
        .documentFromStartDocument = activeBoundsPreview->documentFromStartDocument,
    };
  }
  // Overlay AABBs are computed from the same live DOM snapshot as the path
  // outlines. `selectionBoundsCache_` is maintained only for main-loop
  // selection-change detection and does not gate overlay geometry.
  OverlayRenderer::drawChromeWithTransform(
      overlayRenderer_, std::span<const svg::SVGElement>(overlaySelection), marqueeRectDoc,
      canvasFromDoc, chromeBoundsPreview, std::span<const svg::SVGElement>(sourceHoverElements_));
  overlayRenderer_.endFrame();
  pendingOverlayTexture_ = overlayRenderer_.takeTextureSnapshot();
  if (pendingOverlayTexture_ != nullptr) {
    pendingOverlayBitmap_.reset();
  } else if (overlayRenderer_.requiresTextureSnapshotPresentation()) {
    UTILS_RELEASE_ASSERT_MSG(
        pendingOverlayTexture_ != nullptr,
        "Geode overlay rasterization did not produce a GPU texture. Refusing CPU "
        "readback/upload fallback in Geode presentation mode.");
  } else {
    pendingOverlayBitmap_ = overlayRenderer_.takeSnapshot();
  }
  pendingOverlayVersion_ = currentVersion;
  pendingOverlayDragPreview_ =
      OverlayDragPreviewForSelection(app, std::move(representedDragPreview));
  const bool shouldUploadNow =
      currentVersion == displayedDocVersion_ ||
      (uploadMode == OverlayUploadMode::Immediate &&
       ShouldUploadImmediateOverlayForPresentedTiles(textures.tiles(), currentCanvasSize));

  if (shouldUploadNow && pendingOverlayTexture_ != nullptr) {
    textures.uploadOverlayTexture(std::move(pendingOverlayTexture_));
    presentedOverlayDragPreview_ = std::move(pendingOverlayDragPreview_);
    pendingOverlayVersion_ = 0;
  } else if (shouldUploadNow && pendingOverlayBitmap_.has_value() &&
             !pendingOverlayBitmap_->empty()) {
    textures.uploadOverlay(*pendingOverlayBitmap_);
    presentedOverlayDragPreview_ = std::move(pendingOverlayDragPreview_);
    pendingOverlayBitmap_.reset();
    pendingOverlayVersion_ = 0;
  } else if (shouldUploadNow) {
    textures.clearOverlay();
    presentedOverlayDragPreview_.reset();
    pendingOverlayDragPreview_.reset();
    pendingOverlayBitmap_.reset();
    pendingOverlayVersion_ = 0;
  }

  lastOverlaySelectionVec_ = overlaySelection;
  lastOverlaySourceHoverVec_ = sourceHoverElements_;
  lastOverlayCanvasSize_ = currentCanvasSize;
  lastOverlayVersion_ = currentVersion;
  lastOverlayMarqueeRectDoc_ = marqueeRectDoc;
  lastOverlayActiveBoundsPreview_ = activeBoundsPreview;
  return true;
}

void RenderCoordinator::pollRenderResult(EditorApp& app, const ViewportState& viewport,
                                         GlTextureCache& textures, FrameHistory* frameHistory) {
  ZoneScopedN("RenderCoordinator::pollRenderResult");
  auto resultOpt = renderWorker_.asyncRenderer.pollResult();
  if (!resultOpt.has_value()) {
    return;
  }

  const auto& result = *resultOpt;
  // Forward the worker-measured presentation latency to the frame history so
  // `RenderFrameGraph` can overlay async worker time on the UI frame graph.
  // The frame history's latest slot corresponds to the current UI frame
  // (pushed at the top of `EditorShell::runFrame` before poll) — a landed
  // result belongs to that frame.
  if (frameHistory != nullptr) {
    frameHistory->setLatestBackendMs(static_cast<float>(result.workerMs));
  }
  const Vector2i viewportDesiredCanvas = viewport.desiredCanvasSize();
  if (result.compositedPreview.has_value() && result.compositedPreview->valid()) {
    if (!ShouldPresentCompositedPreviewForViewport(*result.compositedPreview,
                                                   viewportDesiredCanvas)) {
      return;
    }

    textures.uploadComposited(*result.compositedPreview);
    if (displayNoneSuppressedLayerEntity_ != entt::null) {
      const bool stillCarriesSuppressedLayer = std::ranges::any_of(
          result.compositedPreview->tiles, [&](const RenderResult::CompositedTile& tile) {
            return tile.kind == RenderResult::CompositedTile::Kind::Layer &&
                   tile.layerEntity == displayNoneSuppressedLayerEntity_;
          });
      if (!stillCarriesSuppressedLayer) {
        displayNoneSuppressedSelectionEntity_ = entt::null;
        displayNoneSuppressedLayerEntity_ = entt::null;
      }
    }
    // Cache identity is keyed to the document canvas size, not the promoted
    // tile's intrinsic dimensions. Bounded layer tiles can be smaller than the
    // canvas, and using their dimensions here would make the cache appear stale
    // on every UI frame.
    compositedPresentation_.noteCachedTextures(
        result.compositedPreview->entity, result.version, app.document().document().canvasSize(),
        DragPreviewFromRenderRequest(result.compositedPreview->representedDragPreview));
  }

  displayedDocVersion_ = result.version;
  renderScheduler_.noteRenderCompleted(result.version,
                                       renderWorker_.asyncRenderer.lastDocumentCanvasSize());
  if (result.compositedPreview.has_value() && result.compositedPreview->valid() &&
      compositedPresentation_.isWaitingForChromeRefresh() && app.hasDocument()) {
    refreshSelectionBoundsCache(app);
    // Preserve the last-known marquee rect across this internal
    // chrome-refresh path. Composited drag lands here; SelectTool isn't
    // reachable, so we reuse whatever the most recent main-thread
    // rasterize baked in.
    rasterizeOverlayForCurrentSelection(app, viewport, textures, lastOverlayMarqueeRectDoc_);
    compositedPresentation_.noteChromeRefreshCompleted(displayedDocVersion_);
  }
  if (pendingOverlayTexture_ != nullptr && pendingOverlayVersion_ == displayedDocVersion_) {
    textures.uploadOverlayTexture(std::move(pendingOverlayTexture_));
    presentedOverlayDragPreview_ = std::move(pendingOverlayDragPreview_);
    pendingOverlayVersion_ = 0;
  } else if (pendingOverlayBitmap_.has_value() && pendingOverlayVersion_ == displayedDocVersion_) {
    if (!pendingOverlayBitmap_->empty()) {
      textures.uploadOverlay(*pendingOverlayBitmap_);
      presentedOverlayDragPreview_ = std::move(pendingOverlayDragPreview_);
    } else {
      textures.clearOverlay();
      presentedOverlayDragPreview_.reset();
      pendingOverlayDragPreview_.reset();
    }
    pendingOverlayBitmap_.reset();
    pendingOverlayVersion_ = 0;
  }
  promoteSelectionBoundsIfReady();
}

void RenderCoordinator::maybeRequestRender(EditorApp& app, SelectTool& selectTool,
                                           const ViewportState& viewport,
                                           GlTextureCache& textures) {
  ZoneScopedN("RenderCoordinator::maybeRequestRender");
  if (renderWorker_.asyncRenderer.isBusy() || !app.hasDocument() || viewport.paneSize.x <= 0.0 ||
      viewport.paneSize.y <= 0.0) {
    return;
  }

  const Vector2i desiredCanvasSize = viewport.desiredCanvasSize();
  const Vector2i actualDocumentCanvas = app.document().document().canvasSize();
  // Compare desired size against the live document size. Document replacement
  // can reset the stored canvas size, and LayoutSystem readback can round by a
  // pixel, so a separate last-set tracker is not authoritative here.
  pendingCanvasSize_ = desiredCanvasSize;
  const auto now = std::chrono::steady_clock::now();
  const bool throttleElapsed = (now - pendingCanvasSizeSince_) >= kCanvasSizeCommitDelay;
  const bool firstCommit = actualDocumentCanvas == Vector2i::Zero();
  const bool closeEnough = std::abs(pendingCanvasSize_.x - actualDocumentCanvas.x) <= 1 &&
                           std::abs(pendingCanvasSize_.y - actualDocumentCanvas.y) <= 1;
  const bool wouldChange = !closeEnough;
  if (pendingCanvasSize_ != Vector2i::Zero() && wouldChange && (firstCommit || throttleElapsed)) {
    app.document().document().setCanvasSize(pendingCanvasSize_.x, pendingCanvasSize_.y);
    pendingCanvasSizeSince_ = now;
  }

  const Vector2i currentCanvasSize = app.document().document().canvasSize();
  const auto currentVersion = app.document().currentFrameVersion();
  const auto dragPreview = selectTool.activeDragPreview();
  const Entity prewarmEntity = selectedCompositedEntity(app);
  const Entity suppressedLayerEntity = suppressedCompositedLayerEntity(app);
  if (suppressedLayerEntity != entt::null) {
    compositedPresentation_.discardCachedTexturesForEntity(suppressedLayerEntity);
  }

  const bool selectionBoundsChanged = app.selectedElements() != selectionBoundsCache_.lastSelection;
  if (!compositedPresentation_.isWaitingForFullRender() || dragPreview.has_value()) {
    if (selectionBoundsChanged || currentVersion != selectionBoundsCache_.lastRefreshVersion) {
      refreshSelectionBoundsCache(app);
    }
  }

  const auto& overlaySelection = app.selectedElements();
  const bool selectionDiffers = overlaySelection != lastOverlaySelectionVec_;
  const bool sourceHoverDiffers = sourceHoverElements_ != lastOverlaySourceHoverVec_;
  const std::optional<Box2d> marqueeRectDoc = selectTool.marqueeRect();
  const auto activeBoundsPreview = selectTool.activeTransformBoundsPreview();
  // The marquee rect updates every mouse-move during a marquee drag and
  // doesn't bump the document version (no DOM mutation), so it needs
  // its own invalidation check. Comparing via `!=` on the optional<Box2d>
  // covers "marquee appeared", "marquee moved", and "marquee ended".
  const bool marqueeDiffers = marqueeRectDoc != lastOverlayMarqueeRectDoc_;
  const bool activeBoundsPreviewDiffers =
      !SameActiveBoundsPreview(activeBoundsPreview, lastOverlayActiveBoundsPreview_);
  const bool canvasDiffers = currentCanvasSize != lastOverlayCanvasSize_;
  const bool overlayVersionDiffers = currentVersion != lastOverlayVersion_;
  if ((!compositedPresentation_.isWaitingForFullRender() || dragPreview.has_value() ||
       marqueeRectDoc.has_value() || activeBoundsPreviewDiffers || sourceHoverDiffers) &&
      (selectionDiffers || sourceHoverDiffers || marqueeDiffers || canvasDiffers ||
       activeBoundsPreviewDiffers || overlayVersionDiffers)) {
    rasterizeOverlayForCurrentSelection(
        app, viewport, textures, marqueeRectDoc,
        dragPreview.has_value() || suppressedLayerEntity != entt::null
            ? OverlayUploadMode::Immediate
            : OverlayUploadMode::MatchDisplayedVersion,
        dragPreview, activeBoundsPreview);
  }

  const PresentationRenderScheduleDecision schedule =
      renderScheduler_.evaluate(compositedPresentation_, PresentationRenderScheduleInput{
                                                             .selectedEntity = prewarmEntity,
                                                             .activeDragPreview = dragPreview,
                                                             .currentVersion = currentVersion,
                                                             .currentCanvasSize = currentCanvasSize,
                                                         });
  if (!schedule.shouldRequestRender()) {
    return;
  }

  RenderRequest req(renderWorker_.renderer, app.document().document());
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
  // Carry the current renderable selection on every render so the compositor can keep the selected
  // entity promoted across drag → idle → drag transitions. A selected `display:none` element keeps
  // editor chrome but must not keep or refresh a promoted content layer.
  if (prewarmEntity != entt::null) {
    req.selectedEntity = prewarmEntity;
  }
  if (schedule.dragPreview.has_value()) {
    req.dragPreview = *schedule.dragPreview;
  }
  renderWorker_.asyncRenderer.requestRender(req);
}

Entity RenderCoordinator::selectedCompositedEntity(EditorApp& app) const {
  const std::optional<svg::SVGElement> selected = SelectedGraphicsElement(app);
  if (!selected.has_value()) {
    return entt::null;
  }

  if (IsDisplayNone(*selected)) {
    return entt::null;
  }

  return selected->unsafeEntityHandle().entity();
}

Entity RenderCoordinator::suppressedCompositedLayerEntity(EditorApp& app) {
  const std::optional<svg::SVGElement> selected = SelectedGraphicsElement(app);
  if (!selected.has_value()) {
    return displayNoneSuppressedLayerEntity_;
  }

  if (!IsDisplayNone(*selected)) {
    const Entity selectedEntity = selected->unsafeEntityHandle().entity();
    if (selectedEntity == displayNoneSuppressedSelectionEntity_ ||
        selectedEntity == displayNoneSuppressedLayerEntity_) {
      displayNoneSuppressedSelectionEntity_ = entt::null;
      displayNoneSuppressedLayerEntity_ = entt::null;
      return entt::null;
    }

    return displayNoneSuppressedLayerEntity_;
  }

  const Entity selectedEntity = selected->unsafeEntityHandle().entity();
  const CompositedPresentation::DiagnosticsSnapshot diagnostics =
      compositedPresentation_.diagnostics();
  if (diagnostics.hasCachedTextures && diagnostics.cachedEntity != entt::null) {
    displayNoneSuppressedSelectionEntity_ = selectedEntity;
    displayNoneSuppressedLayerEntity_ = diagnostics.cachedEntity;
    return diagnostics.cachedEntity;
  }

  if (displayNoneSuppressedSelectionEntity_ == selectedEntity &&
      displayNoneSuppressedLayerEntity_ != entt::null) {
    return displayNoneSuppressedLayerEntity_;
  }

  displayNoneSuppressedSelectionEntity_ = selectedEntity;
  displayNoneSuppressedLayerEntity_ = selectedEntity;
  return selectedEntity;
}

bool RenderCoordinator::selectedElementIsDisplayNone(EditorApp& app) const {
  const std::optional<svg::SVGElement> selected = SelectedGraphicsElement(app);
  return selected.has_value() && IsDisplayNone(*selected);
}

}  // namespace donner::editor
