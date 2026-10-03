#include "donner/editor/FramePresentation.h"

#include <algorithm>
#include <cmath>

#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/SelectionTransformHandles.h"

namespace donner::editor {
namespace {

bool HasObjectTile(std::span<const GlTextureCache::TileView> tiles, Entity entity) {
  return std::ranges::any_of(tiles, [&](const auto& tile) {
    return tile.kind == RenderResult::CompositedTile::Kind::Layer && tile.layerEntity == entity &&
           (tile.texture != 0 || tile.textureSnapshot != nullptr);
  });
}

bool SameSelection(std::span<const Entity> lhs, std::span<const Entity> rhs) {
  return std::ranges::equal(lhs, rhs);
}

bool ContainsSelection(std::span<const Entity> selection, Entity entity) {
  return std::ranges::find(selection, entity) != selection.end();
}

std::optional<PresentationPose> FindPose(std::span<const PresentationPose> poses, Entity entity) {
  const auto found =
      std::ranges::find_if(poses, [&](const auto& pose) { return pose.entity == entity; });
  return found != poses.end() ? std::optional<PresentationPose>(*found) : std::nullopt;
}

std::optional<Transform2d> PresentedDocumentFromCapturedDocument(const PresentationPose& source,
                                                                 const PresentationPose& target) {
  const std::array from{source};
  const std::array to{target};
  return ResolvePresentationTransform(from, to);
}

bool MovablePose(const CapturedPresentation& capture,
                 std::span<const GlTextureCache::TileView> tiles, const PresentationPose& target) {
  const auto source = capture.pose(target.entity);
  if (!source.has_value()) {
    return false;
  }
  if (SamePresentationTransform(source->documentFromElement, target.documentFromElement)) {
    return true;
  }
  return capture.canProject(target.entity) && HasObjectTile(tiles, target.entity) &&
         PresentedDocumentFromCapturedDocument(*source, target).has_value();
}

void AppendChrome(SelectionChromeSnapshot& destination, SelectionChromeSnapshot source) {
  const auto append = [](auto& output, auto& input) {
    output.insert(output.end(), std::make_move_iterator(input.begin()),
                  std::make_move_iterator(input.end()));
  };
  append(destination.paths, source.paths);
  append(destination.aabbsDoc, source.aabbsDoc);
  append(destination.pathAnchorPointsDoc, source.pathAnchorPointsDoc);
  append(destination.pathControlLinesDoc, source.pathControlLinesDoc);
  append(destination.pathControlPointsDoc, source.pathControlPointsDoc);
  append(destination.textBaselinesDoc, source.textBaselinesDoc);
  append(destination.clipGuidesDoc, source.clipGuidesDoc);
}

void PlaceTiles(std::vector<GlTextureCache::TileView>& tiles, const CapturedPresentation& capture,
                std::span<const PresentationPose> overrides) {
  for (auto& tile : tiles) {
    tile.canvasOffsetDoc += capture.documentOrigin();
    const auto target = FindPose(overrides, tile.layerEntity);
    const auto source = capture.pose(tile.layerEntity);
    if (source.has_value() && target.has_value()) {
      const auto presentedDocumentFromCapturedDocument =
          PresentedDocumentFromCapturedDocument(*source, *target);
      if (presentedDocumentFromCapturedDocument.has_value()) {
        tile.documentFromCachedDocument =
            tile.documentFromCachedDocument * *presentedDocumentFromCapturedDocument;
      }
    }
    tile.dragTranslationDoc = tile.documentFromCachedDocument.translation();
  }
}

bool CoversPane(const PresentationCoverageDiagnostics& coverage,
                const FramePresentationInput& input) {
  if (!coverage.activeTilesViewportBounded) {
    return true;
  }
  const auto visibleClip =
      PresentedImageClipRect(input.paneClipRect, input.viewport.imageScreenRect());
  if (!visibleClip.has_value()) {
    return true;
  }
  const Box2d visible = input.viewport.screenToDocument(*visibleClip);
  return coverage.activeRasterDocumentRect.contains(visible.topLeft) &&
         coverage.activeRasterDocumentRect.contains(visible.bottomRight);
}

std::optional<Box2d> IntersectFrameBoxes(const Box2d& a, const Box2d& b) {
  Box2d box(Vector2d(std::max(a.topLeft.x, b.topLeft.x), std::max(a.topLeft.y, b.topLeft.y)),
            Vector2d(std::min(a.bottomRight.x, b.bottomRight.x),
                     std::min(a.bottomRight.y, b.bottomRight.y)));
  return box.width() > 0.0 && box.height() > 0.0 ? std::optional<Box2d>(box) : std::nullopt;
}

bool RasterCoversProjection(const CapturedPresentation& capture,
                            const CapturedPresentation::Object& object,
                            const PresentationPose& target,
                            const std::vector<GlTextureCache::RasterCoverageGroup>& groups,
                            const FramePresentationInput& input) {
  const auto source = capture.pose(object.entity);
  const auto presentedDocumentFromCapturedDocument =
      source.has_value() ? PresentedDocumentFromCapturedDocument(*source, target) : std::nullopt;
  if (!presentedDocumentFromCapturedDocument.has_value()) {
    return false;
  }
  if (SamePresentationTransform(*presentedDocumentFromCapturedDocument, Transform2d()) ||
      std::abs(presentedDocumentFromCapturedDocument->determinant()) < 1e-12) {
    return true;
  }
  const auto visibleClip =
      PresentedImageClipRect(input.paneClipRect, input.viewport.imageScreenRect());
  if (!visibleClip.has_value()) {
    return true;
  }
  const Box2d visible = input.viewport.screenToDocument(*visibleClip);
  for (const auto& group : groups) {
    const Transform2d presentedFromRaster =
        group.documentFromRaster * *presentedDocumentFromCapturedDocument;
    const Box2d visibleInRaster = presentedFromRaster.inverse().transformBox(visible);
    const auto& bounds = group.paintBounds;
    const auto& coverage = group.tileBounds;
    const bool complete =
        !bounds.empty() && std::ranges::all_of(bounds, [&](const Box2d& paintBounds) {
          const auto required = IntersectFrameBoxes(visibleInRaster, paintBounds);
          return !required.has_value() ||
                 SubtractPresentedTileBoundsFromClip(*required, coverage).empty();
        });
    if (complete) {
      return true;
    }
  }
  return false;
}

bool PreserveOverrides(const CapturedPresentation& capture,
                       std::span<const GlTextureCache::TileView> tiles,
                       const FramePresentation* previous,
                       std::vector<PresentationPose>& overrides) {
  if (previous == nullptr || !capture.identity().sameContent(previous->identity())) {
    return true;
  }
  for (const auto& held : previous->overrides()) {
    if (capture.absent(held.entity)) {
      continue;
    }
    if (!MovablePose(capture, tiles, held)) {
      return false;
    }
    const auto source = capture.pose(held.entity);
    if (!SamePresentationTransform(source->documentFromElement, held.documentFromElement)) {
      overrides.push_back(held);
    }
  }
  return true;
}

void SetOverride(std::vector<PresentationPose>& overrides, const PresentationPose& target) {
  const auto found = std::ranges::find_if(
      overrides, [&](const auto& pose) { return pose.entity == target.entity; });
  if (found != overrides.end()) {
    *found = target;
  } else {
    overrides.push_back(target);
  }
}

std::optional<Box2d> GestureReferenceBounds(const CapturedPresentation& capture,
                                            const SelectTool::ActiveDragPreview& desired) {
  std::optional<Box2d> bounds;
  for (const auto& object : capture.objects()) {
    if (!object.pathBoundsCoverFrame) {
      return std::nullopt;
    }
    const auto captured = capture.pose(object.entity);
    const auto start = FindPose(desired.startPoses, object.entity);
    if (!captured.has_value() || !start.has_value()) {
      return std::nullopt;
    }
    const auto startFromCaptured = PresentedDocumentFromCapturedDocument(*captured, *start);
    if (!startFromCaptured.has_value()) {
      return std::nullopt;
    }
    for (const auto& item : object.chrome.paths) {
      const Box2d box = item.pathDoc.transformed(*startFromCaptured).bounds();
      if (bounds.has_value()) {
        bounds->addBox(box);
      } else {
        bounds = box;
      }
    }
  }
  return bounds;
}

void ClearSelectionGeometry(SelectionChromeSnapshot& chrome) {
  chrome.paths.clear();
  chrome.clipGuidesDoc.clear();
  chrome.aabbsDoc.clear();
  chrome.orientedBoundsDoc.reset();
  chrome.handleAnchorsDoc.clear();
  chrome.pathAnchorPointsDoc.clear();
  chrome.pathControlLinesDoc.clear();
  chrome.pathControlPointsDoc.clear();
  chrome.textBaselinesDoc.clear();
  chrome.livePathPreview.reset();
}

void CopyTextDecorations(SelectionChromeSnapshot& destination,
                         const SelectionChromeSnapshot& source) {
  destination.textCaretDoc = source.textCaretDoc;
  destination.textSelectionQuadsDoc = source.textSelectionQuadsDoc;
  destination.textFrameCornersDoc = source.textFrameCornersDoc;
  destination.textFrameOpacity = source.textFrameOpacity;
}

}  // namespace

Transform2d PresentedFramebufferFromDocumentTransform(const ViewportState& viewport,
                                                      const Vector2d& framebufferFromLogicalScale) {
  const Vector2d framebufferPixelsPerDocUnit =
      framebufferFromLogicalScale * viewport.pixelsPerDocUnit();
  const Vector2d framebufferOriginFromDocumentOrigin =
      viewport.panScreenPoint * framebufferFromLogicalScale -
      viewport.panDocPoint * framebufferPixelsPerDocUnit;

  Transform2d framebufferFromDocument(Transform2d::uninitialized);
  framebufferFromDocument.data[0] = framebufferPixelsPerDocUnit.x;
  framebufferFromDocument.data[1] = 0.0;
  framebufferFromDocument.data[2] = 0.0;
  framebufferFromDocument.data[3] = framebufferPixelsPerDocUnit.y;
  framebufferFromDocument.data[4] = framebufferOriginFromDocumentOrigin.x;
  framebufferFromDocument.data[5] = framebufferOriginFromDocumentOrigin.y;
  return framebufferFromDocument;
}

namespace {
bool FiniteFramePoint(const Vector2d& point) {
  return std::isfinite(point.x) && std::isfinite(point.y);
}
}  // namespace

std::optional<Box2d> PresentedImageClipRect(const Box2d& paneRect, const Box2d& imageRect) {
  if (!FiniteFramePoint(paneRect.topLeft) || !FiniteFramePoint(paneRect.bottomRight) ||
      !FiniteFramePoint(imageRect.topLeft) || !FiniteFramePoint(imageRect.bottomRight)) {
    return std::nullopt;
  }

  const Box2d clipRect(Vector2d(std::max(paneRect.topLeft.x, imageRect.topLeft.x),
                                std::max(paneRect.topLeft.y, imageRect.topLeft.y)),
                       Vector2d(std::min(paneRect.bottomRight.x, imageRect.bottomRight.x),
                                std::min(paneRect.bottomRight.y, imageRect.bottomRight.y)));
  if (clipRect.bottomRight.x <= clipRect.topLeft.x ||
      clipRect.bottomRight.y <= clipRect.topLeft.y) {
    return std::nullopt;
  }

  return clipRect;
}

namespace {
bool CandidatePreservesPose(const CapturedPresentation& capture,
                            std::span<const RenderResult::CompositedTile> tiles,
                            const PresentationPose& held) {
  const auto source = capture.pose(held.entity);
  if (!source.has_value()) {
    return false;
  }
  if (SamePresentationTransform(source->documentFromElement, held.documentFromElement)) {
    return true;
  }
  const bool ownsPixels = std::ranges::any_of(tiles, [&](const auto& tile) {
    return tile.kind == RenderResult::CompositedTile::Kind::Layer &&
           tile.layerEntity == held.entity;
  });
  if (!ownsPixels || !capture.canProject(held.entity) ||
      !PresentedDocumentFromCapturedDocument(*source, held).has_value()) {
    return false;
  }
  return true;
}
}  // namespace

bool FramePresentation::CanAdopt(const CapturedPresentation& capture,
                                 std::span<const RenderResult::CompositedTile> tiles,
                                 const FramePresentation* previous, bool latestCommittedScene) {
  if (previous != nullptr &&
      capture.identity().documentGeneration == previous->identity().documentGeneration &&
      (capture.identity().documentRevision < previous->identity().documentRevision ||
       capture.identity().version < previous->identity().version)) {
    return false;
  }
  if (latestCommittedScene || previous == nullptr ||
      !capture.identity().sameContent(previous->identity())) {
    return true;
  }
  for (const auto& held : previous->overrides()) {
    if (capture.absent(held.entity)) {
      continue;
    }
    if (!CandidatePreservesPose(capture, tiles, held)) {
      return false;
    }
  }
  return true;
}

namespace {
bool ValidFrameInput(const FramePresentationInput& input) {
  return input.frameId != 0 && std::isfinite(input.viewport.zoom) && input.viewport.zoom > 0.0 &&
         std::isfinite(input.viewport.devicePixelRatio) && input.viewport.devicePixelRatio > 0.0 &&
         FinitePresentationTransform(
             PresentedFramebufferFromDocumentTransform(input.viewport, Vector2d(1.0, 1.0)));
}

bool CompatibleOverview(const GlTextureCache::PresentationResources& resources) {
  const auto overview = resources.overviewCapture();
  const auto active = resources.capture();
  if (overview == nullptr || !active->identity().sameContent(overview->identity()) ||
      active->canvasSize() != overview->canvasSize() ||
      active->documentOrigin() != overview->documentOrigin()) {
    return false;
  }
  return active->identity().sameScene(overview->identity());
}

std::optional<bool> ChooseOverview(
    const std::shared_ptr<const GlTextureCache::PresentationResources>& resources,
    const FramePresentationInput& input) {
  const bool needsOverview =
      resources->tiles().empty() || !CoversPane(resources->coverage(), input);
  const bool compatibleOverview = CompatibleOverview(*resources);
  if (needsOverview && !compatibleOverview &&
      (!resources->tiles().empty() || !resources->overviewTiles().empty())) {
    return std::nullopt;
  }
  const bool useOverview = needsOverview && compatibleOverview;
  return useOverview;
}
}  // namespace

std::shared_ptr<const CapturedPresentation> FramePresentation::CaptureForFrame(
    const std::shared_ptr<const GlTextureCache::PresentationResources>& resources,
    const FramePresentationInput& input) {
  if (resources == nullptr || resources->capture() == nullptr || !ValidFrameInput(input)) {
    return nullptr;
  }
  const auto useOverview = ChooseOverview(resources, input);
  if (!useOverview.has_value()) {
    return nullptr;
  }
  return *useOverview ? resources->overviewCapture() : resources->capture();
}

namespace {
FramePresentationFailure ValidateFrameResources(
    const std::shared_ptr<const GlTextureCache::PresentationResources>& resources,
    const FramePresentationInput& input) {
  if (resources == nullptr || resources->capture() == nullptr) {
    return FramePresentationFailure::MissingResources;
  }
  if (!ValidFrameInput(input)) {
    return FramePresentationFailure::InvalidInput;
  }
  return FramePresentationFailure::None;
}
std::shared_ptr<const CapturedPresentation> SelectionCaptureForRaster(
    std::shared_ptr<const CapturedPresentation> selection,
    const std::shared_ptr<const CapturedPresentation>& raster) {
  return selection != nullptr && selection->identity() == raster->identity() &&
                 selection->canvasSize() == raster->canvasSize()
             ? std::move(selection)
             : raster;
}
bool IsCurrentCommittedFrame(const CapturedPresentation& capture,
                             const FramePresentationInput& input) {
  return capture.identity().sameScene(input.documentIdentity) && !input.pendingDocumentMutations &&
         !input.desired;
}
FramePresentationBuildResult FinishFrameResult(std::shared_ptr<const FramePresentation> frame,
                                               const FramePresentationInput& input) {
  const auto failure = !input.selection.empty() && !frame->hasSelectionGeometry()
                           ? FramePresentationFailure::MissingSelectionGeometry
                           : FramePresentationFailure::None;
  return {.frame = std::move(frame), .failure = failure};
}
}  // namespace

FramePresentationBuildResult FramePresentation::Build(
    std::shared_ptr<const GlTextureCache::PresentationResources> resources,
    const FramePresentationInput& input,
    std::shared_ptr<const CapturedPresentation> selectionCapture,
    std::shared_ptr<const FramePresentation> previous) {
  const auto validation = ValidateFrameResources(resources, input);
  if (validation != FramePresentationFailure::None) {
    return {.failure = validation};
  }
  const auto useOverview = ChooseOverview(resources, input);
  if (!useOverview.has_value()) {
    return {.failure = FramePresentationFailure::MissingOverview};
  }
  const auto rasterCapture = *useOverview ? resources->overviewCapture() : resources->capture();
  selectionCapture = SelectionCaptureForRaster(std::move(selectionCapture), rasterCapture);
  auto frame = std::shared_ptr<FramePresentation>(new FramePresentation());
  frame->resources_ = std::move(resources);
  frame->rasterCapture_ = rasterCapture;
  frame->selectionCapture_ = std::move(selectionCapture);
  frame->initialize(input, *useOverview);
  const bool currentCommitted = IsCurrentCommittedFrame(*rasterCapture, input);
  if (!frame->retainDisplayedPoses(currentCommitted ? nullptr : previous.get())) {
    return {.failure = FramePresentationFailure::IncompatiblePose};
  }
  frame->applyPointerIntent(input);
  if (!frame->chosenPosesHaveCoverage(input)) {
    return {.failure = FramePresentationFailure::InsufficientCoverage};
  }
  frame->projectSelectionGeometry();
  frame->resolveSelectionBounds(input);
  frame->resolveTiles(input);
  frame->finishChrome(input, previous.get());
  return FinishFrameResult(std::move(frame), input);
}

void FramePresentation::initialize(const FramePresentationInput& input, bool useOverview) {
  identity_ = rasterCapture_->identity();
  frameId_ = input.frameId;
  viewport_ = input.viewport;
  paneClipRect_ = input.paneClipRect;
  documentClipRect_ = PresentedImageClipRect(input.paneClipRect, input.viewport.imageScreenRect());
  screenFromDocument_ = PresentedFramebufferFromDocumentTransform(input.viewport, Vector2d(1, 1));
  chromeEnabled_ = input.includeChrome;
  useOverview_ = useOverview;
  tiles_ = useOverview ? resources_->overviewTiles() : resources_->tiles();
}

bool FramePresentation::retainDisplayedPoses(const FramePresentation* previous) {
  if (!PreserveOverrides(*rasterCapture_, tiles_, previous, overrides_)) {
    return false;
  }

  return true;
}

bool FramePresentation::chosenPosesHaveCoverage(const FramePresentationInput& input) const {
  for (const auto& held : overrides_) {
    const auto object = std::ranges::find_if(
        selectionCapture_->objects(),
        [&](const auto& candidate) { return candidate.entity == held.entity; });
    const CapturedPresentation::Object fallback{
        .entity = held.entity, .paintBoundsDoc = rasterCapture_->paintBounds(held.entity)};
    const auto& paint = object != selectionCapture_->objects().end() ? *object : fallback;
    if (!RasterCoversProjection(*rasterCapture_, paint, held,
                                resources_->objectCoverage(held.entity, useOverview_), input)) {
      return false;
    }
  }
  return true;
}

void FramePresentation::applyPointerIntent(const FramePresentationInput& input) {
  hasSelectionGeometry_ =
      identity_.documentGeneration == input.documentIdentity.documentGeneration &&
      SameSelection(selectionCapture_->selection(), input.selection);
  if (hasSelectionGeometry_ && input.desired.has_value() &&
      identity_.sameContent(input.desired->contentIdentity) &&
      selectionCapture_->affinePreviewAllowed() &&
      input.desired->poses.size() == input.selection.size()) {
    followsPointer_ = std::ranges::all_of(selectionCapture_->objects(), [&](const auto& object) {
      const auto target = FindPose(input.desired->poses, object.entity);
      return target.has_value() && MovablePose(*selectionCapture_, tiles_, *target) &&
             RasterCoversProjection(*selectionCapture_, object, *target,
                                    resources_->objectCoverage(object.entity, useOverview_), input);
    });
    if (followsPointer_) {
      for (const auto& pose : input.desired->poses) {
        SetOverride(overrides_, pose);
      }
    }
  }
}

void FramePresentation::projectSelectionGeometry() {
  for (const auto& object : selectionCapture_->objects()) {
    const auto source = selectionCapture_->pose(object.entity);
    const auto chosen = FindPose(overrides_, object.entity);
    auto geometry = object.chrome;
    if (source.has_value()) {
      poses_.push_back(chosen.value_or(*source));
      if (chosen.has_value()) {
        const auto presentedDocumentFromCapturedDocument =
            PresentedDocumentFromCapturedDocument(*source, *chosen);
        if (presentedDocumentFromCapturedDocument.has_value()) {
          geometry = OverlayRenderer::projectSelectionSnapshot(
              std::move(geometry), *presentedDocumentFromCapturedDocument);
        }
      }
    }
    AppendChrome(chrome_, std::move(geometry));
  }
}

void FramePresentation::resolveSelectionBounds(const FramePresentationInput& input) {
  if (!chrome_.aabbsDoc.empty()) {
    auto corners = TransformedBoxCorners(CombinedSelectionBounds(chrome_.aabbsDoc), Transform2d());
    if (followsPointer_) {
      const auto bounds = GestureReferenceBounds(*selectionCapture_, *input.desired);
      const auto presentedFromStart =
          ResolvePresentationTransform(input.desired->startPoses, input.desired->poses);
      if (bounds.has_value() && presentedFromStart.has_value()) {
        corners = TransformedBoxCorners(*bounds, *presentedFromStart);
      }
    }
    chrome_.orientedBoundsDoc = SelectionChromeSnapshot::OrientedBox{.cornersDoc = corners};
    chrome_.handleAnchorsDoc.assign(corners.begin(), corners.end());
  }
}

void FramePresentation::resolveTiles(const FramePresentationInput& input) {
  // Unchanged captures need no retained override; future gestures retain the actual captured pose.
  std::erase_if(overrides_, [&](const auto& pose) {
    const auto source = rasterCapture_->pose(pose.entity);
    return source.has_value() &&
           SamePresentationTransform(source->documentFromElement, pose.documentFromElement);
  });
  PlaceTiles(tiles_, *rasterCapture_, overrides_);
  for (auto& tile : tiles_) {
    tile.isDragTarget =
        input.desired.has_value() && ContainsSelection(input.selection, tile.layerEntity);
  }
  std::erase_if(tiles_, [&](const auto& tile) {
    return tile.layerEntity != entt::null &&
           (tile.layerEntity == input.suppressedLayerEntity ||
            (input.suppressSelectionPixels &&
             ContainsSelection(input.selection, tile.layerEntity)));
  });
}

void FramePresentation::finishChrome(const FramePresentationInput& input,
                                     const FramePresentation* previous) {
  if (!hasSelectionGeometry_) {
    ClearSelectionGeometry(chrome_);
  }
  applyLivePathReplacement(input);
  applyCapturedAdornments(input);
  selectionBounds_ = chrome_.aabbsDoc;
  applyDetail(input.detail);
  applyDecorations(input, previous);
  chrome_.devicePixelRatio = input.viewport.devicePixelRatio;
  const double pixelsPerDocUnit = std::max(std::abs(input.viewport.pixelsPerDocUnit()), 1e-9);
  OverlayRenderer::cullSnapshot(
      chrome_,
      input.viewport.screenToDocument(input.paneClipRect).inflatedBy(64.0 / pixelsPerDocUnit));
}

void FramePresentation::applyLivePathReplacement(const FramePresentationInput& input) {
  const auto& replacement = input.livePathReplacement;
  if (replacement == nullptr || replacement->objects().size() != 1 ||
      replacement->selection() != input.selection ||
      replacement->identity().documentGeneration != identity_.documentGeneration ||
      replacement->identity().presentationEpoch != identity_.presentationEpoch) {
    return;
  }
  const auto& object = replacement->objects().front();
  if (!object.chrome.livePathPreview.has_value() ||
      !resources_->capture()->canProject(object.entity) ||
      !replacement->canProject(object.entity) || !HasObjectTile(tiles_, object.entity)) {
    return;
  }
  livePathCapture_ = replacement;
  replacementPaint_.emplace();
  replacementPaint_->livePathPreview = object.chrome.livePathPreview;
  chrome_ = object.chrome;
  chrome_.livePathPreview.reset();
  poses_ = replacement->poses();
  hasSelectionGeometry_ = true;
  followsPointer_ = false;
  std::erase_if(overrides_, [&](const auto& pose) { return pose.entity == object.entity; });
}

std::optional<SelectionChromeSnapshot> FramePresentation::liveAdornment(
    const CapturedPresentation::Adornment& item) const {
  if (livePathCapture_ == nullptr || item.subject != livePathCapture_->objects().front().entity) {
    return std::nullopt;
  }
  auto chrome = item.chrome;
  const auto& replacement = livePathCapture_->objects().front().chrome;
  chrome.hoverPaths = replacement.paths;
  chrome.hoverAabbsDoc = replacement.aabbsDoc;
  if (chrome.lockedFlash.has_value()) {
    chrome.lockedFlash->pathDoc = replacement.livePathPreview->pathDoc;
  }
  return chrome;
}

SelectionChromeSnapshot FramePresentation::projectAdornment(
    const CapturedPresentation::Adornment& item) const {
  if (auto live = liveAdornment(item)) {
    return std::move(*live);
  }
  auto chrome = item.chrome;
  const auto source = selectionCapture_->pose(item.owner);
  const auto target = FindPose(overrides_, item.owner);
  if (source.has_value() && target.has_value()) {
    if (const auto presentedDocumentFromCapturedDocument =
            PresentedDocumentFromCapturedDocument(*source, *target)) {
      for (auto& path : chrome.hoverPaths) {
        path.pathDoc = path.pathDoc.transformed(*presentedDocumentFromCapturedDocument);
      }
      for (auto& box : chrome.hoverAabbsDoc) {
        box = presentedDocumentFromCapturedDocument->transformBox(box);
      }
      if (chrome.lockedFlash.has_value()) {
        chrome.lockedFlash->pathDoc =
            chrome.lockedFlash->pathDoc.transformed(*presentedDocumentFromCapturedDocument);
      }
    }
  }
  return chrome;
}

void FramePresentation::applyCapturedAdornments(const FramePresentationInput& input) {
  for (const auto& item : selectionCapture_->adornments()) {
    if ((item.lockedFlash && item.subject != input.decorations.lockedFlashEntity) ||
        (!item.lockedFlash && !ContainsSelection(input.decorations.sourceHover, item.subject))) {
      continue;
    }
    auto chrome = projectAdornment(item);
    if (item.lockedFlash) {
      chrome_.lockedFlash = chrome.lockedFlash;
      if (chrome_.lockedFlash.has_value()) {
        chrome_.lockedFlash->intensity = input.decorations.lockedFlashIntensity;
      }
    } else {
      chrome_.hoverPaths.insert(chrome_.hoverPaths.end(),
                                std::make_move_iterator(chrome.hoverPaths.begin()),
                                std::make_move_iterator(chrome.hoverPaths.end()));
      chrome_.hoverAabbsDoc.insert(chrome_.hoverAabbsDoc.end(), chrome.hoverAabbsDoc.begin(),
                                   chrome.hoverAabbsDoc.end());
    }
  }
}

void FramePresentation::applyDetail(SelectionChromeDetail detail) {
  if (detail != SelectionChromeDetail::Complete &&
      detail != SelectionChromeDetail::PathOutlinesOnly) {
    chrome_.pathAnchorPointsDoc.clear();
    chrome_.pathControlLinesDoc.clear();
    chrome_.pathControlPointsDoc.clear();
  }
  if (detail == SelectionChromeDetail::EditingChromeOnly) {
    auto baselines = std::move(chrome_.textBaselinesDoc);
    ClearSelectionGeometry(chrome_);
    chrome_.textBaselinesDoc = std::move(baselines);
  } else if (detail == SelectionChromeDetail::PathOutlinesOnly) {
    chrome_.aabbsDoc.clear();
    chrome_.handleAnchorsDoc.clear();
    chrome_.orientedBoundsDoc.reset();
  } else if (detail == SelectionChromeDetail::CombinedBoundsOnly) {
    chrome_.paths.clear();
    if (!chrome_.aabbsDoc.empty()) {
      chrome_.aabbsDoc = {CombinedSelectionBounds(chrome_.aabbsDoc)};
    }
  }
}

bool FramePresentation::textMatchesFrame(const CapturedPresentation::TextEditing& text,
                                         std::span<const Entity> selection) const {
  return text.identity.sameScene(identity_) && text.canvasSize == selectionCapture_->canvasSize() &&
         ContainsSelection(selection, text.subject);
}

void FramePresentation::applyDecorations(const FramePresentationInput& input,
                                         const FramePresentation* previous) {
  if (!input.includeChrome) {
    return;
  }
  chrome_.marqueeDoc = input.decorations.marqueeDoc;
  chrome_.penPreviewSegmentDoc = input.decorations.penPreviewSegmentDoc;
  chrome_.penCloseAffordanceDoc = input.decorations.penCloseAffordanceDoc;
  chrome_.textBoxDragPreviewDoc = input.decorations.textBoxDragPreviewDoc;
  if (input.detail != SelectionChromeDetail::EditingChromeOnly) {
    return;
  }
  const auto& text = selectionCapture_->textEditing();
  if (text.has_value() && ContainsSelection(input.selection, text->subject)) {
    chrome_.textCaretDoc = text->caret;
    chrome_.textSelectionQuadsDoc = text->selection;
    chrome_.textFrameCornersDoc = text->frame;
    chrome_.textFrameOpacity = text->frameOpacity;
  } else if (previous != nullptr && previous->identity_ == identity_ &&
             SameSelection(previous->selectionCapture_->selection(),
                           selectionCapture_->selection())) {
    CopyTextDecorations(chrome_, previous->chrome_);
  }
  const auto& currentText = input.decorations.textEditing;
  if (currentText.has_value() && textMatchesFrame(*currentText, input.selection)) {
    chrome_.textCaretDoc = currentText->caret;
    chrome_.textSelectionQuadsDoc = currentText->selection;
    chrome_.textFrameCornersDoc = currentText->frame;
    chrome_.textFrameOpacity = currentText->frameOpacity;
  }
}

}  // namespace donner::editor
