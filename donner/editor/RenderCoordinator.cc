#include "donner/editor/RenderCoordinator.h"

#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectTool.h"

namespace donner::editor {

void RenderCoordinator::resetForLoadedDocument() {
  experimentalDragPresentation_ = ExperimentalDragPresentation{};
  selectionBoundsCache_ = SelectionBoundsCache{};
  pendingOverlayBitmap_.reset();
  pendingOverlayVersion_ = 0;
  displayedDocVersion_ = 0;
  lastOverlaySelectionVec_.clear();
  lastOverlayCanvasSize_ = Vector2i::Zero();
  lastOverlayVersion_ = std::numeric_limits<std::uint64_t>::max();
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

bool RenderCoordinator::rasterizeOverlayForCurrentSelection(EditorApp& app,
                                                            const ViewportState& viewport,
                                                            GlTextureCache& textures) {
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
  OverlayRenderer::drawChromeWithTransform(
      overlayRenderer_, std::span<const svg::SVGElement>(overlaySelection), canvasFromDoc);
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
  return true;
}

void RenderCoordinator::pollRenderResult(EditorApp& app, const ViewportState& viewport,
                                         GlTextureCache& textures) {
  auto resultOpt = asyncRenderer_.pollResult();
  if (!resultOpt.has_value()) {
    return;
  }

  const auto& result = *resultOpt;
  const bool hasComposited =
      result.compositedPreview.has_value() && result.compositedPreview->valid();
  if (hasComposited) {
    textures.uploadComposited(*result.compositedPreview);
    experimentalDragPresentation_.noteCachedTextures(
        result.compositedPreview->entity, result.version,
        Vector2i(result.compositedPreview->promotedBitmap.dimensions.x,
                 result.compositedPreview->promotedBitmap.dimensions.y));
  }

  if (!result.bitmap.empty()) {
    textures.uploadFlat(result.bitmap);
    if (!hasComposited) {
      experimentalDragPresentation_.noteFullRenderLanded(result.version);
    }
  }

  displayedDocVersion_ = result.version;
  if (hasComposited && experimentalDragPresentation_.waitingForChromeRefresh && app.hasDocument()) {
    refreshSelectionBoundsCache(app);
    rasterizeOverlayForCurrentSelection(app, viewport, textures);
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
  if (asyncRenderer_.isBusy() || !app.hasDocument() || viewport.paneSize.x <= 0.0 ||
      viewport.paneSize.y <= 0.0) {
    return;
  }

  const Vector2i desiredCanvasSize = viewport.desiredCanvasSize();
  const Vector2i currentSize = app.document().document().canvasSize();
  if (currentSize.x != desiredCanvasSize.x || currentSize.y != desiredCanvasSize.y) {
    app.document().document().setCanvasSize(desiredCanvasSize.x, desiredCanvasSize.y);
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
  if ((!experimentalMode || !experimentalDragPresentation_.waitingForFullRender ||
       dragPreview.has_value()) &&
      (selectionDiffers || currentCanvasSize != lastOverlayCanvasSize_ ||
       currentVersion != lastOverlayVersion_)) {
    rasterizeOverlayForCurrentSelection(app, viewport, textures);
  }

  const bool needsExperimentalLayerCapture =
      experimentalMode && dragPreview.has_value() &&
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
        .translation = dragPreview->translation,
        .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
    };
  } else if (needsExperimentalPrewarm) {
    req.dragPreview = RenderRequest::DragPreview{
        .entity = prewarmEntity,
        .translation = Vector2d::Zero(),
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
