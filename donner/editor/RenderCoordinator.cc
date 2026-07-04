#include "donner/editor/RenderCoordinator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>

#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/core/Display.h"

namespace donner::editor {

namespace {

bool IsGraphicsElement(const svg::SVGElement& element) {
  return element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
    return element.isa<svg::SVGGraphicsElement>();
  });
}

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
constexpr double kOverlayCullMarginScreenPx = 64.0;
constexpr std::size_t kLargeSelectionOverlayLodThreshold = 128;
constexpr std::chrono::milliseconds kLargeSelectionFullDetailDelay{120};

svg::Renderer CreateRenderer(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice) {
  if (geodeDevice != nullptr) {
    return svg::Renderer(std::move(geodeDevice));
  }
  return svg::Renderer();
}

bool CanvasSizeCloseEnough(const Vector2i& lhs, const Vector2i& rhs) {
  return std::abs(lhs.x - rhs.x) <= 1 && std::abs(lhs.y - rhs.y) <= 1;
}

int OverlayRasterDimension(double logicalSize, double devicePixelRatio) {
  if (!(logicalSize > 0.0) || !(devicePixelRatio > 0.0)) {
    return 1;
  }
  return std::max(1, static_cast<int>(std::round(logicalSize * devicePixelRatio)));
}

Vector2i OverlayRasterSizeForViewport(const ViewportState& viewport) {
  return Vector2i(OverlayRasterDimension(viewport.paneSize.x, viewport.devicePixelRatio),
                  OverlayRasterDimension(viewport.paneSize.y, viewport.devicePixelRatio));
}

Box2d OverlayScreenRectForViewport(const ViewportState& viewport) {
  return Box2d(viewport.paneOrigin, viewport.paneOrigin + viewport.paneSize);
}

Box2d OverlayCullRectDocForViewport(const ViewportState& viewport) {
  const Box2d paneRect = OverlayScreenRectForViewport(viewport);
  const double pixelsPerDocUnit = std::abs(viewport.pixelsPerDocUnit());
  const double marginDoc = pixelsPerDocUnit > 1e-9 ? kOverlayCullMarginScreenPx / pixelsPerDocUnit
                                                   : kOverlayCullMarginScreenPx;
  return viewport.screenToDocument(paneRect).inflatedBy(marginDoc);
}

Transform2d OverlayCanvasFromDocumentTransform(const ViewportState& viewport) {
  const double devicePixelsPerDocUnit = viewport.devicePixelsPerDocUnit();
  const Vector2d overlayCanvasOriginFromDocumentOrigin =
      (viewport.panScreenPoint - viewport.paneOrigin) * viewport.devicePixelRatio -
      viewport.panDocPoint * devicePixelsPerDocUnit;

  Transform2d overlayCanvasFromDocument(Transform2d::uninitialized);
  overlayCanvasFromDocument.data[0] = devicePixelsPerDocUnit;
  overlayCanvasFromDocument.data[1] = 0.0;
  overlayCanvasFromDocument.data[2] = 0.0;
  overlayCanvasFromDocument.data[3] = devicePixelsPerDocUnit;
  overlayCanvasFromDocument.data[4] = overlayCanvasOriginFromDocumentOrigin.x;
  overlayCanvasFromDocument.data[5] = overlayCanvasOriginFromDocumentOrigin.y;
  return overlayCanvasFromDocument;
}

std::optional<SelectTool::ActiveDragPreview> DragPreviewFromRenderRequest(
    const std::optional<RenderRequest::DragPreview>& preview) {
  if (!preview.has_value() || preview->entity == entt::null) {
    return std::nullopt;
  }

  return SelectTool::ActiveDragPreview{
      .entity = preview->entity,
      .extraEntities = preview->extraEntities,
      .translation = preview->translation,
      .documentFromCachedDocument = preview->documentFromCachedDocument,
      .dragGeneration = preview->dragGeneration,
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

bool SameRasterViewport(const EditorRasterViewport& lhs, const EditorRasterViewport& rhs) {
  return lhs.documentRect == rhs.documentRect && lhs.outputSizePx == rhs.outputSizePx &&
         lhs.semanticCanvasSizePx == rhs.semanticCanvasSizePx &&
         lhs.viewportBounded == rhs.viewportBounded &&
         SameTransform(lhs.outputFromDocument, rhs.outputFromDocument);
}

bool SameRasterScaleAndSemanticCanvas(const EditorRasterViewport& lhs,
                                      const EditorRasterViewport& rhs) {
  return lhs.semanticCanvasSizePx == rhs.semanticCanvasSizePx && lhs.viewportBounded &&
         rhs.viewportBounded && lhs.outputFromDocument.data[0] == rhs.outputFromDocument.data[0] &&
         lhs.outputFromDocument.data[1] == rhs.outputFromDocument.data[1] &&
         lhs.outputFromDocument.data[2] == rhs.outputFromDocument.data[2] &&
         lhs.outputFromDocument.data[3] == rhs.outputFromDocument.data[3];
}

bool DocumentRectContains(const Box2d& outer, const Box2d& inner) {
  constexpr double kTolerance = 1e-6;
  return outer.topLeft.x <= inner.topLeft.x + kTolerance &&
         outer.topLeft.y <= inner.topLeft.y + kTolerance &&
         outer.bottomRight.x + kTolerance >= inner.bottomRight.x &&
         outer.bottomRight.y + kTolerance >= inner.bottomRight.y;
}

bool RasterViewportCanPresentCurrentViewport(const EditorRasterViewport& rendered,
                                             const EditorRasterViewport& current) {
  return SameRasterViewport(rendered, current) ||
         (SameRasterScaleAndSemanticCanvas(rendered, current) &&
          DocumentRectContains(rendered.documentRect, current.documentRect));
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
  if (!IsGraphicsElement(selected)) {
    return std::nullopt;
  }

  return selected;
}

bool IsDisplayNone(const svg::SVGElement& element) {
  if (const svg::PropertyRegistry* computedStyle = element.computedStyleIfPresent()) {
    return computedStyle->display.getOr(svg::Display::Inline) == svg::Display::None;
  }

  if (const svg::PropertyRegistry* specifiedStyle = element.specifiedStyle()) {
    return specifiedStyle->display.getOr(svg::Display::Inline) == svg::Display::None;
  }

  return false;
}

bool SubtreeContainsEntity(const svg::SVGElement& element, Entity entity) {
  if (element.unsafeEntityHandle().entity() == entity) {
    return true;
  }

  for (auto child = element.firstChild(); child.has_value(); child = child->nextSibling()) {
    if (SubtreeContainsEntity(*child, entity)) {
      return true;
    }
  }

  return false;
}

bool DocumentContainsEntity(const svg::SVGDocument& document, Entity entity) {
  if (entity == entt::null) {
    return false;
  }
  return SubtreeContainsEntity(document.svgElement(), entity);
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

FrameCostBreakdown::CompositedRender CompositedRenderCostFromStats(
    const svg::compositor::CompositorController::RenderFrameStats& stats) {
  return FrameCostBreakdown::CompositedRender{
      .immediateMs = stats.immediateRasterizeMs,
      .cachedMs = stats.cachedRasterizeMs,
      .immediateTileCount = stats.immediateTileCount,
      .cachedTileCount = stats.cachedTileCount,
  };
}

bool IsUnsetTimePoint(std::chrono::steady_clock::time_point timePoint) {
  return timePoint == std::chrono::steady_clock::time_point{};
}

SelectionChromeDetail ChooseSelectionChromeDetail(
    std::size_t selectedElementCount, bool overlayInteractionActive,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point overlayStableSince) {
  if (selectedElementCount < kLargeSelectionOverlayLodThreshold) {
    return SelectionChromeDetail::Full;
  }

  if (overlayInteractionActive || IsUnsetTimePoint(overlayStableSince) ||
      now - overlayStableSince < kLargeSelectionFullDetailDelay) {
    return SelectionChromeDetail::CombinedBoundsOnly;
  }

  return SelectionChromeDetail::Full;
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

std::uint64_t PostReleaseSettleTargetVersion(std::uint64_t currentFrameVersion,
                                             bool hasPendingMutations) {
  if (hasPendingMutations && currentFrameVersion < std::numeric_limits<std::uint64_t>::max()) {
    return currentFrameVersion + 1u;
  }
  return currentFrameVersion;
}

bool ShouldDeferSelectedViewportRefresh(Entity selectedEntity, bool hasActiveDrag,
                                        std::uint64_t currentVersion,
                                        std::uint64_t displayedDocVersion,
                                        bool hasCachedSelectedTexture, bool rasterViewportSettled,
                                        bool needsOverviewInfill,
                                        bool pendingSelectedLayerRasterization) {
  return selectedEntity != entt::null && !hasActiveDrag && currentVersion == displayedDocVersion &&
         hasCachedSelectedTexture && !rasterViewportSettled && !needsOverviewInfill &&
         !pendingSelectedLayerRasterization;
}

bool ShouldClearPendingSelectedLayerRasterization(
    const std::optional<RenderRequest::DragPreview>& representedDragPreview, Entity pendingEntity,
    std::uint64_t resultVersion, std::uint64_t pendingVersion) {
  return pendingEntity != entt::null && resultVersion >= pendingVersion &&
         representedDragPreview.has_value() && representedDragPreview->entity == pendingEntity &&
         representedDragPreview->forceLayerRasterization;
}

bool CompositedPreviewClearsPendingSelectedLayerRasterization(
    const RenderResult::CompositedPreview& preview, Entity pendingEntity,
    std::uint64_t resultVersion, std::uint64_t pendingVersion) {
  return preview.entity == pendingEntity &&
         ShouldClearPendingSelectedLayerRasterization(preview.representedDragPreview, pendingEntity,
                                                      resultVersion, pendingVersion);
}

std::optional<SelectTool::ActiveDragPreview> OverlayRepresentedDragPreviewForPresentation(
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedDragPreview,
    bool hasPresentableActiveDragTarget) {
  if (!activeDragPreview.has_value()) {
    return std::nullopt;
  }

  if (hasPresentableActiveDragTarget) {
    return activeDragPreview;
  }

  if (displayedDragPreview.has_value() &&
      displayedDragPreview->entity == activeDragPreview->entity &&
      displayedDragPreview->dragGeneration == activeDragPreview->dragGeneration) {
    return displayedDragPreview;
  }

  return SelectTool::ActiveDragPreview{
      .entity = activeDragPreview->entity,
      .translation = Vector2d::Zero(),
      .documentFromCachedDocument = Transform2d(),
      .dragGeneration = activeDragPreview->dragGeneration,
  };
}

Transform2d OverlayRepresentedDocumentFromLiveDocument(
    const std::optional<SelectTool::ActiveDragPreview>& liveDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& representedDragPreview) {
  if (!liveDragPreview.has_value() || !representedDragPreview.has_value() ||
      liveDragPreview->entity != representedDragPreview->entity ||
      liveDragPreview->dragGeneration != representedDragPreview->dragGeneration ||
      std::abs(liveDragPreview->documentFromCachedDocument.determinant()) < 1e-12) {
    return Transform2d();
  }

  return liveDragPreview->documentFromCachedDocument.inverse() *
         representedDragPreview->documentFromCachedDocument;
}

std::optional<SelectTool::ActiveGesturePreview> OverlayGesturePreviewForPresentation(
    const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview,
    const std::optional<SelectTool::ActiveDragPreview>& liveDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& representedDragPreview) {
  if (!activeGesturePreview.has_value()) {
    return std::nullopt;
  }

  SelectTool::ActiveGesturePreview representedGesturePreview = *activeGesturePreview;
  const Transform2d representedDocumentFromLiveDocument =
      OverlayRepresentedDocumentFromLiveDocument(liveDragPreview, representedDragPreview);
  representedGesturePreview.documentFromStartDocument =
      representedDocumentFromLiveDocument * activeGesturePreview->documentFromStartDocument;
  if (liveDragPreview.has_value() && representedDragPreview.has_value() &&
      liveDragPreview->entity == representedDragPreview->entity &&
      liveDragPreview->dragGeneration == representedDragPreview->dragGeneration) {
    representedGesturePreview.currentDocumentDelta = representedDragPreview->translation;
  }
  return representedGesturePreview;
}

RenderCoordinator::RenderWorkerBundle::RenderWorkerBundle(
    std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice)
    : renderer(CreateRenderer(std::move(geodeDevice))) {}

RenderCoordinator::RenderCoordinator(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice)
    : renderWorker_(std::move(geodeDevice)) {}

void RenderCoordinator::resetForLoadedDocument() {
  compositedPresentation_ = CompositedPresentation{};
  selectionBoundsCache_ = SelectionBoundsCache{};
  displayedDocVersion_ = 0;
  lastOverlaySelectionVec_.clear();
  sourceHoverElements_.clear();
  lockedRejectionFlash_.reset();
  lastOverlaySourceHoverVec_.clear();
  immediateOverlaySnapshot_.reset();
  lastOverlayRasterSize_ = Vector2i::Zero();
  lastOverlayScreenRect_.reset();
  lastOverlayCanvasFromDocument_.reset();
  lastOverlaySelectionDetail_ = SelectionChromeDetail::Full;
  lastOverlayInteractionActive_ = false;
  overlayStableSince_ = std::chrono::steady_clock::time_point{};
  lastOverlayVersion_ = std::numeric_limits<std::uint64_t>::max();
  lastOverlayMarqueeRectDoc_.reset();
  lastOverlayActiveBoundsPreview_.reset();
  penLivePreviewElement_.reset();
  lastOverlayPenLivePreviewElement_.reset();
  penHoverPreviewSegmentDoc_.reset();
  penHoverCloseAffordanceDoc_.reset();
  lastOverlayPenHoverPreviewSegmentDoc_.reset();
  lastOverlayPenHoverCloseAffordanceDoc_.reset();
  renderScheduler_.reset();
  displayNoneSuppressedSelectionEntity_ = entt::null;
  displayNoneSuppressedLayerEntity_ = entt::null;
  pendingSelectedLayerRasterizationEntity_ = entt::null;
  pendingSelectedLayerRasterizationVersion_ = 0;
  pendingCanvasSize_ = Vector2i::Zero();
  pendingCanvasSizeSince_ = std::chrono::steady_clock::time_point{};
  pendingRasterViewport_.reset();
  pendingRasterViewportSince_ = std::chrono::steady_clock::time_point{};
  pendingDocumentMutationOverviewRefresh_ = false;
  lastFrameCostBreakdown_ = FrameCostBreakdown{};
}

bool RenderCoordinator::setSourceHoverElements(std::vector<svg::SVGElement> elements) {
  if (sourceHoverElements_ == elements) {
    return false;
  }

  sourceHoverElements_ = std::move(elements);
  return true;
}

void RenderCoordinator::setLockedRejectionFlash(
    std::optional<SelectTool::LockedRejectionFlash> flash) {
  lockedRejectionFlash_ = std::move(flash);
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
    EditorApp& app, const ViewportState& viewport, const std::optional<Box2d>& marqueeRectDoc,
    std::optional<SelectTool::ActiveDragPreview> representedDragPreview,
    std::optional<SelectTool::ActiveTransformBoundsPreview> activeBoundsPreview,
    std::optional<SelectionChromeDetail> selectionDetail,
    std::optional<SelectTool::ActiveDragPreview> liveDragPreview) {
  ZoneScopedN("RenderCoordinator::rasterizeOverlay");
  if (!app.hasDocument()) {
    immediateOverlaySnapshot_.reset();
    return false;
  }

  const Vector2i currentOverlayRasterSize = OverlayRasterSizeForViewport(viewport);
  const Box2d currentOverlayScreenRect = OverlayScreenRectForViewport(viewport);
  const Box2d currentOverlayCullRectDoc = OverlayCullRectDocForViewport(viewport);
  const Transform2d currentOverlayCanvasFromDocument = OverlayCanvasFromDocumentTransform(viewport);
  const auto currentVersion = app.document().currentFrameVersion();
  const auto now = std::chrono::steady_clock::now();
  const auto& overlaySelection = app.selectedElements();
  const std::optional<SelectTool::ActiveDragPreview> effectiveLiveDragPreview =
      liveDragPreview.has_value() ? liveDragPreview : representedDragPreview;
  // An active locked-rejection flash fades every frame, so it must force a fresh overlay capture
  // (defeating the "geometry unchanged → reuse cached overlay" fast path below) until it expires.
  const bool lockedFlashActive =
      lockedRejectionFlash_.has_value() && lockedRejectionFlash_->intensity > 0.0f;
  const bool overlayInteractionActive =
      effectiveLiveDragPreview.has_value() || representedDragPreview.has_value() ||
      marqueeRectDoc.has_value() || activeBoundsPreview.has_value() || lockedFlashActive;
  const bool overlayGeometryDiffers =
      overlaySelection != lastOverlaySelectionVec_ ||
      sourceHoverElements_ != lastOverlaySourceHoverVec_ ||
      marqueeRectDoc != lastOverlayMarqueeRectDoc_ ||
      currentOverlayRasterSize != lastOverlayRasterSize_ || !lastOverlayScreenRect_.has_value() ||
      currentOverlayScreenRect != *lastOverlayScreenRect_ ||
      !lastOverlayCanvasFromDocument_.has_value() ||
      !SameTransform(currentOverlayCanvasFromDocument, *lastOverlayCanvasFromDocument_) ||
      !SameActiveBoundsPreview(activeBoundsPreview, lastOverlayActiveBoundsPreview_) ||
      currentVersion != lastOverlayVersion_ ||
      overlayInteractionActive != lastOverlayInteractionActive_ ||
      penLivePreviewElement_ != lastOverlayPenLivePreviewElement_ ||
      penHoverPreviewSegmentDoc_ != lastOverlayPenHoverPreviewSegmentDoc_ ||
      penHoverCloseAffordanceDoc_ != lastOverlayPenHoverCloseAffordanceDoc_;
  if (!selectionDetail.has_value()) {
    if (overlayGeometryDiffers || IsUnsetTimePoint(overlayStableSince_)) {
      overlayStableSince_ = now;
    }

    selectionDetail = ChooseSelectionChromeDetail(overlaySelection.size(), overlayInteractionActive,
                                                  now, overlayStableSince_);
  }
  const SelectionChromeDetail resolvedSelectionDetail = *selectionDetail;

  if (!overlayInteractionActive && !overlayGeometryDiffers &&
      resolvedSelectionDetail == lastOverlaySelectionDetail_ &&
      immediateOverlaySnapshot_.has_value()) {
    FrameCostBreakdown::Overlay overlayCost;
    overlayCost.canvasSize = currentOverlayRasterSize;
    overlayCost.selectedElementCount = static_cast<int>(overlaySelection.size());
    overlayCost.sourceHoverElementCount = static_cast<int>(sourceHoverElements_.size());
    overlayCost.selectionBoundsOnly =
        resolvedSelectionDetail == SelectionChromeDetail::CombinedBoundsOnly;
    lastFrameCostBreakdown_.overlay = overlayCost;
    return false;
  }

  FrameCostBreakdown::Overlay overlayCost;
  overlayCost.canvasSize = currentOverlayRasterSize;
  overlayCost.selectedElementCount = static_cast<int>(app.selectedElements().size());
  overlayCost.sourceHoverElementCount = static_cast<int>(sourceHoverElements_.size());
  overlayCost.selectionBoundsOnly =
      resolvedSelectionDetail == SelectionChromeDetail::CombinedBoundsOnly;
  overlayCost.hasLiveDragPreview = effectiveLiveDragPreview.has_value();
  overlayCost.hasRepresentedDragPreview = representedDragPreview.has_value();
  if (effectiveLiveDragPreview.has_value()) {
    overlayCost.liveDragTranslationDoc = effectiveLiveDragPreview->translation;
  }
  if (representedDragPreview.has_value()) {
    overlayCost.representedDragTranslationDoc = representedDragPreview->translation;
  }
  const Transform2d representedDocumentFromLiveDocument =
      OverlayRepresentedDocumentFromLiveDocument(effectiveLiveDragPreview, representedDragPreview);

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
  const auto captureStart = std::chrono::steady_clock::now();
  std::optional<LockedRejectionFlashInput> lockedFlashInput;
  if (lockedFlashActive) {
    lockedFlashInput = LockedRejectionFlashInput{
        .element = lockedRejectionFlash_->element,
        .intensity = lockedRejectionFlash_->intensity,
    };
  }
  SelectionChromeSnapshot chromeSnapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(overlaySelection), marqueeRectDoc,
      currentOverlayCanvasFromDocument, chromeBoundsPreview,
      std::span<const svg::SVGElement>(sourceHoverElements_), currentOverlayCullRectDoc,
      resolvedSelectionDetail, representedDocumentFromLiveDocument, lockedFlashInput,
      viewport.devicePixelRatio, penLivePreviewElement_);
  // Pen hover chrome is pushed state (no registry reads), stamped onto the
  // snapshot after capture.
  chromeSnapshot.penPreviewSegmentDoc = penHoverPreviewSegmentDoc_;
  chromeSnapshot.penCloseAffordanceDoc = penHoverCloseAffordanceDoc_;
  overlayCost.captureMs = MillisecondsSince(captureStart);
  overlayCost.pathCount = static_cast<int>(chromeSnapshot.paths.size());
  overlayCost.hoverPathCount = static_cast<int>(chromeSnapshot.hoverPaths.size());
  overlayCost.aabbCount = static_cast<int>(chromeSnapshot.aabbsDoc.size());
  overlayCost.hoverAabbCount = static_cast<int>(chromeSnapshot.hoverAabbsDoc.size());
  overlayCost.handleCount = static_cast<int>(chromeSnapshot.handleBoxesDoc.size());
  overlayCost.hasMarquee = chromeSnapshot.marqueeDoc.has_value();
  // Report the overlay payload this frame so the metric is non-zero whenever the
  // overlay was re-rasterized with content — independent of presentation
  // backend. The TinySkia path uploads the overlay raster texture (its
  // `payloadBytes` is that texture's bytes); the Geode/WGPU path renders the
  // snapshot direct to the framebuffer with no upload, so without this the
  // metric would read 0 even though the overlay was rebuilt every frame
  // (gl_rnr GeodeDragZoomRerasterizes...). Mirror the upload metric: the overlay
  // raster bytes, gated on the snapshot actually having something to draw.
  const bool overlayHasContent =
      !chromeSnapshot.paths.empty() || !chromeSnapshot.hoverPaths.empty() ||
      !chromeSnapshot.handleBoxesDoc.empty() || !chromeSnapshot.aabbsDoc.empty() ||
      !chromeSnapshot.pathAnchorBoxesDoc.empty() || !chromeSnapshot.pathControlLinesDoc.empty() ||
      !chromeSnapshot.pathControlPointBoxesDoc.empty() || chromeSnapshot.marqueeDoc.has_value() ||
      chromeSnapshot.orientedBoundsDoc.has_value() || chromeSnapshot.livePathPreview.has_value() ||
      chromeSnapshot.penPreviewSegmentDoc.has_value() ||
      chromeSnapshot.penCloseAffordanceDoc.has_value();
  overlayCost.payloadBytes =
      overlayHasContent
          ? static_cast<std::uint64_t>(std::max(0, currentOverlayRasterSize.x)) *
                static_cast<std::uint64_t>(std::max(0, currentOverlayRasterSize.y)) * 4u
          : 0u;
  immediateOverlaySnapshot_ = chromeSnapshot;

  lastOverlaySelectionVec_ = overlaySelection;
  lastOverlaySourceHoverVec_ = sourceHoverElements_;
  lastOverlayRasterSize_ = currentOverlayRasterSize;
  lastOverlayScreenRect_ = currentOverlayScreenRect;
  lastOverlayCanvasFromDocument_ = currentOverlayCanvasFromDocument;
  lastOverlaySelectionDetail_ = resolvedSelectionDetail;
  lastOverlayInteractionActive_ = overlayInteractionActive;
  lastOverlayVersion_ = currentVersion;
  lastOverlayMarqueeRectDoc_ = marqueeRectDoc;
  lastOverlayActiveBoundsPreview_ = activeBoundsPreview;
  lastOverlayPenLivePreviewElement_ = penLivePreviewElement_;
  lastOverlayPenHoverPreviewSegmentDoc_ = penHoverPreviewSegmentDoc_;
  lastOverlayPenHoverCloseAffordanceDoc_ = penHoverCloseAffordanceDoc_;
  lastFrameCostBreakdown_.overlay = overlayCost;
  return true;
}

bool RenderCoordinator::rasterizeOverlayForPresentation(
    EditorApp& app, SelectTool& selectTool, const ViewportState& viewport, GlTextureCache& textures,
    std::optional<SelectTool::ActiveDragPreview> activeDragPreview,
    std::optional<SelectTool::ActiveDragPreview> representedDragPreview,
    std::optional<SelectionChromeDetail> selectionDetail, bool allowLiveGeometryOverlay) {
  ZoneScopedN("RenderCoordinator::rasterizeOverlayForPresentation");
  if (!app.hasDocument() || viewport.paneSize.x <= 0.0 || viewport.paneSize.y <= 0.0) {
    return false;
  }

  const std::optional<SelectTool::ActiveDragPreview> liveActiveDragPreview =
      selectTool.activeDragPreview();
  const std::optional<SelectTool::ActiveDragPreview> liveDragPreview =
      liveActiveDragPreview.has_value() ? liveActiveDragPreview : activeDragPreview;
  const std::optional<SelectTool::ActiveTransformBoundsPreview> activeBoundsPreview =
      selectTool.activeTransformBoundsPreview();
  const bool hasPresentationProjection = liveDragPreview.has_value() ||
                                         representedDragPreview.has_value() ||
                                         activeBoundsPreview.has_value();
  const std::uint64_t currentVersion = app.document().currentFrameVersion();
  // Non-transforming geometry edits usually have no presenter-side projection that can make live
  // chrome line up with older document pixels. Active PenTool drags are the exception: the selected
  // path is itself the live interaction surface, so the renderer-backed path chrome must track the
  // live path directly.
  if (displayedDocVersion_ != 0u && currentVersion > displayedDocVersion_ &&
      !hasPresentationProjection && !allowLiveGeometryOverlay) {
    if (immediateOverlaySnapshot_.has_value() && lastOverlayVersion_ > displayedDocVersion_) {
      immediateOverlaySnapshot_.reset();
    }

    FrameCostBreakdown::Overlay overlayCost;
    overlayCost.canvasSize = OverlayRasterSizeForViewport(viewport);
    overlayCost.selectedElementCount = static_cast<int>(app.selectedElements().size());
    overlayCost.sourceHoverElementCount = static_cast<int>(sourceHoverElements_.size());
    overlayCost.selectionBoundsOnly = selectionDetail.has_value() &&
                                      *selectionDetail == SelectionChromeDetail::CombinedBoundsOnly;
    lastFrameCostBreakdown_.overlay = overlayCost;
    return false;
  }

  return rasterizeOverlayForCurrentSelection(app, viewport, selectTool.marqueeRect(),
                                             representedDragPreview, activeBoundsPreview,
                                             selectionDetail, liveDragPreview);
}

void RenderCoordinator::pollRenderResult(EditorApp& app, const ViewportState& viewport,
                                         GlTextureCache& textures, FrameHistory* frameHistory) {
  ZoneScopedN("RenderCoordinator::pollRenderResult");
  auto resultOpt = renderWorker_.asyncRenderer.pollResult();
  if (!resultOpt.has_value()) {
    return;
  }

  const auto& result = *resultOpt;
  lastFrameCostBreakdown_.compositedRender =
      CompositedRenderCostFromStats(renderWorker_.asyncRenderer.compositorRenderFrameStats());
  // Forward the worker-measured presentation latency to the frame history so
  // `RenderFrameGraph` can overlay async worker time on the UI frame graph.
  // The frame history's latest slot corresponds to the current UI frame
  // (pushed at the top of `EditorShell::runFrame` before poll) — a landed
  // result belongs to that frame.
  if (frameHistory != nullptr) {
    frameHistory->setLatestBackendMs(static_cast<float>(result.workerMs));
  }
  const EditorRasterViewport rasterViewport = viewport.rasterViewport();
  const bool overviewInfillResult =
      result.overviewInfillOnly && result.compositedPreview.has_value() &&
      result.compositedPreview->valid() && !result.rasterViewport.viewportBounded;
  if (overviewInfillResult && rasterViewport.viewportBounded) {
    textures.uploadCompositedOverview(*result.compositedPreview, result.rasterViewport);
    lastFrameCostBreakdown_.compositedUpload = textures.lastCompositedUploadCost();
    pendingDocumentMutationOverviewRefresh_ = false;
    displayedDocVersion_ = result.version;
    return;
  }
  if (!RasterViewportCanPresentCurrentViewport(result.rasterViewport, rasterViewport)) {
    return;
  }
  if (result.rasterViewport.viewportBounded && rasterViewport.viewportBounded &&
      !textures.coverageDiagnostics().overviewInfillAvailable) {
    // A viewport-bounded result covers only the currently rasterized window. Never make it the
    // sole presented content: zooming out would expose checkerboard for missing tile coverage
    // instead of document transparency. Keep the previous presentation until an overview infill
    // exists underneath the crisp bounded tiles.
    return;
  }
  const Vector2i resultCanvasSize = result.rasterViewport.outputSizePx;
  if (result.compositedPreview.has_value() && result.compositedPreview->valid()) {
    if (!ShouldPresentCompositedPreviewForViewport(*result.compositedPreview, resultCanvasSize)) {
      return;
    }

    textures.uploadComposited(*result.compositedPreview, result.rasterViewport);
    lastFrameCostBreakdown_.compositedUpload = textures.lastCompositedUploadCost();
    if (!result.rasterViewport.viewportBounded) {
      pendingDocumentMutationOverviewRefresh_ = false;
    }
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
    compositedPresentation_.noteCachedTextures(
        result.compositedPreview->entity, result.version, resultCanvasSize,
        DragPreviewFromRenderRequest(result.compositedPreview->representedDragPreview));
    if (CompositedPreviewClearsPendingSelectedLayerRasterization(
            *result.compositedPreview, pendingSelectedLayerRasterizationEntity_, result.version,
            pendingSelectedLayerRasterizationVersion_)) {
      pendingSelectedLayerRasterizationEntity_ = entt::null;
      pendingSelectedLayerRasterizationVersion_ = 0;
    }
  }

  displayedDocVersion_ = result.version;
  renderScheduler_.noteRenderCompleted(result.version, resultCanvasSize, result.rasterViewport);
  if (result.compositedPreview.has_value() && result.compositedPreview->valid() &&
      compositedPresentation_.isWaitingForChromeRefresh() && app.hasDocument()) {
    refreshSelectionBoundsCache(app);
    // Preserve the last-known marquee rect across this internal
    // chrome-refresh path. Composited drag lands here; SelectTool isn't
    // reachable, so we reuse whatever the most recent main-thread
    // rasterize baked in.
    rasterizeOverlayForCurrentSelection(app, viewport, lastOverlayMarqueeRectDoc_, std::nullopt,
                                        std::nullopt, lastOverlaySelectionDetail_);
    compositedPresentation_.noteChromeRefreshCompleted(displayedDocVersion_);
  }
  promoteSelectionBoundsIfReady();
}

void RenderCoordinator::maybeRequestRender(EditorApp& app, SelectTool& selectTool,
                                           const ViewportState& viewport,
                                           GlTextureCache* textures) {
  ZoneScopedN("RenderCoordinator::maybeRequestRender");
  if (!app.hasDocument() || viewport.paneSize.x <= 0.0 || viewport.paneSize.y <= 0.0) {
    return;
  }

  invalidatePresentationAfterDocumentFlush(app.document().lastFlushResult());

  const EditorRasterViewport rasterViewport = viewport.rasterViewport();
  const Vector2i desiredCanvasSize = rasterViewport.semanticCanvasSizePx;
  const Vector2i actualDocumentCanvas = app.document().document().canvasSize();
  const auto dragPreview = selectTool.activeDragPreview();
  // Compare desired size against the live document size. Document replacement
  // can reset the stored canvas size, and LayoutSystem readback can round by a
  // pixel, so a separate last-set tracker is not authoritative here.
  const auto now = std::chrono::steady_clock::now();
  if (pendingCanvasSize_ != desiredCanvasSize) {
    pendingCanvasSize_ = desiredCanvasSize;
    pendingCanvasSizeSince_ = now;
  }
  if (!pendingRasterViewport_.has_value() ||
      !SameRasterViewport(*pendingRasterViewport_, rasterViewport)) {
    pendingRasterViewport_ = rasterViewport;
    pendingRasterViewportSince_ = now;
  }
  const bool throttleElapsed = (now - pendingCanvasSizeSince_) >= kCanvasSizeCommitDelay;
  const bool rasterViewportSettled = !IsUnsetTimePoint(pendingRasterViewportSince_) &&
                                     now - pendingRasterViewportSince_ >= kCanvasSizeCommitDelay;
  const bool firstCommit = actualDocumentCanvas == Vector2i::Zero();
  const bool closeEnough = std::abs(pendingCanvasSize_.x - actualDocumentCanvas.x) <= 1 &&
                           std::abs(pendingCanvasSize_.y - actualDocumentCanvas.y) <= 1;
  const bool wouldChange = !closeEnough;
  // During active drag the presenter can transform the existing promoted tile in lockstep with
  // the overlay. Committing a zoom-driven canvas size here invalidates the render tree and can
  // rerasterize every cached span before the next pointer frame; defer that crisp refresh until
  // mouse-up unless the document still needs its first canvas.
  const bool deferCanvasCommitForActiveDrag = dragPreview.has_value() && !firstCommit;
  const auto currentVersion = app.document().currentFrameVersion();
  const Entity prewarmEntity = selectedCompositedEntity(app);
  const PresentationCoverageDiagnostics coverageDiagnostics =
      textures != nullptr ? textures->coverageDiagnostics() : PresentationCoverageDiagnostics{};
  const bool needsOverviewInfill =
      rasterViewport.viewportBounded && !dragPreview.has_value() && textures != nullptr &&
      (!coverageDiagnostics.overviewInfillAvailable || pendingDocumentMutationOverviewRefresh_);
  const bool pendingSelectedLayerRasterization =
      prewarmEntity != entt::null && prewarmEntity == pendingSelectedLayerRasterizationEntity_;
  const bool deferSelectedViewportRefresh = ShouldDeferSelectedViewportRefresh(
      prewarmEntity, dragPreview.has_value(), currentVersion, displayedDocVersion_,
      compositedPresentation_.hasCachedTexturesForEntity(prewarmEntity), rasterViewportSettled,
      needsOverviewInfill, pendingSelectedLayerRasterization);
  if (deferSelectedViewportRefresh && !needsOverviewInfill) {
    if (renderWorker_.asyncRenderer.isBusy()) {
      renderWorker_.asyncRenderer.cancelInFlight();
    }
    return;
  }
  if (renderWorker_.asyncRenderer.isBusy()) {
    return;
  }

  const bool requestOverviewInfill = needsOverviewInfill;
  const std::vector<Entity> prewarmExtraEntities =
      requestOverviewInfill ? std::vector<Entity>{}
                            : selectedCompositedExtraEntities(app, prewarmEntity);
  const bool forceSelectedLayerRasterization = pendingSelectedLayerRasterization;
  const bool useSelectedPrewarmRasterViewport =
      !requestOverviewInfill && prewarmEntity != entt::null && !dragPreview.has_value() &&
      rasterViewport.viewportBounded;
  const EditorRasterViewport requestRasterViewport =
      requestOverviewInfill
          ? viewport.overviewInfillRasterViewport()
          : (useSelectedPrewarmRasterViewport ? viewport.selectedPrewarmRasterViewport()
                                              : rasterViewport);
  const Vector2i currentCanvasSize = requestRasterViewport.outputSizePx;

  if (pendingCanvasSize_ != Vector2i::Zero() && wouldChange && !deferCanvasCommitForActiveDrag &&
      (firstCommit || throttleElapsed)) {
    app.document().document().setCanvasSize(pendingCanvasSize_.x, pendingCanvasSize_.y);
    pendingCanvasSizeSince_ = now;
    ++lastFrameCostBreakdown_.documentCanvasCommitCount;
    lastFrameCostBreakdown_.lastCommittedCanvasSize = pendingCanvasSize_;
  }

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

  const PresentationRenderScheduleDecision schedule = renderScheduler_.evaluate(
      compositedPresentation_,
      PresentationRenderScheduleInput{
          .selectedEntity = requestOverviewInfill ? entt::null : prewarmEntity,
          .selectedExtraEntities = prewarmExtraEntities,
          .activeDragPreview = dragPreview,
          .forceSelectedLayerRasterization = forceSelectedLayerRasterization,
          .currentVersion = currentVersion,
          .currentCanvasSize = currentCanvasSize,
          .currentRasterViewport = requestRasterViewport,
      });
  if (!schedule.shouldRequestRender()) {
    return;
  }

  RenderRequest req(renderWorker_.renderer, app.document().document());
  req.version = currentVersion;
  req.documentGeneration = app.document().documentGeneration();
  req.rasterViewport = requestRasterViewport;
  req.overviewInfillOnly = requestOverviewInfill;
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
  if (!requestOverviewInfill && schedule.dragPreview.has_value()) {
    req.dragPreview = *schedule.dragPreview;
  }
  renderWorker_.asyncRenderer.requestRender(req);
}

void RenderCoordinator::invalidatePresentationAfterDocumentFlush(
    const AsyncSVGDocument::FlushResult& flushResult) {
  if (!flushResult.removedElements) {
    return;
  }

  pendingDocumentMutationOverviewRefresh_ = true;
}

void RenderCoordinator::invalidatePresentationAfterDocumentFlush(
    EditorApp& app, const AsyncSVGDocument::FlushResult& flushResult) {
  invalidatePresentationAfterDocumentFlush(flushResult);

  const Entity selectedEntity = selectedCompositedEntity(app);
  if (selectedEntity == entt::null) {
    return;
  }

  if (std::find(flushResult.cacheInvalidatedElements.begin(),
                flushResult.cacheInvalidatedElements.end(),
                selectedEntity) != flushResult.cacheInvalidatedElements.end()) {
    pendingSelectedLayerRasterizationEntity_ = selectedEntity;
    pendingSelectedLayerRasterizationVersion_ = app.document().currentFrameVersion();
    compositedPresentation_.discardCachedTexturesForEntity(selectedEntity);
  }
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

Entity RenderCoordinator::selectedCompositedEntityForDiagnostics(EditorApp& app) const {
  if (!app.hasDocument() || !app.selectedElement().has_value()) {
    return entt::null;
  }

  return app.document().document().withReadAccess(
      [this, &app](svg::DocumentReadAccess&) { return selectedCompositedEntity(app); });
}

std::vector<Entity> RenderCoordinator::selectedCompositedExtraEntities(EditorApp& app,
                                                                       Entity primaryEntity) const {
  std::vector<Entity> extras;
  if (primaryEntity == entt::null) {
    return extras;
  }

  for (const svg::SVGElement& selected : app.selectedElements()) {
    if (!selected.isa<svg::SVGGraphicsElement>() || IsDisplayNone(selected)) {
      continue;
    }

    const Entity entity = selected.unsafeEntityHandle().entity();
    if (entity == primaryEntity || std::ranges::find(extras, entity) != extras.end()) {
      continue;
    }

    extras.push_back(entity);
  }
  return extras;
}

Entity RenderCoordinator::suppressedCompositedLayerEntity(EditorApp& app) {
  const std::optional<svg::SVGElement> selected = SelectedGraphicsElement(app);
  if (!selected.has_value()) {
    if (displayNoneSuppressedLayerEntity_ != entt::null) {
      return displayNoneSuppressedLayerEntity_;
    }

    const CompositedPresentation::DiagnosticsSnapshot diagnostics =
        compositedPresentation_.diagnostics();
    if (diagnostics.hasCachedTextures && diagnostics.cachedEntity != entt::null &&
        !DocumentContainsEntity(app.document().document(), diagnostics.cachedEntity)) {
      displayNoneSuppressedSelectionEntity_ = entt::null;
      displayNoneSuppressedLayerEntity_ = diagnostics.cachedEntity;
      return diagnostics.cachedEntity;
    }

    return entt::null;
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
