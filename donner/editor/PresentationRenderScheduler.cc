#include "donner/editor/PresentationRenderScheduler.h"

#include <cstddef>

#include "donner/base/MathUtils.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"

namespace donner::editor {
namespace {

bool SameTransform(const Transform2d& lhs, const Transform2d& rhs) {
  constexpr double kTolerance = 1e-6;
  for (std::size_t i = 0; i < 6; ++i) {
    if (!NearEquals(lhs.data[i], rhs.data[i], kTolerance)) {
      return false;
    }
  }
  return true;
}

bool SameRasterViewport(const EditorRasterViewport& lhs, const EditorRasterViewport& rhs) {
  return lhs.documentRect == rhs.documentRect && lhs.outputSizePx == rhs.outputSizePx &&
         lhs.semanticCanvasSizePx == rhs.semanticCanvasSizePx &&
         lhs.viewportBounded == rhs.viewportBounded &&
         SameTransform(lhs.outputFromDocument, rhs.outputFromDocument);
}

bool SameDragPreviewTransform(const SelectTool::ActiveDragPreview& lhs,
                              const SelectTool::ActiveDragPreview& rhs) {
  constexpr double kTolerance = 1e-6;
  return lhs.entity == rhs.entity && lhs.extraEntities == rhs.extraEntities &&
         lhs.dragGeneration == rhs.dragGeneration &&
         NearEquals(lhs.translation.x, rhs.translation.x, kTolerance) &&
         NearEquals(lhs.translation.y, rhs.translation.y, kTolerance) &&
         SameTransform(lhs.documentFromCachedDocument, rhs.documentFromCachedDocument);
}

}  // namespace

void PresentationRenderScheduler::reset() {
  lastRenderedVersion_ = 0;
  lastRenderedCanvasSize_ = Vector2i::Zero();
  lastRenderedRasterViewport_.reset();
}

PresentationRenderScheduleDecision PresentationRenderScheduler::evaluate(
    CompositedPresentation& presentation, const PresentationRenderScheduleInput& input) const {
  presentation.clearSettlingIfSelectionChanged(input.selectedEntity,
                                               input.activeDragPreview.has_value());

  PresentationRenderScheduleDecision decision;
  decision.currentVersion = input.currentVersion;
  decision.currentCanvasSize = input.currentCanvasSize;
  decision.currentRasterViewport = input.currentRasterViewport;
  const bool deferIdentityActiveDragCapture =
      input.deferIdentityActiveDragCapture && input.activeDragPreview.has_value() &&
      input.activeDragPreview->documentFromCachedDocument.isIdentity() &&
      NearEquals(input.activeDragPreview->translation.x, 0.0) &&
      NearEquals(input.activeDragPreview->translation.y, 0.0);
  if (!deferIdentityActiveDragCapture) {
    decision.needsCompositedLayerCapture = presentation.needsCompositedLayerCapture(
        input.activeDragPreview, input.currentVersion, input.currentCanvasSize,
        input.dragTranslationRecaptureDistanceDoc);
  }
  if (!deferIdentityActiveDragCapture && input.requiresRenderedActiveDragPresentation &&
      input.activeDragPreview.has_value()) {
    const std::optional<SelectTool::ActiveDragPreview> representedPreview =
        presentation.presentationPreview(input.activeDragPreview);
    decision.needsRenderedActiveDragPresentation =
        !representedPreview.has_value() ||
        !SameDragPreviewTransform(*input.activeDragPreview, *representedPreview);
  }
  const bool versionChanged = input.currentVersion != lastRenderedVersion_;
  const bool canvasSizeChanged = input.currentCanvasSize != lastRenderedCanvasSize_;
  const bool rasterViewportChanged =
      !lastRenderedRasterViewport_.has_value() ||
      !SameRasterViewport(input.currentRasterViewport, *lastRenderedRasterViewport_);
  const bool needsSettledSelectionRefresh =
      !input.activeDragPreview.has_value() &&
      presentation.needsSettledSelectionRefresh(input.selectedEntity, input.currentVersion);
  const bool needsSettledLayerRasterization =
      !input.activeDragPreview.has_value() &&
      presentation.needsSettledLayerRasterization(input.selectedEntity, input.currentVersion);
  const bool selectedLayerCacheMissing =
      !input.activeDragPreview.has_value() && input.selectedEntity != entt::null &&
      !presentation.hasCachedTexturesForEntity(input.selectedEntity);
  const bool selectedLayerNeedsForcedRasterization = !input.activeDragPreview.has_value() &&
                                                     input.selectedEntity != entt::null &&
                                                     input.forceSelectedLayerRasterization;
  // A selected element needs a selection-hint render whenever the raster window changes. Otherwise
  // a high-zoom pan/zoom regular render can publish a full-canvas fallback and evict the promoted
  // drag-target tile just before the next drag starts.
  const bool selectedViewportRenderNeedsPrewarm = input.selectedEntity != entt::null &&
                                                  !input.activeDragPreview.has_value() &&
                                                  (canvasSizeChanged || rasterViewportChanged);
  decision.needsRegularRender = input.forcePresentationRefresh || versionChanged ||
                                canvasSizeChanged || rasterViewportChanged;
  const bool selectionCacheNeedsPrewarm =
      presentation.shouldPrewarm(input.selectedEntity, input.selectedExtraEntities,
                                 input.currentVersion, input.currentCanvasSize,
                                 /*dragActive=*/input.activeDragPreview.has_value()) ||
      selectedViewportRenderNeedsPrewarm;
  decision.needsCompositedPrewarm =
      needsSettledSelectionRefresh || selectedLayerNeedsForcedRasterization ||
      (selectionCacheNeedsPrewarm &&
       (input.selectionOnlyPrewarmMayTriggerRender || decision.needsRegularRender));

  if (input.activeDragPreview.has_value()) {
    decision.dragPreview = RenderRequest::DragPreview{
        .entity = input.activeDragPreview->entity,
        .extraEntities = input.activeDragPreview->extraEntities,
        .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
        .translation = input.activeDragPreview->translation,
        .documentFromCachedDocument = input.activeDragPreview->documentFromCachedDocument,
        .dragGeneration = input.activeDragPreview->dragGeneration,
        .forceLayerRasterization = decision.needsCompositedLayerCapture,
    };
  } else if (decision.needsCompositedPrewarm && input.selectedEntity != entt::null) {
    decision.dragPreview = RenderRequest::DragPreview{
        .entity = input.selectedEntity,
        .extraEntities = input.selectedExtraEntities,
        .interactionKind = svg::compositor::InteractionHint::Selection,
        .forceLayerRasterization = needsSettledLayerRasterization || selectedLayerCacheMissing ||
                                   selectedLayerNeedsForcedRasterization,
    };
  }
  if (input.activeDragPreview.has_value() && !input.requiresRenderedActiveDragPresentation) {
    decision.needsRegularRender = false;
  }

  return decision;
}

void PresentationRenderScheduler::noteRenderCompleted(
    std::uint64_t completedVersion, const Vector2i& completedCanvasSize,
    const EditorRasterViewport& completedRasterViewport) {
  lastRenderedVersion_ = completedVersion;
  lastRenderedCanvasSize_ = completedCanvasSize;
  lastRenderedRasterViewport_ = completedRasterViewport;
}

}  // namespace donner::editor
