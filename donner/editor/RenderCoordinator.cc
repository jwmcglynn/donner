#include "donner/editor/RenderCoordinator.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>

#include "donner/base/MemoryAttribution.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

#include "donner/editor/EditorApp.h"
#include "donner/editor/RenderPanePresenter.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/core/Display.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {

namespace {

bool IsCurrentRenderResult(const std::optional<RenderResult>& result, const EditorApp& app) {
  if (!result || !app.hasDocument()) {
    return false;
  }
  return result->documentGeneration == app.document().documentGeneration() &&
         result->fontResourceRevision == app.document().fontResourceRevision();
}

#ifdef __EMSCRIPTEN__
/// Stable wire name for a published GPU wait site. These strings are what the
/// browser test harness prints, so a failing run names the wait that hung
/// instead of an opaque number.
const char* GpuWaitTimeoutSiteName(svg::GpuWaitTimeoutSite site) {
  switch (site) {
    case svg::GpuWaitTimeoutSite::None: return "none";
    case svg::GpuWaitTimeoutSite::ReadbackMap: return "readback-map";
    case svg::GpuWaitTimeoutSite::QueueIdle: return "queue-idle";
    case svg::GpuWaitTimeoutSite::Present: return "present";
    case svg::GpuWaitTimeoutSite::Unknown: return "unknown";
  }
  return "unknown";
}

// Proxied to the browser main thread: the render pthread has no `window`.
// Numeric values ride an owned heap buffer because EM_ASM argument substitution stops at $15.
// Each asynchronous proxy owns its buffer until the main thread copies and frees it, so a later
// result cannot overwrite an earlier one's accounting. The wait-site name points to a string
// literal whose storage outlives the proxy hop.
void PublishWorkerTimingStats(
    const RenderResult& result, const EditorApp& app,
    const svg::compositor::CompositorController::RenderFrameStats& compositorStats) {
  const auto& timing = result.workerTiming;
  constexpr std::size_t kValueCount = 38;
  const double values[kValueCount] = {
      result.workerMs,
      timing.queueWaitMs,
      timing.dequeueToStartMs,
      timing.setupMs,
      timing.renderFrameMs,
      timing.buildPreviewMs,
      timing.finalSnapshotMs,
      timing.diagnosticsMs,
      timing.pollDelayMs,
      timing.wakeToPollMs,
      compositorStats.firstFrameDrawMs,
      compositorStats.firstFramePlanningMs,
      compositorStats.firstFrameWarmupMs,
      compositorStats.immediateRasterizeMs,
      compositorStats.cachedRasterizeMs,
      static_cast<double>(compositorStats.immediateTileCount),
      static_cast<double>(compositorStats.cachedTileCount),
      static_cast<double>(compositorStats.offscreenCreateCount),
      static_cast<double>(compositorStats.offscreenRecycleCount),
      static_cast<double>(compositorStats.offscreenCreateTotal),
      static_cast<double>(compositorStats.offscreenRecycleTotal),
      static_cast<double>(timing.readbackCount),
      static_cast<double>(timing.readbackPollIterations),
      timing.usedTimedWaitAny ? 1.0 : 0.0,
      timing.deviceLost ? 1.0 : 0.0,
      static_cast<double>(timing.timedOutWaitMs),
      static_cast<double>(result.documentGeneration),
      static_cast<double>(result.version),
      static_cast<double>(result.fontResourceRevision),
      static_cast<double>(app.document().document().sourceVersion()),
      static_cast<double>(app.undoTimeline().entryCount()),
      static_cast<double>(timing.fullCanvasTextureAllocationFailureCount),
      timing.nothingToPresent ? 1.0 : 0.0,
      timing.documentWriteLockMs,
      static_cast<double>(result.presentationRepairReason),
      static_cast<double>(timing.compositorReadbackCount),
      static_cast<double>(timing.tileHandoffReadbackCount),
      static_cast<double>(timing.finalSnapshotReadbackCount)};
  double* buffer = static_cast<double*>(std::malloc(sizeof(values)));
  if (buffer == nullptr) {
    return;
  }
  std::memcpy(buffer, values, sizeof(values));
  // clang-format off: EM_JS and EM_ASM bodies are JavaScript, which clang-format rewrites
  // as C++ - it has already split a `===` into `== =` elsewhere in the editor, a SyntaxError
  // the browser reports only once that arm is built.
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        let heap;
        try {
          const start = $0 >> 3;
          heap = HEAPF64.slice(start, start + $2);
        } finally {
          _free($0);
        }
        const b = 0;
        const names = ([
          'workerMs',
          'queueWaitMs',
          'dequeueToStartMs',
          'setupMs',
          'renderFrameMs',
          'buildPreviewMs',
          'finalSnapshotMs',
          'diagnosticsMs',
          'pollDelayMs',
          'wakeToPollMs',
          'firstFrameDrawMs',
          'firstFramePlanningMs',
          'firstFrameWarmupMs',
          'immediateRasterizeMs',
          'cachedRasterizeMs',
          'immediateTileCount',
          'cachedTileCount',
          'offscreenCreateCount',
          'offscreenRecycleCount',
          'offscreenCreateTotal',
          'offscreenRecycleTotal',
          'readbackCount',
          'readbackPollIterations'
        ]);
        const previous = window['__donnerWorkerStats'];
        const stats = ({
          'completedResults' : previous ? previous['completedResults'] + 1 : 1,
          'publishedAtMs' : performance.now(),
          'acceptedForPresentation' : false,
        });
        for (let index = 0; index < names.length; ++index) {
          stats[names[index]] = heap[b + index];
        }
        stats['readbackWaitStrategy'] = heap[b + 23] ? 'timed-wait-any' : 'device-poll';
        stats['deviceLost'] = heap[b + 24] > 0;
        stats['gpuWaitTimeoutSite'] = UTF8ToString($1);
        stats['gpuWaitTimeoutMs'] = heap[b + 25];
        stats['documentGeneration'] = heap[b + 26];
        stats['frameVersion'] = heap[b + 27];
        stats['fontResourceRevision'] = heap[b + 28];
        stats['sourceVersion'] = heap[b + 29];
        stats['undoEntryCount'] = heap[b + 30];
        stats['fullCanvasTextureAllocationFailureCount'] = heap[b + 31];
        stats['nothingToPresent'] = heap[b + 32] > 0;
        stats['documentWriteLockMs'] = heap[b + 33];
        stats['presentationRepairReason'] = heap[b + 34];
        stats['compositorReadbackCount'] = heap[b + 35];
        stats['tileHandoffReadbackCount'] = heap[b + 36];
        stats['finalSnapshotReadbackCount'] = heap[b + 37];
        stats['compositorReadbackTotal'] =
            (previous ? previous['compositorReadbackTotal'] || 0 : 0) +
            stats['compositorReadbackCount'];
        stats['tileHandoffReadbackTotal'] =
            (previous ? previous['tileHandoffReadbackTotal'] || 0 : 0) +
            stats['tileHandoffReadbackCount'];
        stats['finalSnapshotReadbackTotal'] =
            (previous ? previous['finalSnapshotReadbackTotal'] || 0 : 0) +
            stats['finalSnapshotReadbackCount'];
        stats['nothingToPresentTotal'] = (previous ? previous['nothingToPresentTotal'] || 0 : 0) +
                                         (stats['nothingToPresent'] ? 1 : 0);
        stats['publishReason'] = 'render-result';
        window['__donnerWorkerStats'] = stats;
      },
      buffer, GpuWaitTimeoutSiteName(timing.timedOutWaitSite), kValueCount);
  // clang-format on
}

/// Marks only the matching result whose textures passed the presentation admission gates.
void PublishAcceptedWorkerResult(const RenderResult& result) {
  // clang-format off
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        const stats = window['__donnerWorkerStats'];
        if (stats && Object.is(stats['documentGeneration'], $0) &&
            Object.is(stats['frameVersion'], $1) && Object.is(stats['fontResourceRevision'], $2)) {
          stats['acceptedForPresentation'] = true;
        }
      },
      static_cast<double>(result.documentGeneration), static_cast<double>(result.version),
      static_cast<double>(result.fontResourceRevision));
  // clang-format on
}

// Publish a GPU-wait failure that produced no frame.
//
// A bounded GPU wait that burns its deadline declares the device lost and
// ends the worker iteration with nothing to present, so the stats object
// above is never written and the failure reads as silence to anything polling
// the page. Merge the failure onto whatever was last published, creating the
// object when nothing ever completed, and leave `completedResults` alone: that
// counter orders presentation samples and must keep counting frames only.
void PublishWorkerGpuWaitFailure(bool deviceLost, const char* timedOutWaitSiteName,
                                 int timedOutWaitMs) {
  // clang-format off
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        const previous = window['__donnerWorkerStats'];
        const stats = previous ? Object.assign({}, previous) : ({'completedResults' : 0});
        // Every published stats object starts without 'presentedAtMs'; the
        // frame that consumes a result stamps it exactly once, and probes pair
        // the two timestamps to measure the handoff. Carrying the previous
        // object's stamp forward would date this publish to a frame that
        // presented something else. Absent is also the more useful answer
        // here: this publish is the one that never presented.
        delete stats['presentedAtMs'];
        stats['deviceLost'] = $0 > 0;
        stats['gpuWaitTimeoutSite'] = UTF8ToString($1);
        stats['gpuWaitTimeoutMs'] = $2;
        stats['publishedAtMs'] = performance.now();
        stats['publishReason'] = 'gpu-wait-failure';
        window['__donnerWorkerStats'] = stats;
      },
      deviceLost ? 1 : 0, timedOutWaitSiteName, timedOutWaitMs);
  // clang-format on
}
#endif

bool IsGraphicsElement(const svg::SVGElement& element) {
  return element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
    return element.isa<svg::SVGGraphicsElement>();
  });
}

/// During continuous pinch-zoom, the viewport's `desiredCanvasSize` changes
/// every wheel event (~60 Hz). Each commit through
/// `SVGDocument::setCanvasSize` calls `invalidateRenderTree`, which forces
/// the compositor to re-rasterize every promoted layer at the new canvas
/// size - at high zoom on the splash that's 7-8 layers × seconds each, so
/// the editor freezes for the whole gesture. Debouncing the commit lets
/// the previously-rendered bitmap (at the prior canvas size) keep displaying
/// stretched until the user stops zooming; the high-quality re-rasterize
/// then happens once.
constexpr std::chrono::milliseconds kCanvasSizeCommitDelay{120};
constexpr double kDragTranslationRecaptureScreenPx = 128.0;
constexpr bool kSelectionOnlyPrewarmMayTriggerRender = true;

svg::Renderer CreateRenderer(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice) {
  if (geodeDevice != nullptr) {
    return svg::Renderer(std::move(geodeDevice));
  }
#ifdef __EMSCRIPTEN__
  // Browser builds never render through this instance. WebGPU handles are
  // per-worker, so `AsyncRenderer::workerLoop` constructs its own renderer on
  // the raster thread and substitutes it for the one every request carries.
  // Giving this instance a backend would acquire a second headless GPU device
  // on the UI thread - an adapter/device request plus a pipeline compile that
  // nothing ever draws with, serialized ahead of the raster thread's own
  // device. Hand back a device-less instance so the render lease's reference
  // stays valid without owning a device.
  return svg::Renderer(std::shared_ptr<::donner::geode::GeodeDevice>());
#else
  return svg::Renderer();
#endif
}

constexpr AsyncRendererStartMode EditorRenderWorkerStartMode() {
  return AsyncRendererStartMode::Immediate;
}

bool ShouldCommitCanvas(const Vector2i& pending, bool wouldChange, bool deferForActiveDrag,
                        bool firstCommit, bool throttleElapsed) {
  return pending != Vector2i::Zero() && wouldChange && !deferForActiveDrag &&
         (firstCommit || throttleElapsed);
}

bool CompleteOverviewCanRepair(FramePresentationFailure failure) {
  return failure == FramePresentationFailure::MissingOverview ||
         failure == FramePresentationFailure::MissingSelectionGeometry ||
         failure == FramePresentationFailure::InsufficientCoverage ||
         failure == FramePresentationFailure::IncompatiblePose;
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

bool ShouldRetainSelectedPrewarmFallback(const RenderAttemptIdentity& attempt,
                                         const RenderResult& result,
                                         const EditorRasterViewport& visibleRaster) {
  return SameRasterViewport(attempt.rasterViewport, result.rasterViewport) &&
         attempt.selectedEntity != entt::null && !result.overviewInfillOnly &&
         result.rasterViewport.viewportBounded && visibleRaster.viewportBounded &&
         (result.rasterViewport.outputSizePx.x > visibleRaster.outputSizePx.x ||
          result.rasterViewport.outputSizePx.y > visibleRaster.outputSizePx.y);
}

bool SameRequestedDragPreview(const std::optional<RenderRequest::DragPreview>& lhs,
                              const std::optional<RenderRequest::DragPreview>& rhs) {
  if (!lhs.has_value() || !rhs.has_value()) {
    return lhs.has_value() == rhs.has_value();
  }
  return lhs->entity == rhs->entity && lhs->extraEntities == rhs->extraEntities &&
         lhs->interactionKind == rhs->interactionKind &&
         lhs->dragGeneration == rhs->dragGeneration && lhs->translation == rhs->translation &&
         SameTransform(lhs->documentFromCachedDocument, rhs->documentFromCachedDocument);
}

bool SamePresentationRepair(const std::optional<PresentationRepairIdentity>& lhs,
                            const std::optional<PresentationRepairIdentity>& rhs) {
  if (!lhs.has_value() || !rhs.has_value()) {
    return lhs.has_value() == rhs.has_value();
  }
  if (!lhs->scene.sameScene(rhs->scene) || !SameRasterViewport(lhs->coverage, rhs->coverage) ||
      lhs->selection != rhs->selection || lhs->poses.size() != rhs->poses.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs->poses.size(); ++i) {
    if (lhs->poses[i].entity != rhs->poses[i].entity ||
        !SamePresentationTransform(lhs->poses[i].documentFromElement,
                                   rhs->poses[i].documentFromElement)) {
      return false;
    }
  }
  return true;
}

bool SameRenderAttempt(const RenderAttemptIdentity& lhs, const RenderAttemptIdentity& rhs) {
  if (lhs.repair.has_value() || rhs.repair.has_value()) {
    return SamePresentationRepair(lhs.repair, rhs.repair);
  }
  return lhs.documentGeneration == rhs.documentGeneration && lhs.version == rhs.version &&
         lhs.overviewInfillOnly == rhs.overviewInfillOnly &&
         lhs.selectedEntity == rhs.selectedEntity &&
         lhs.presentationEpoch == rhs.presentationEpoch &&
         SameRasterViewport(lhs.rasterViewport, rhs.rasterViewport) &&
         SameRequestedDragPreview(lhs.dragPreview, rhs.dragPreview);
}

RenderAttemptIdentity MakeRenderAttempt(
    std::uint64_t documentGeneration, std::uint64_t version,
    const EditorRasterViewport& rasterViewport, bool overviewInfillOnly, Entity selectedEntity,
    const std::optional<RenderRequest::DragPreview>& scheduledDragPreview,
    std::uint64_t presentationEpoch) {
  return RenderAttemptIdentity{
      .documentGeneration = documentGeneration,
      .version = version,
      .rasterViewport = rasterViewport,
      .overviewInfillOnly = overviewInfillOnly,
      .selectedEntity = selectedEntity,
      .dragPreview = overviewInfillOnly ? std::nullopt : scheduledDragPreview,
      .presentationEpoch = presentationEpoch,
  };
}

bool SameViewport(const ViewportState& lhs, const ViewportState& rhs) {
  return lhs.paneOrigin == rhs.paneOrigin && lhs.paneSize == rhs.paneSize &&
         lhs.documentViewBox == rhs.documentViewBox &&
         lhs.devicePixelRatio == rhs.devicePixelRatio && lhs.zoom == rhs.zoom &&
         lhs.panDocPoint == rhs.panDocPoint && lhs.panScreenPoint == rhs.panScreenPoint;
}

DocumentPixelCaptureIdentity CurrentPixelCaptureIdentity(const EditorApp& app,
                                                         const ViewportState& viewport,
                                                         std::uint64_t sessionId,
                                                         std::uint64_t canvasCommitGeneration) {
  return DocumentPixelCaptureIdentity{
      .sessionId = sessionId,
      .documentGeneration = app.document().documentGeneration(),
      .version = app.document().currentFrameVersion(),
      .fontResourceRevision = app.document().fontResourceRevision(),
      .canvasCommitGeneration = canvasCommitGeneration,
      .rasterViewport = viewport.rasterViewport(),
      .viewport = viewport,
  };
}

bool SamePixelCaptureIdentity(const DocumentPixelCaptureIdentity& lhs,
                              const DocumentPixelCaptureIdentity& rhs) {
  return lhs.sessionId == rhs.sessionId && lhs.documentGeneration == rhs.documentGeneration &&
         lhs.version == rhs.version && lhs.fontResourceRevision == rhs.fontResourceRevision &&
         lhs.canvasCommitGeneration == rhs.canvasCommitGeneration &&
         SameRasterViewport(lhs.rasterViewport, rhs.rasterViewport) &&
         SameViewport(lhs.viewport, rhs.viewport);
}

bool IsExactPixelCaptureResult(const RenderResult& result,
                               const DocumentPixelCaptureIdentity& current) {
  return result.documentGeneration == current.documentGeneration &&
         result.version == current.version &&
         result.fontResourceRevision == current.fontResourceRevision &&
         SameRasterViewport(result.rasterViewport, current.rasterViewport) &&
         SameViewport(result.viewport, current.viewport) && !result.overviewInfillOnly &&
         (!result.compositedPreview.has_value() ||
          !result.compositedPreview->representedDragPreview.has_value() ||
          result.compositedPreview->representedDragPreview->interactionKind !=
              svg::compositor::InteractionHint::ActiveDrag);
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

bool HasCompleteVisibleCachedCoverage(const GlTextureCache* textures, const ViewportState& viewport,
                                      const EditorRasterViewport& visibleRaster) {
  if (textures == nullptr || textures->tiles().empty() || textures->metadataOnlyMissCount() != 0) {
    return false;
  }
  const PresentationCoverageDiagnostics coverage = textures->coverageDiagnostics();
  // The base raster carries a 128-screen-pixel margin. During a small pan the requested raster
  // rectangle shifts, but the previously cached rectangle still covers every visible artboard
  // pixel. Test pane coverage rather than insisting on a second 128-pixel margin at the new pan.
  const Box2d paneDocumentRect = viewport.screenToDocument(
      Box2d(viewport.paneOrigin, viewport.paneOrigin + viewport.paneSize));
  const Box2d visibleArtboardRect(
      Vector2d(std::max(paneDocumentRect.topLeft.x, viewport.documentViewBox.topLeft.x),
               std::max(paneDocumentRect.topLeft.y, viewport.documentViewBox.topLeft.y)),
      Vector2d(std::min(paneDocumentRect.bottomRight.x, viewport.documentViewBox.bottomRight.x),
               std::min(paneDocumentRect.bottomRight.y, viewport.documentViewBox.bottomRight.y)));
  if (visibleArtboardRect.size().x <= 0.0 || visibleArtboardRect.size().y <= 0.0 ||
      !DocumentRectContains(coverage.activeRasterDocumentRect, visibleArtboardRect)) {
    return false;
  }
  const Vector2d cachedDocumentSize = coverage.activeRasterDocumentRect.size();
  const Vector2d visibleDocumentSize = visibleRaster.documentRect.size();
  if (cachedDocumentSize.x <= 0.0 || cachedDocumentSize.y <= 0.0 || visibleDocumentSize.x <= 0.0 ||
      visibleDocumentSize.y <= 0.0) {
    return false;
  }
  // A lower-resolution overview may cover the same document rect without having enough source
  // pixels for a crisp interaction. Compare pixel density on both axes before retaining it.
  constexpr double kScaleTolerance = 1e-3;
  const Vector2d cachedScale(coverage.activeOutputSizePx.x / cachedDocumentSize.x,
                             coverage.activeOutputSizePx.y / cachedDocumentSize.y);
  const Vector2d visibleScale(visibleRaster.outputSizePx.x / visibleDocumentSize.x,
                              visibleRaster.outputSizePx.y / visibleDocumentSize.y);
  return std::abs(cachedScale.x - visibleScale.x) <= kScaleTolerance &&
         std::abs(cachedScale.y - visibleScale.y) <= kScaleTolerance;
}

bool RasterViewportCanPresentCurrentViewport(const EditorRasterViewport& rendered,
                                             const EditorRasterViewport& current) {
  return SameRasterViewport(rendered, current) ||
         (SameRasterScaleAndSemanticCanvas(rendered, current) &&
          DocumentRectContains(rendered.documentRect, current.documentRect));
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
                                        bool hasCachedPresentation, bool rasterViewportSettled,
                                        bool needsOverviewInfill,
                                        bool pendingSelectedLayerRasterization) {
  return selectedEntity != entt::null && !hasActiveDrag && currentVersion == displayedDocVersion &&
         hasCachedPresentation && !rasterViewportSettled && !needsOverviewInfill &&
         !pendingSelectedLayerRasterization;
}

bool ShouldUseSelectedPrewarmRasterViewport(Entity selectedEntity, bool requestOverviewInfill,
                                            bool rasterViewportBounded,
                                            bool selectionOnlyPrewarmMayTriggerRender,
                                            bool hasIndependentRenderReason,
                                            bool hasCompleteVisibleCachedCoverage,
                                            Vector2i visibleOutputSizePx,
                                            Vector2i prewarmOutputSizePx) {
  if (selectedEntity == entt::null || requestOverviewInfill || !rasterViewportBounded ||
      (!selectionOnlyPrewarmMayTriggerRender && !hasIndependentRenderReason)) {
    return false;
  }

  // A different output size invalidates every cached static segment, regardless of whether the
  // canvas is large enough to cross an absolute pixel threshold. Preserve complete visible cache
  // coverage for the first pointer frame. The promoted target is rasterized independently from
  // its own full object bounds, so the visible background raster need not expand with it.
  if (prewarmOutputSizePx == visibleOutputSizePx) {
    return true;
  }
  if (hasCompleteVisibleCachedCoverage) {
    return false;
  }

  // Without a complete visible cache there is no static tile set to protect. Use a modest
  // incremental area limit so a cold selection still gains overdraw without multiplying the
  // initial scene render cost on a small pane.
  const std::int64_t visiblePixels = static_cast<std::int64_t>(visibleOutputSizePx.x) *
                                     static_cast<std::int64_t>(visibleOutputSizePx.y);
  const std::int64_t prewarmPixels = static_cast<std::int64_t>(prewarmOutputSizePx.x) *
                                     static_cast<std::int64_t>(prewarmOutputSizePx.y);
  return visiblePixels > 0 && prewarmPixels > 0 &&
         prewarmPixels - visiblePixels <= visiblePixels / 4;
}

bool HasIndependentSelectedPrewarmRenderReason(bool hasActiveDrag, bool versionChanged,
                                               bool forceSelectedLayerRasterization,
                                               bool forcePresentationRefresh) {
  return hasActiveDrag || versionChanged || forceSelectedLayerRasterization ||
         forcePresentationRefresh;
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
  // A selected text/marker/filter child may be unpromotable, yet its complete owning tiles still
  // contain the forced style refresh. Requiring a dedicated selected tile here leaves the pending
  // obligation set forever and posts the identical render on every idle frame.
  return preview.valid() &&
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

Transform2d OverlayDocumentFromSourceDragPreview(
    const std::optional<SelectTool::ActiveDragPreview>& sourceDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& targetDragPreview) {
  if (!sourceDragPreview.has_value() || !targetDragPreview.has_value() ||
      sourceDragPreview->entity != targetDragPreview->entity ||
      sourceDragPreview->dragGeneration != targetDragPreview->dragGeneration ||
      std::abs(sourceDragPreview->documentFromCachedDocument.determinant()) < 1e-12) {
    return Transform2d();
  }

  return sourceDragPreview->documentFromCachedDocument.inverse() *
         targetDragPreview->documentFromCachedDocument;
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
      OverlayDocumentFromSourceDragPreview(liveDragPreview, representedDragPreview);
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
    : renderer(CreateRenderer(std::move(geodeDevice))),
      asyncRenderer(EditorRenderWorkerStartMode()) {}

RenderCoordinator::RenderCoordinator(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice)
    : renderWorker_(std::move(geodeDevice)) {}

bool NothingToPresentRetry::noteFailure(const RenderAttemptIdentity& attempt,
                                        Clock::time_point now) {
  if (failedAttempt_.has_value() && SameRenderAttempt(*failedAttempt_, attempt)) {
    ++failures_;
  } else {
    failedAttempt_ = attempt;
    failures_ = 1;
  }
  if (failures_ <= kRetryDelays.size()) {
    retryAt_ = now + kRetryDelays[failures_ - 1];
    return false;
  }
  return failures_ == kRetryDelays.size() + 1;
}

bool NothingToPresentRetry::mayPost(const RenderAttemptIdentity& attempt,
                                    Clock::time_point now) const {
  if (!failedAttempt_.has_value() || !SameRenderAttempt(*failedAttempt_, attempt)) {
    return true;
  }
  return retryScheduled() && now >= retryAt_;
}

bool NothingToPresentRetry::retryScheduled() const {
  return failedAttempt_.has_value() && failures_ <= kRetryDelays.size();
}

std::optional<float> NothingToPresentRetry::secondsUntilRetry(Clock::time_point now) const {
  if (!retryScheduled() || now >= retryAt_) {
    return std::nullopt;
  }
  return std::max(0.0f, std::chrono::duration<float>(retryAt_ - now).count());
}

void NothingToPresentRetry::reset() {
  failedAttempt_.reset();
  failures_ = 0;
}

void RenderCoordinator::resetForLoadedDocument(std::uint64_t documentGeneration) {
  framePresentation_.reset();
  reusableFrameInput_.reset();
  reusableFrameResources_.reset();
  reusableSelectionCapture_.reset();
  presentationNeedsRender_ = false;
  presentationNeedsCoverage_ = false;
  adoptionFailure_ = FramePresentationFailure::None;
  pendingRepair_.reset();
  acceptedRepairAttempt_.reset();
  lastRejectedRepairCapture_ = 0;
  selectedSceneCapture_.reset();
  livePathCapture_.reset();
  (void)documentGeneration;
  compositedPresentation_ = CompositedPresentation{};
  selectionBoundsCache_ = SelectionBoundsCache{};
  displayedDocVersion_ = 0;
  sourceHoverElements_.clear();
  lockedRejectionFlash_.reset();
  immediateOverlaySnapshot_.reset();
  penLivePreviewElement_.reset();
  penHoverPreviewSegmentDoc_.reset();
  penHoverCloseAffordanceDoc_.reset();
  textEditing_.reset();
  textBoxDragPreviewDoc_.reset();
  renderScheduler_.reset();
  displayNoneSuppressedSelectionEntity_ = entt::null;
  displayNoneSuppressedLayerEntity_ = entt::null;
  pendingSelectedLayerRasterizationEntity_ = entt::null;
  pendingSelectedLayerRasterizationVersion_ = 0;
  pendingCanvasSize_ = Vector2i::Zero();
  pendingCanvasSizeSince_ = std::chrono::steady_clock::time_point{};
  pendingRasterViewport_.reset();
  pendingRasterViewportSince_ = std::chrono::steady_clock::time_point{};
  overviewDocVersion_ = 0;
  pendingOverviewResult_.reset();
  pendingPresentationRefresh_ = false;
  lastPostedAttempt_.reset();
  selectedPrewarmFallback_.reset();
  selectedPrewarmRecoveryPending_ = false;
  unavailableSelectedPromotion_.reset();
  nothingToPresentRetry_.reset();
  setDocumentPixelCaptureEnabled(false);
  lastFrameCostBreakdown_ = FrameCostBreakdown{};
}

void RenderCoordinator::setDocumentPixelCaptureEnabled(bool enabled) {
  if (documentPixelCaptureEnabled_ == enabled) {
    return;
  }
  documentPixelCaptureEnabled_ = enabled;
  ++documentPixelCaptureSessionId_;
  requestedPixelCapture_.reset();
  documentPixelCapture_.reset();
  captureUnavailable_ = false;
  pixelCaptureCanvasCommitDue_.reset();
  if (enabled) {
    requestPresentationRefresh();
  }
}

const DocumentPixelCapture* RenderCoordinator::documentPixelCaptureFor(
    const EditorApp& app, const ViewportState& viewport) const {
  if (!documentPixelCaptureEnabled_ || !documentPixelCapture_.has_value() || !app.hasDocument()) {
    return nullptr;
  }
  if (pixelCaptureCanvasCommitDue_.has_value()) {
    return nullptr;
  }
  const DocumentPixelCaptureIdentity current = CurrentPixelCaptureIdentity(
      app, viewport, documentPixelCaptureSessionId_, documentCanvasCommitTotal_);
  return SamePixelCaptureIdentity(documentPixelCapture_->identity, current)
             ? &*documentPixelCapture_
             : nullptr;
}

std::optional<float> RenderCoordinator::nextNothingToPresentRetryWakeSeconds() const {
  if (renderWorker_.asyncRenderer.isBusy()) {
    return std::nullopt;
  }
  return nothingToPresentRetry_.secondsUntilRetry(schedulingNow());
}

std::chrono::steady_clock::time_point RenderCoordinator::schedulingNow() const {
  return schedulingClockForTesting_ ? schedulingClockForTesting_()
                                    : std::chrono::steady_clock::now();
}

void RenderCoordinator::noteResultWithNothingToPresent(const std::optional<RenderResult>& result) {
  ++nothingToPresentResultTotal_;
  const bool fromLastPost = lastPostedAttempt_.has_value() &&
                            lastPostedAttempt_->documentGeneration == result->documentGeneration &&
                            lastPostedAttempt_->version == result->version;
  const EditorRasterViewport visibleRaster = result->viewport.rasterViewport();
  if (fromLastPost &&
      ShouldRetainSelectedPrewarmFallback(*lastPostedAttempt_, *result, visibleRaster)) {
    selectedPrewarmFallback_ = SelectedPrewarmFallback{
        .documentGeneration = result->documentGeneration,
        .selectedEntity = lastPostedAttempt_->selectedEntity,
        .visibleRaster = visibleRaster,
    };
    selectedPrewarmRecoveryPending_ = true;
  }
  if (!fromLastPost || !nothingToPresentRetry_.noteFailure(*lastPostedAttempt_, schedulingNow())) {
    rejectPixelCaptureResult(result);
    return;
  }
  std::fprintf(stderr,
               "[editor] document version %" PRIu64
               " rendered nothing to present %zu times; not rendering it again until it changes\n",
               result->version, NothingToPresentRetry::kRetryDelays.size() + 1);
  if (documentPixelCaptureEnabled_ &&
      result->cpuSnapshotRequestId == documentPixelCaptureSessionId_) {
    // Hold the capture request for this identity as unavailable, as for an oversized capture, so
    // the picker stops asking for it until the document or viewport changes.
    captureUnavailable_ = true;
  }
}

bool RenderCoordinator::selectedPrewarmFallbackApplies(std::uint64_t documentGeneration,
                                                       Entity selectedEntity,
                                                       const EditorRasterViewport& visibleRaster) {
  if (selectedPrewarmFallback_.has_value() &&
      (selectedPrewarmFallback_->documentGeneration != documentGeneration ||
       selectedPrewarmFallback_->selectedEntity != selectedEntity ||
       !SameRasterViewport(selectedPrewarmFallback_->visibleRaster, visibleRaster))) {
    selectedPrewarmFallback_.reset();
    selectedPrewarmRecoveryPending_ = false;
  }
  return selectedPrewarmFallback_.has_value();
}

void RenderCoordinator::noteSelectedPrewarmResultPresented(const RenderResult& result) {
  if (selectedPrewarmFallback_.has_value() &&
      result.documentGeneration == selectedPrewarmFallback_->documentGeneration &&
      SameRasterViewport(result.rasterViewport, selectedPrewarmFallback_->visibleRaster)) {
    selectedPrewarmRecoveryPending_ = false;
  }
}

bool RenderCoordinator::shouldRequestSelectionOnlyPrewarm(
    std::uint64_t documentGeneration, Entity selectedEntity, std::uint64_t version,
    const EditorRasterViewport& visibleRaster) const {
  return kSelectionOnlyPrewarmMayTriggerRender &&
         (!unavailableSelectedPromotion_.has_value() ||
          unavailableSelectedPromotion_->documentGeneration != documentGeneration ||
          unavailableSelectedPromotion_->version != version ||
          unavailableSelectedPromotion_->selectedEntity != selectedEntity ||
          !SameRasterViewport(unavailableSelectedPromotion_->visibleRaster, visibleRaster));
}

void RenderCoordinator::noteSelectedPromotionAvailability(const RenderResult& result) {
  const RenderResult::CompositedPreview& preview = *result.compositedPreview;
  if (preview.entity != entt::null) {
    unavailableSelectedPromotion_.reset();
    return;
  }
  if (preview.interactionKind != svg::compositor::InteractionHint::Selection ||
      !preview.representedDragPreview.has_value() ||
      preview.representedDragPreview->entity == entt::null) {
    return;
  }
  unavailableSelectedPromotion_ = UnavailableSelectedPromotion{
      .documentGeneration = result.documentGeneration,
      .version = result.version,
      .selectedEntity = preview.representedDragPreview->entity,
      .visibleRaster = result.viewport.rasterViewport(),
  };
}

std::optional<float> RenderCoordinator::nextPixelCaptureCanvasCommitWakeSeconds() const {
  if (!documentPixelCaptureEnabled_ || !pixelCaptureCanvasCommitDue_.has_value() ||
      renderWorker_.asyncRenderer.isBusy()) {
    return std::nullopt;
  }
  const auto remaining = *pixelCaptureCanvasCommitDue_ - schedulingNow();
  return std::max(0.0f, std::chrono::duration<float>(remaining).count());
}

void RenderCoordinator::noteMissingPixelCaptureResult(const std::optional<RenderResult>& result) {
  if (!result.has_value() && documentPixelCaptureEnabled_ && requestedPixelCapture_.has_value() &&
      !documentPixelCapture_.has_value() && !captureUnavailable_ &&
      !renderWorker_.asyncRenderer.isBusy()) {
    requestedPixelCapture_.reset();
    requestPresentationRefresh();
  }
}

void RenderCoordinator::rejectRenderResult(const std::optional<RenderResult>& result) {
  if (result.has_value()) {
    renderWorker_.asyncRenderer.discardUnpresentedResult(*result);
  }
  rejectPixelCaptureResult(result);
}

void RenderCoordinator::rejectPixelCaptureResult(const std::optional<RenderResult>& result) {
  if (result.has_value() && documentPixelCaptureEnabled_ &&
      result->cpuSnapshotRequestId == documentPixelCaptureSessionId_) {
    requestedPixelCapture_.reset();
    captureUnavailable_ = false;
  }
}

void RenderCoordinator::acceptPixelCaptureResult(RenderResult& result, const EditorApp& app,
                                                 const ViewportState& viewport) {
  if (!documentPixelCaptureEnabled_ ||
      result.cpuSnapshotRequestId != documentPixelCaptureSessionId_ ||
      !requestedPixelCapture_.has_value()) {
    return;
  }
  const DocumentPixelCaptureIdentity current = CurrentPixelCaptureIdentity(
      app, viewport, documentPixelCaptureSessionId_, documentCanvasCommitTotal_);
  if (!SamePixelCaptureIdentity(*requestedPixelCapture_, current) ||
      !IsExactPixelCaptureResult(result, current)) {
    requestedPixelCapture_.reset();
    captureUnavailable_ = false;
    return;
  }
  captureUnavailable_ = !IsValidDocumentPixelBitmap(result.bitmap);
  if (!captureUnavailable_) {
    documentPixelCapture_ = DocumentPixelCapture{
        .identity = current,
        .bitmap = std::move(result.bitmap),
    };
  }
}

bool RenderCoordinator::preparePixelCaptureRequest(const EditorApp& app,
                                                   const ViewportState& viewport,
                                                   DocumentPixelCaptureIdentity* desired) {
  if (!documentPixelCaptureEnabled_) {
    return false;
  }
  *desired = CurrentPixelCaptureIdentity(app, viewport, documentPixelCaptureSessionId_,
                                         documentCanvasCommitTotal_);
  if (documentPixelCapture_.has_value() &&
      !SamePixelCaptureIdentity(documentPixelCapture_->identity, *desired)) {
    documentPixelCapture_.reset();
    captureUnavailable_ = false;
  }
  if (requestedPixelCapture_.has_value() &&
      !SamePixelCaptureIdentity(*requestedPixelCapture_, *desired)) {
    requestedPixelCapture_.reset();
    captureUnavailable_ = false;
  }
  const bool alreadyRequested = requestedPixelCapture_.has_value();
  if (documentPixelCapture_.has_value() || alreadyRequested) {
    return false;
  }
  if (!CanCaptureDocumentPixelSize(desired->rasterViewport.outputSizePx)) {
    requestedPixelCapture_ = *desired;
    captureUnavailable_ = true;
    return false;
  }
  return true;
}

bool RenderCoordinator::pixelCaptureBlocksViewportDefer(bool captureNeeded) const {
  return captureNeeded ||
         (documentPixelCaptureEnabled_ &&
          (pixelCaptureCanvasCommitDue_.has_value() ||
           (requestedPixelCapture_.has_value() && !documentPixelCapture_.has_value())));
}

bool RenderCoordinator::shouldDeferViewportRender(bool selectedViewportDeferred,
                                                  bool needsOverviewInfill,
                                                  bool captureNeeded) const {
  return selectedViewportDeferred && !needsOverviewInfill && !pendingPresentationRefresh_ &&
         !presentationNeedsRender_ && !pixelCaptureBlocksViewportDefer(captureNeeded);
}

bool RenderCoordinator::shouldCapturePixelsForRequest(
    bool requestOverviewInfill, bool activeDrag, const EditorRasterViewport& rasterViewport) const {
  return documentPixelCaptureEnabled_ && !documentPixelCapture_.has_value() &&
         !captureUnavailable_ && !requestOverviewInfill && !activeDrag &&
         !compositedPresentation_.isWaitingForFullRender() &&
         CanCaptureDocumentPixelSize(rasterViewport.outputSizePx);
}

void RenderCoordinator::configurePixelCaptureRequest(
    RenderRequest* request, bool requestOverviewInfill, bool activeDrag,
    const EditorRasterViewport& rasterViewport) const {
  request->captureCpuSnapshot =
      shouldCapturePixelsForRequest(requestOverviewInfill, activeDrag, rasterViewport);
  if (request->captureCpuSnapshot) {
    request->cpuSnapshotRequestId = documentPixelCaptureSessionId_;
  }
}

void RenderCoordinator::recordPixelCaptureRequest(const RenderRequest& request,
                                                  const DocumentPixelCaptureIdentity& desired) {
  if (request.captureCpuSnapshot) {
    requestedPixelCapture_ = desired;
    captureUnavailable_ = false;
  }
}

void RenderCoordinator::updatePixelCaptureCanvasCommitWake(bool wouldChange, bool firstCommit,
                                                           bool deferForActiveDrag) {
  if (documentPixelCaptureEnabled_ && wouldChange && !firstCommit && !deferForActiveDrag) {
    pixelCaptureCanvasCommitDue_ = pendingCanvasSizeSince_ + kCanvasSizeCommitDelay;
  } else {
    pixelCaptureCanvasCommitDue_.reset();
  }
}

void RenderCoordinator::noteCanvasSizeCommitForPixelCapture(const EditorApp& app,
                                                            const ViewportState& viewport,
                                                            DocumentPixelCaptureIdentity* desired,
                                                            bool* captureNeeded,
                                                            bool* forcePresentationRefresh) {
  pixelCaptureCanvasCommitDue_.reset();
  if (!documentPixelCaptureEnabled_) {
    return;
  }
  documentPixelCapture_.reset();
  requestedPixelCapture_.reset();
  captureUnavailable_ = false;
  *desired = CurrentPixelCaptureIdentity(app, viewport, documentPixelCaptureSessionId_,
                                         documentCanvasCommitTotal_);
  *captureNeeded = true;
  *forcePresentationRefresh = true;
  requestPresentationRefresh();
}

bool RenderCoordinator::setSourceHoverElements(std::vector<svg::SVGElement> elements) {
  if (sourceHoverElements_ == elements) {
    return false;
  }

  sourceHoverElements_ = std::move(elements);
  selectionGeometryRefreshRequested_ = true;
  return true;
}

void RenderCoordinator::setLockedRejectionFlash(
    std::optional<SelectTool::LockedRejectionFlash> flash) {
  const auto before = lockedRejectionFlash_.has_value()
                          ? std::optional<svg::SVGElement>(lockedRejectionFlash_->element)
                          : std::nullopt;
  const auto after =
      flash.has_value() ? std::optional<svg::SVGElement>(flash->element) : std::nullopt;
  selectionGeometryRefreshRequested_ |= before != after;
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

std::vector<Entity> RenderCoordinator::selectedPresentationEntities(const EditorApp& app) const {
  std::vector<Entity> selected;
  for (const auto& element : app.selectedElements()) {
    selected.push_back(element.unsafeEntityHandle().entity());
  }
  return selected;
}

std::optional<LockedRejectionFlashInput> RenderCoordinator::lockedFlashForCapture() const {
  if (!lockedRejectionFlash_.has_value()) {
    return std::nullopt;
  }
  return LockedRejectionFlashInput{lockedRejectionFlash_->element,
                                   lockedRejectionFlash_->intensity};
}

bool RenderCoordinator::shouldRecaptureSelection(const CapturedPresentation& captured,
                                                 const std::vector<Entity>& selection) const {
  const bool subjectChanged = captured.selection() != selection ||
                              selectionGeometryRefreshRequested_ || !sourceHoverElements_.empty() ||
                              lockedRejectionFlash_.has_value();
  const bool captureStale = selectedSceneCapture_ == nullptr ||
                            selectedSceneCapture_->selection() != selection ||
                            selectedSceneCapture_->identity() != captured.identity() ||
                            selectionGeometryRefreshRequested_;
  return subjectChanged && captureStale;
}

void RenderCoordinator::refreshFrameSelectionCapture(
    EditorApp& app, const std::shared_ptr<const CapturedPresentation>& captured,
    const std::vector<Entity>& selection) {
  if (shouldRecaptureSelection(*captured, selection) &&
      captured->identity().documentGeneration == app.document().documentGeneration() &&
      captured->identity().version == app.document().currentFrameVersion() &&
      captured->identity().fontResourceRevision == app.document().fontResourceRevision()) {
    const auto access = app.document().document().tryWriteAccess();
    if (access.has_value() && app.document().document().canvasSize() == captured->canvasSize() &&
        app.document().document().handle()->revision() == captured->identity().documentRevision) {
      selectedSceneCapture_ = CapturedPresentation::Capture(
          app.document().document(), captured->identity(), app.selectedElements(), {},
          captured->independentlyMovable(), sourceHoverElements_, lockedFlashForCapture(),
          std::nullopt, captured->textEditing());
      selectionGeometryRefreshRequested_ = false;
    }
  }
}

PresentationIdentity RenderCoordinator::currentPresentationIdentity(const EditorApp& app) const {
  return {.documentGeneration = app.document().documentGeneration(),
          .documentRevision = app.document().document().handle()->revision(),
          .version = app.document().currentFrameVersion(),
          .geometryRevision = app.document().nonTransformRevision(),
          .fontResourceRevision = app.document().fontResourceRevision(),
          .presentationEpoch = presentationEpoch_};
}

FramePresentationInput RenderCoordinator::makeFrameInput(EditorApp& app, SelectTool& tool,
                                                         const ViewportState& viewport,
                                                         const Box2d& paneClipRect,
                                                         SelectionChromeDetail detail,
                                                         bool includeChrome) {
  auto desired = compositedPresentation_.activePreviewForPresentation(tool.activeDragPreview());
  if (desired.has_value()) {
    desired->contentIdentity.presentationEpoch = presentationEpoch_;
  }
  FramePresentationInput input{
      .frameId = nextPresentationFrameId_++,
      .viewport = viewport,
      .paneClipRect = paneClipRect,
      .selection = selectedPresentationEntities(app),
      .documentIdentity = currentPresentationIdentity(app),
      .pendingDocumentMutations = app.document().hasPendingMutations(),
      .desired = std::move(desired),
      .detail = detail,
      .decorations = PresentationDecorations{.marqueeDoc = tool.marqueeRect(),
                                             .penPreviewSegmentDoc = penHoverPreviewSegmentDoc_,
                                             .penCloseAffordanceDoc = penHoverCloseAffordanceDoc_,
                                             .textEditing = textEditing_,
                                             .textBoxDragPreviewDoc = textBoxDragPreviewDoc_},
      .suppressedLayerEntity = suppressedCompositedLayerEntity(app),
      .suppressSelectionPixels = selectedElementIsDisplayNone(app),
      .includeChrome = includeChrome,
  };
  for (const auto& hovered : sourceHoverElements_) {
    input.decorations.sourceHover.push_back(hovered.unsafeEntityHandle().entity());
  }
  if (lockedRejectionFlash_.has_value()) {
    input.decorations.lockedFlashEntity =
        lockedRejectionFlash_->element.unsafeEntityHandle().entity();
    input.decorations.lockedFlashIntensity = lockedRejectionFlash_->intensity;
  }
  return input;
}

void RenderCoordinator::refreshLivePathCapture(EditorApp& app, FramePresentationInput& input) {
  if (!penLivePreviewElement_.has_value()) {
    livePathCapture_.reset();
  } else if (const auto access = app.document().document().tryWriteAccess(); access.has_value()) {
    const std::array selected{*penLivePreviewElement_};
    const std::array independent{penLivePreviewElement_->unsafeEntityHandle().entity()};
    if (livePathCapture_ == nullptr || livePathCapture_->selection() != input.selection ||
        livePathCapture_->identity().documentRevision !=
            app.document().document().handle()->revision()) {
      livePathCapture_ =
          CapturedPresentation::Capture(app.document().document(), input.documentIdentity, selected,
                                        {}, independent, {}, std::nullopt, penLivePreviewElement_);
    }
  }
  input.livePathReplacement = livePathCapture_;
}

void RenderCoordinator::recordFramePresentationCost(const FramePresentationInput& input,
                                                    const SelectTool& tool, double elapsedMs) {
  auto& cost = lastFrameCostBreakdown_.overlay;
  cost = {};
  cost.canvasSize = OverlayRasterSizeForViewport(input.viewport);
  cost.captureMs = elapsedMs;
  cost.selectedElementCount = static_cast<int>(input.selection.size());
  cost.pathCount = static_cast<int>(framePresentation_->chrome().paths.size());
  cost.aabbCount = static_cast<int>(framePresentation_->chrome().aabbsDoc.size());
  cost.handleCount = static_cast<int>(framePresentation_->chrome().handleAnchorsDoc.size());
  cost.sourceHoverElementCount = static_cast<int>(input.decorations.sourceHover.size());
  cost.hoverPathCount = static_cast<int>(framePresentation_->chrome().hoverPaths.size());
  cost.hoverAabbCount = static_cast<int>(framePresentation_->chrome().hoverAabbsDoc.size());
  cost.hasMarquee = input.decorations.marqueeDoc.has_value();
  cost.hasLiveDragPreview = tool.activeDragPreview().has_value();
  cost.hasRepresentedDragPreview = framePresentation_->followsPointer();
  cost.selectionBoundsOnly = input.detail == SelectionChromeDetail::CombinedBoundsOnly;
  if (input.desired.has_value()) {
    cost.liveDragTranslationDoc = input.desired->translation;
    if (const auto represented =
            ResolvePresentationTransform(input.desired->startPoses, framePresentation_->poses())) {
      cost.representedDragTranslationDoc = represented->translation();
    }
  }
  if (cost.pathCount != 0 || cost.aabbCount != 0 || cost.hoverPathCount != 0) {
    cost.payloadBytes = static_cast<std::uint64_t>(cost.canvasSize.x) * cost.canvasSize.y * 4u;
  }
}

namespace {
#ifdef __EMSCRIPTEN__
void PublishFrameRepairDetails(const FramePresentationInput& input,
                               const GlTextureCache::PresentationResources& resources,
                               FramePresentationFailure failure,
                               FramePresentationFailure adoptionFailure) {
  const auto current = input.documentIdentity;
  const auto active = resources.capture()->identity();
  const auto overview = resources.overviewCapture() ? resources.overviewCapture()->identity()
                                                    : PresentationIdentity{};
  // clang-format off
  MAIN_THREAD_ASYNC_EM_ASM({
    window['__donnerPresentationRepairStats'] = ({
      'failure': $0, 'current': [$1, $2, $3, $4],
      'active': [$5, $6, $7, $8, $9], 'overview': [$10, $11, $12, $13, $14],
      'bounded': Boolean($15),
    });
  }, static_cast<int>(failure), static_cast<double>(current.documentRevision),
      static_cast<double>(current.version), static_cast<double>(current.geometryRevision),
      static_cast<double>(current.presentationEpoch), static_cast<double>(active.captureId),
      static_cast<double>(active.documentRevision), static_cast<double>(active.version),
      static_cast<double>(active.geometryRevision), static_cast<double>(active.presentationEpoch),
      static_cast<double>(overview.captureId), static_cast<double>(overview.documentRevision),
      static_cast<double>(overview.version), static_cast<double>(overview.geometryRevision),
      static_cast<double>(overview.presentationEpoch), resources.coverage().activeTilesViewportBounded);
  MAIN_THREAD_ASYNC_EM_ASM({
    const stats = window['__donnerPresentationRepairStats'];
    if (stats && Object.is(stats['active'][0], $0)) {
      stats['pendingMutations'] = Boolean($1);
      stats['adoptionFailure'] = $10;
      stats['activeCoverage'] = ([$2, $3, $4, $5]);
      stats['overviewCoverage'] = ([$6, $7, $8, $9]);
    }
  }, static_cast<double>(active.captureId), input.pendingDocumentMutations,
      resources.coverage().activeRasterDocumentRect.topLeft.x,
      resources.coverage().activeRasterDocumentRect.topLeft.y,
      resources.coverage().activeRasterDocumentRect.bottomRight.x,
      resources.coverage().activeRasterDocumentRect.bottomRight.y,
      resources.coverage().overviewRasterDocumentRect.topLeft.x,
      resources.coverage().overviewRasterDocumentRect.topLeft.y,
      resources.coverage().overviewRasterDocumentRect.bottomRight.x,
      resources.coverage().overviewRasterDocumentRect.bottomRight.y, static_cast<int>(adoptionFailure));
  // clang-format on
}
#endif

bool HasOnlyStaticFrameIntent(const FramePresentationInput& input) {
  const auto& decorations = input.decorations;
  return !input.desired && !input.pendingDocumentMutations && !input.livePathReplacement &&
         decorations.sourceHover.empty() && decorations.lockedFlashEntity == entt::null &&
         !decorations.marqueeDoc && !decorations.penPreviewSegmentDoc &&
         !decorations.penCloseAffordanceDoc && !decorations.textEditing &&
         !decorations.textBoxDragPreviewDoc;
}
}  // namespace

namespace {
bool SameStaticFrameIntent(const FramePresentationInput& input, const FramePresentationInput& old) {
  return input.documentIdentity.sameScene(old.documentIdentity) &&
         input.selection == old.selection && SameViewport(input.viewport, old.viewport) &&
         input.paneClipRect == old.paneClipRect && input.detail == old.detail &&
         input.includeChrome == old.includeChrome &&
         input.suppressedLayerEntity == old.suppressedLayerEntity &&
         input.suppressSelectionPixels == old.suppressSelectionPixels;
}

void PrepareCandidateFrame(FramePresentationBuildResult& outcome,
                           const FramePresentationAdmission& admit) {
  if (outcome.frame != nullptr && admit && !admit(*outcome.frame)) {
    outcome.frame.reset();
    outcome.failure = FramePresentationFailure::UploadRefused;
  }
}

bool FrameCarriesInput(const FramePresentation& frame, const FramePresentationInput& input) {
  if (!frame.identity().sameContent(input.documentIdentity)) {
    return false;
  }
  if (!input.desired) {
    return frame.identity().sameScene(input.documentIdentity);
  }
  return std::ranges::all_of(input.desired->poses, [&](const auto& requested) {
    return std::ranges::any_of(frame.poses(), [&](const auto& actual) {
      return requested.entity == actual.entity &&
             SamePresentationTransform(requested.documentFromElement, actual.documentFromElement);
    });
  });
}
}  // namespace

bool RenderCoordinator::canReuseFrame(
    const FramePresentationInput& input,
    const std::shared_ptr<const GlTextureCache::PresentationResources>& resources) const {
  if (presentationNeedsRender_ || framePresentation_ == nullptr || !reusableFrameInput_ ||
      resources != reusableFrameResources_ || selectedSceneCapture_ != reusableSelectionCapture_ ||
      !HasOnlyStaticFrameIntent(input)) {
    return false;
  }
  return SameStaticFrameIntent(input, *reusableFrameInput_);
}

void RenderCoordinator::updateFrameRepairStatus(
    const FramePresentationInput& input,
    const std::shared_ptr<const GlTextureCache::PresentationResources>& resources,
    const FramePresentationBuildResult& outcome) {
  const auto failure =
      outcome.failure != FramePresentationFailure::None ? outcome.failure : adoptionFailure_;
  presentationNeedsRender_ = failure != FramePresentationFailure::None;
  presentationNeedsCoverage_ = failure == FramePresentationFailure::InsufficientCoverage ||
                               failure == FramePresentationFailure::MissingOverview;
#ifdef __EMSCRIPTEN__
  PublishFrameRepairDetails(input, *resources, failure, adoptionFailure_);
#endif
  if (presentationNeedsRender_) {
    pendingRepair_ = PresentationRepairIdentity{
        .scene = input.documentIdentity,
        .selection = input.selection,
        .poses = input.desired.has_value() ? input.desired->poses : std::vector<PresentationPose>{},
        .coverage = input.viewport.rasterViewport(),
        .failure = failure};
    if (acceptedRepairAttempt_.has_value() && outcome.failure != FramePresentationFailure::None &&
        resources->capture()->identity().captureId != lastRejectedRepairCapture_) {
      lastRejectedRepairCapture_ = resources->capture()->identity().captureId;
      nothingToPresentRetry_.noteFailure(*acceptedRepairAttempt_, schedulingNow());
    }
  } else {
    pendingRepair_.reset();
    acceptedRepairAttempt_.reset();
    nothingToPresentRetry_.reset();
  }
}

void RenderCoordinator::installFrameDecision(
    const FramePresentationInput& input,
    const std::shared_ptr<const GlTextureCache::PresentationResources>& resources,
    std::shared_ptr<const FramePresentation> next) {
  frameRepresentsCurrentIntent_ =
      (input.selection.empty() || next->hasSelectionGeometry()) && FrameCarriesInput(*next, input);
  framePresentation_ = std::move(next);
  immediateOverlaySnapshot_ = framePresentation_->chrome();
  selectionBoundsCache_.displayedBoundsDoc = framePresentation_->selectionBounds();
  if (!presentationNeedsRender_ && HasOnlyStaticFrameIntent(input)) {
    reusableFrameInput_ = input;
    reusableFrameResources_ = resources;
    reusableSelectionCapture_ = selectedSceneCapture_;
  } else {
    reusableFrameInput_.reset();
    reusableFrameResources_.reset();
    reusableSelectionCapture_.reset();
  }
}

std::shared_ptr<const FramePresentation> RenderCoordinator::buildFramePresentation(
    EditorApp& app, SelectTool& tool, const ViewportState& viewport, const Box2d& paneClipRect,
    SelectionChromeDetail detail, bool includeChrome, FramePresentationAdmission admit) {
  auto& cost = lastFrameCostBreakdown_.overlay;
  const auto live = tool.activeDragPreview();
  cost.hasLiveDragPreview = live.has_value();
  cost.liveDragTranslationDoc = live.has_value() ? live->translation : Vector2d::Zero();
  const auto resources = compositedPresentation_.resources();
  frameRepresentsCurrentIntent_ = false;
  if (!app.hasDocument()) {
    framePresentation_.reset();
    return nullptr;
  }
  if (resources == nullptr || resources->capture() == nullptr) {
    presentationNeedsRender_ = true;
    pendingRepair_ =
        PresentationRepairIdentity{.scene = currentPresentationIdentity(app),
                                   .selection = selectedPresentationEntities(app),
                                   .coverage = viewport.rasterViewport(),
                                   .failure = FramePresentationFailure::MissingResources};
    return framePresentation_;
  }
  const auto buildStart = std::chrono::steady_clock::now();
  auto input = makeFrameInput(app, tool, viewport, paneClipRect, detail, includeChrome);
  if (const auto selectedSource = FramePresentation::CaptureForFrame(resources, input)) {
    refreshFrameSelectionCapture(app, selectedSource, input.selection);
  }
  refreshLivePathCapture(app, input);
  if (canReuseFrame(input, resources)) {
    frameRepresentsCurrentIntent_ = true;
    recordFramePresentationCost(input, tool, MillisecondsSince(buildStart));
    return framePresentation_;
  }
  auto outcome =
      FramePresentation::Build(resources, input, selectedSceneCapture_, framePresentation_);
  PrepareCandidateFrame(outcome, admit);
  auto next = outcome.frame;
  updateFrameRepairStatus(input, resources, outcome);
  if (next != nullptr) {
    installFrameDecision(input, resources, std::move(next));
  }
  if (framePresentation_ != nullptr) {
    recordFramePresentationCost(input, tool, MillisecondsSince(buildStart));
  } else {
    cost.captureMs = MillisecondsSince(buildStart);
  }
  return framePresentation_;
}

bool RenderCoordinator::canReplaceWithOverview(const RenderResult& result,
                                               const EditorApp& app) const {
  if (!pendingRepair_ || !CompleteOverviewCanRepair(pendingRepair_->failure) ||
      result.capturedPresentation == nullptr ||
      !result.capturedPresentation->identity().sameContent(currentPresentationIdentity(app))) {
    return false;
  }
  const std::span<const RenderResult::CompositedTile> tiles =
      result.compositedPreview
          ? std::span<const RenderResult::CompositedTile>(result.compositedPreview->tiles)
          : std::span<const RenderResult::CompositedTile>();
  return FramePresentation::CanAdopt(
      *result.capturedPresentation, tiles, framePresentation_.get(),
      result.capturedPresentation->identity().sameScene(currentPresentationIdentity(app)) &&
          !app.document().hasPendingMutations());
}

void RenderCoordinator::acceptOverviewResult(RenderResult result, EditorApp& app,
                                             GlTextureCache& textures) {
  const bool replacement = canReplaceWithOverview(result, app);
  if (result.version != app.document().currentFrameVersion() && !replacement) {
    return;
  }
  if (!textures.tiles().empty() && replacement) {
    presentCompositedResult(result, app, result.viewport, textures);
    return;
  }
  if (!textures.tiles().empty()) {
    pendingOverviewResult_ = std::move(result);
    return;
  }
  if (!textures.uploadCompositedOverview(*result.compositedPreview, result.rasterViewport,
                                         result.capturedPresentation)) {
    renderWorker_.asyncRenderer.discardUnpresentedResult(result);
    rejectPreparedPresentation(FramePresentationFailure::UploadRefused);
    return;
  }
  adoptionFailure_ = FramePresentationFailure::None;
  acceptedRepairAttempt_ = lastPostedAttempt_;
  lastFrameCostBreakdown_.compositedUpload = textures.lastCompositedUploadCost();
  compositedPresentation_.notePreparedResources(
      textures.presentationResources(), result.compositedPreview->entity,
      DragPreviewFromRenderRequest(result.compositedPreview->representedDragPreview),
      result.capturedPresentation != nullptr &&
          result.capturedPresentation->identity().sameScene(currentPresentationIdentity(app)) &&
          !app.document().hasPendingMutations());
  overviewDocVersion_ = result.version;
  displayedDocVersion_ = result.version;
#ifdef __EMSCRIPTEN__
  PublishAcceptedWorkerResult(result);
#endif
}

bool RenderCoordinator::hasMatchingPendingOverview(const RenderResult& result,
                                                   EditorApp& app) const {
  return IsCurrentRenderResult(pendingOverviewResult_, app) &&
         pendingOverviewResult_->version == result.version &&
         result.version == app.document().currentFrameVersion() &&
         pendingOverviewResult_->capturedPresentation != nullptr &&
         result.capturedPresentation != nullptr &&
         pendingOverviewResult_->capturedPresentation->identity().sameScene(
             result.capturedPresentation->identity());
}

bool RenderCoordinator::canPresentWithOverview(const RenderResult& result,
                                               const EditorRasterViewport& rasterViewport,
                                               EditorApp& app,
                                               const GlTextureCache& textures) const {
  if (!result.rasterViewport.viewportBounded || !rasterViewport.viewportBounded) {
    return true;
  }
  const auto overview =
      hasMatchingPendingOverview(result, app)
          ? pendingOverviewResult_->capturedPresentation
          : (textures.presentationResources() ? textures.presentationResources()->overviewCapture()
                                              : nullptr);
  const auto active = result.capturedPresentation;
  return active != nullptr && overview != nullptr &&
         active->identity().sameScene(overview->identity()) &&
         active->canvasSize() == overview->canvasSize() &&
         active->documentOrigin() == overview->documentOrigin();
}

bool RenderCoordinator::requiresFreshOverview(bool available, std::uint64_t currentVersion) const {
  return !available || overviewDocVersion_ != currentVersion ||
         (pendingRepair_ && pendingRepair_->failure == FramePresentationFailure::MissingOverview);
}

void RenderCoordinator::discardStalePendingOverview(const EditorApp& app) {
  const auto currentVersion = app.document().currentFrameVersion();
  if (pendingOverviewResult_.has_value() &&
      (!IsCurrentRenderResult(pendingOverviewResult_, app) ||
       pendingOverviewResult_->version != currentVersion ||
       pendingOverviewResult_->capturedPresentation == nullptr ||
       !pendingOverviewResult_->capturedPresentation->identity().sameScene(
           currentPresentationIdentity(app)))) {
    pendingOverviewResult_.reset();
  }
}

bool RenderCoordinator::needsOverviewInfillForViewport(EditorApp& app,
                                                       const EditorRasterViewport& rasterViewport,
                                                       bool activeDrag,
                                                       const GlTextureCache* textures) {
  const auto currentVersion = app.document().currentFrameVersion();
  discardStalePendingOverview(app);
  const auto resources = textures != nullptr ? textures->presentationResources() : nullptr;
  const auto overview = resources ? resources->overviewCapture() : nullptr;
  const bool currentOverview =
      overview != nullptr && overview->identity().sameScene(currentPresentationIdentity(app));
  return rasterViewport.viewportBounded && (!activeDrag || presentationNeedsCoverage_) &&
         textures != nullptr && !pendingOverviewResult_.has_value() &&
         requiresFreshOverview(currentOverview, currentVersion);
}

void RenderCoordinator::pollRenderResult(EditorApp& app, const ViewportState& viewport,
                                         GlTextureCache& textures, FrameHistory* frameHistory) {
  ZoneScopedN("RenderCoordinator::pollRenderResult");
  const ScopedHeapDelta pollHeapDelta(MemoryStage::AppPollResult);
  auto resultOpt = renderWorker_.asyncRenderer.pollResult();
  noteMissingPixelCaptureResult(resultOpt);
#ifdef __EMSCRIPTEN__
  // Report a GPU-wait failure whether or not a frame landed. The completed
  // frame below carries the same fields, so consume the generation either way
  // and only publish separately when there is no frame to carry them.
  {
    const AsyncRenderer::GpuWaitFailure gpuWaitFailure =
        renderWorker_.asyncRenderer.gpuWaitFailure();
    if (gpuWaitFailure.generation != publishedGpuWaitGeneration_) {
      publishedGpuWaitGeneration_ = gpuWaitFailure.generation;
      if (!resultOpt.has_value()) {
        PublishWorkerGpuWaitFailure(gpuWaitFailure.deviceLost,
                                    GpuWaitTimeoutSiteName(gpuWaitFailure.timedOutWaitSite),
                                    gpuWaitFailure.timedOutWaitMs);
      }
    }
  }
#endif
  if (!IsCurrentRenderResult(resultOpt, app)) {
    rejectRenderResult(resultOpt);
    return;
  }

  // Renderer configuration belongs to the consumed capture, not to the most recently posted work.
  if (resultOpt->capturedPresentation != nullptr &&
      resultOpt->capturedPresentation->identity().presentationEpoch != presentationEpoch_) {
    rejectRenderResult(resultOpt);
    return;
  }

  const auto& result = *resultOpt;
  const auto compositorStats = renderWorker_.asyncRenderer.compositorRenderFrameStats();
#ifdef __EMSCRIPTEN__
  PublishWorkerTimingStats(result, app, compositorStats);
#endif
  lastFrameCostBreakdown_.compositedRender = CompositedRenderCostFromStats(compositorStats);
  lastFrameCostBreakdown_.compositedRender.presentationCoverageRepair =
      result.presentationCoverageRepair;
  lastFrameCostBreakdown_.compositedRender.presentationRepairReason =
      result.presentationRepairReason;
  // Forward the worker-measured presentation latency to the frame history so
  // `RenderFrameGraph` can overlay async worker time on the UI frame graph.
  // The frame history's latest slot corresponds to the current UI frame
  // (pushed at the top of `EditorShell::runFrame` before poll) - a landed
  // result belongs to that frame.
  if (frameHistory != nullptr) {
    frameHistory->setLatestBackendMs(static_cast<float>(result.workerMs));
  }
  if (!result.compositedPreview.has_value() || !result.compositedPreview->valid()) {
    noteResultWithNothingToPresent(resultOpt);
    if (hasMatchingPendingOverview(result, app)) {
      RenderResult overview = std::move(*pendingOverviewResult_);
      pendingOverviewResult_.reset();
      presentCompositedResult(overview, app, viewport, textures);
    }
    return;
  }
  const EditorRasterViewport rasterViewport = viewport.rasterViewport();
  const bool overviewInfillResult =
      result.overviewInfillOnly && !result.rasterViewport.viewportBounded;
  if (overviewInfillResult && rasterViewport.viewportBounded) {
    acceptOverviewResult(std::move(*resultOpt), app, textures);
    return;
  }
  if (!RasterViewportCanPresentCurrentViewport(result.rasterViewport, rasterViewport)) {
    rejectRenderResult(resultOpt);
    return;
  }
  if (!canPresentWithOverview(result, rasterViewport, app, textures)) {
    // A viewport-bounded result covers only the currently rasterized window. Never make it the
    // sole presented content: zooming out would expose checkerboard for missing tile coverage
    // instead of document transparency. Keep the previous presentation until an overview infill
    // exists underneath the crisp bounded tiles.
    renderWorker_.asyncRenderer.discardUnpresentedResult(result);
    rejectPreparedPresentation(FramePresentationFailure::MissingOverview);
    return;
  }
  const Vector2i resultCanvasSize = result.rasterViewport.outputSizePx;
  if (!ShouldPresentCompositedPreviewForViewport(*result.compositedPreview, resultCanvasSize)) {
    rejectRenderResult(resultOpt);
    return;
  }

  presentCompositedResult(*resultOpt, app, viewport, textures);
}

void RenderCoordinator::rejectPreparedPresentation(FramePresentationFailure failure) {
  adoptionFailure_ = failure;
  presentationNeedsRender_ = true;
  if (lastPostedAttempt_.has_value()) {
    nothingToPresentRetry_.noteFailure(*lastPostedAttempt_, schedulingNow());
  }
}

bool RenderCoordinator::prepareResultResources(RenderResult& result, EditorApp& app,
                                               GlTextureCache& textures) {
  if (result.capturedPresentation == nullptr ||
      !FramePresentation::CanAdopt(
          *result.capturedPresentation, result.compositedPreview->tiles, framePresentation_.get(),
          result.capturedPresentation->identity().sameScene(currentPresentationIdentity(app)) &&
              !app.document().hasPendingMutations())) {
    renderWorker_.asyncRenderer.discardUnpresentedResult(result);
    rejectPreparedPresentation(FramePresentationFailure::IncompatiblePose);
    return false;
  }
  const RenderResult* overview =
      hasMatchingPendingOverview(result, app) ? &*pendingOverviewResult_ : nullptr;
  if (!textures.uploadComposited(*result.compositedPreview, result.rasterViewport, overview,
                                 result.capturedPresentation)) {
    renderWorker_.asyncRenderer.discardUnpresentedResult(result);
    rejectPreparedPresentation(FramePresentationFailure::UploadRefused);
    return false;
  }
  adoptionFailure_ = FramePresentationFailure::None;
  acceptedRepairAttempt_ = lastPostedAttempt_;
  if (overview != nullptr) {
    overviewDocVersion_ = overview->version;
    pendingOverviewResult_.reset();
  }
  return true;
}

void RenderCoordinator::presentCompositedResult(RenderResult& result, EditorApp& app,
                                                const ViewportState& viewport,
                                                GlTextureCache& textures) {
  const Vector2i resultCanvasSize = result.rasterViewport.outputSizePx;
  if (!prepareResultResources(result, app, textures)) {
    return;
  }
  if (result.overviewInfillOnly) {
    renderWorker_.asyncRenderer.noteOverviewPresentedAsActive(result);
  }
  lastFrameCostBreakdown_.compositedUpload = textures.lastCompositedUploadCost();
  noteSelectedPrewarmResultPresented(result);
  if (!result.rasterViewport.viewportBounded) {
    overviewDocVersion_ = result.version;
    pendingOverviewResult_.reset();
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
  compositedPresentation_.notePreparedResources(
      textures.presentationResources(), result.compositedPreview->entity,
      DragPreviewFromRenderRequest(result.compositedPreview->representedDragPreview),
      result.capturedPresentation != nullptr &&
          result.capturedPresentation->identity().sameScene(currentPresentationIdentity(app)) &&
          !app.document().hasPendingMutations());
  noteSelectedPromotionAvailability(result);
  if (CompositedPreviewClearsPendingSelectedLayerRasterization(
          *result.compositedPreview, pendingSelectedLayerRasterizationEntity_, result.version,
          pendingSelectedLayerRasterizationVersion_)) {
    pendingSelectedLayerRasterizationEntity_ = entt::null;
    pendingSelectedLayerRasterizationVersion_ = 0;
  }

  displayedDocVersion_ = result.version;
#ifdef __EMSCRIPTEN__
  PublishAcceptedWorkerResult(result);
#endif
  renderScheduler_.noteRenderCompleted(result.version, resultCanvasSize, result.rasterViewport);
  promoteSelectionBoundsIfReady();
  acceptPixelCaptureResult(result, app, viewport);
}

std::optional<SelectTool::ActiveDragPreview> RenderCoordinator::previewWithPresentationEpoch(
    std::optional<SelectTool::ActiveDragPreview> preview) const {
  if (preview.has_value()) {
    preview->contentIdentity.presentationEpoch = presentationEpoch_;
  }
  return preview;
}

void RenderCoordinator::capturePresentationRequest(RenderRequest& request,
                                                   const EditorApp& app) const {
  request.version = app.document().currentFrameVersion();
  request.documentGeneration = app.document().documentGeneration();
  request.fontResourceRevision = app.document().fontResourceRevision();
  request.geometryRevision = app.document().nonTransformRevision();
  request.presentationEpoch = presentationEpoch_;
  request.selectedElements = app.selectedElements();
  request.sourceHoverElements = sourceHoverElements_;
  request.textEditing = textEditing_;
  if (lockedRejectionFlash_.has_value()) {
    request.lockedFlash =
        LockedRejectionFlashInput{lockedRejectionFlash_->element, lockedRejectionFlash_->intensity};
  }
  if (framePresentation_ != nullptr) {
    for (const auto& pose : framePresentation_->overrides()) {
      request.trackedPresentationObjects.push_back(pose.entity);
    }
  }
}

bool RenderCoordinator::hasForcedRenderReason(bool captureNeeded) const {
  return pendingPresentationRefresh_ || presentationNeedsRender_ || captureNeeded ||
         selectedPrewarmRecoveryPending_;
}

void RenderCoordinator::configurePresentationRepair(RenderRequest& request) const {
  request.presentationRepairReason = pendingRepair_ ? static_cast<int>(pendingRepair_->failure) : 0;
  request.presentationCoverageRepair =
      presentationNeedsCoverage_ ||
      (pendingRepair_ && pendingRepair_->failure == FramePresentationFailure::IncompatiblePose);
}

bool RenderCoordinator::maybeRequestRender(EditorApp& app, SelectTool& selectTool,
                                           const ViewportState& viewport, GlTextureCache* textures,
                                           bool supersedeInFlight,
                                           SelectionChromeDetail directSurfaceSelectionDetail) {
  ZoneScopedN("RenderCoordinator::maybeRequestRender");
  if (!app.hasDocument() || viewport.paneSize.x <= 0.0 || viewport.paneSize.y <= 0.0) {
    return false;
  }

  const EditorRasterViewport rasterViewport = viewport.rasterViewport();
  const Vector2i desiredCanvasSize = rasterViewport.semanticCanvasSizePx;
  const Vector2i actualDocumentCanvas = app.document().document().canvasSize();
  const auto dragPreview = previewWithPresentationEpoch(selectTool.activeDragPreview());
  // Compare desired size against the live document size. Document replacement
  // can reset the stored canvas size, and LayoutSystem readback can round by a
  // pixel, so a separate last-set tracker is not authoritative here.
  const auto now = schedulingNow();
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
  updatePixelCaptureCanvasCommitWake(wouldChange, firstCommit, deferCanvasCommitForActiveDrag);
  const auto currentVersion = app.document().currentFrameVersion();
  DocumentPixelCaptureIdentity desiredCapture;
  bool captureNeeded = preparePixelCaptureRequest(app, viewport, &desiredCapture);
  const Entity prewarmEntity = selectedCompositedEntity(app);
  const bool useVisibleSelectedRaster = selectedPrewarmFallbackApplies(
      app.document().documentGeneration(), prewarmEntity, rasterViewport);
  const bool willCommitCanvas =
      ShouldCommitCanvas(pendingCanvasSize_, wouldChange, deferCanvasCommitForActiveDrag,
                         firstCommit, throttleElapsed);
  const bool needsOverviewInfill =
      needsOverviewInfillForViewport(app, rasterViewport, dragPreview.has_value(), textures) ||
      (textures != nullptr && rasterViewport.viewportBounded && willCommitCanvas);
  const bool pendingSelectedLayerRasterization =
      prewarmEntity != entt::null && prewarmEntity == pendingSelectedLayerRasterizationEntity_;
  const bool deferSelectedViewportRefresh = ShouldDeferSelectedViewportRefresh(
      prewarmEntity, dragPreview.has_value(), currentVersion, displayedDocVersion_,
      compositedPresentation_.hasCachedTextures(), rasterViewportSettled, needsOverviewInfill,
      pendingSelectedLayerRasterization || selectedPrewarmRecoveryPending_);
  if (shouldDeferViewportRender(deferSelectedViewportRefresh, needsOverviewInfill, captureNeeded)) {
    if (renderWorker_.asyncRenderer.isBusy()) {
      renderWorker_.asyncRenderer.cancelInFlight();
    }
    return false;
  }
  if (renderWorker_.asyncRenderer.isBusy() && !supersedeInFlight) {
    return false;
  }

  const bool requestOverviewInfill = needsOverviewInfill;
  const std::vector<Entity> prewarmExtraEntities =
      requestOverviewInfill ? std::vector<Entity>{}
                            : selectedCompositedExtraEntities(app, prewarmEntity);
  const bool forceSelectedLayerRasterization = pendingSelectedLayerRasterization;
  bool forcePresentationRefresh = hasForcedRenderReason(captureNeeded);
  const bool hasIndependentSelectedPrewarmRenderReason = HasIndependentSelectedPrewarmRenderReason(
      dragPreview.has_value(), currentVersion != displayedDocVersion_,
      forceSelectedLayerRasterization, forcePresentationRefresh);
  const bool selectionOnlyPrewarmAllowed = shouldRequestSelectionOnlyPrewarm(
      app.document().documentGeneration(), prewarmEntity, currentVersion, rasterViewport);
  const EditorRasterViewport selectedPrewarmRaster = viewport.selectedPrewarmRasterViewport();
  const bool useSelectedPrewarmRasterViewport =
      !documentPixelCaptureEnabled_ && !useVisibleSelectedRaster &&
      ShouldUseSelectedPrewarmRasterViewport(
          prewarmEntity, requestOverviewInfill, rasterViewport.viewportBounded,
          selectionOnlyPrewarmAllowed, hasIndependentSelectedPrewarmRenderReason,
          HasCompleteVisibleCachedCoverage(textures, viewport, rasterViewport),
          rasterViewport.outputSizePx, selectedPrewarmRaster.outputSizePx);
  const EditorRasterViewport requestRasterViewport =
      requestOverviewInfill
          ? viewport.overviewInfillRasterViewport()
          : (useSelectedPrewarmRasterViewport ? selectedPrewarmRaster : rasterViewport);
  const Vector2i currentCanvasSize = requestRasterViewport.outputSizePx;

  if (willCommitCanvas) {
    app.document().document().setCanvasSize(pendingCanvasSize_.x, pendingCanvasSize_.y);
    pendingCanvasSizeSince_ = now;
    ++lastFrameCostBreakdown_.documentCanvasCommitCount;
    ++documentCanvasCommitTotal_;
    lastFrameCostBreakdown_.lastCommittedCanvasSize = pendingCanvasSize_;
    noteCanvasSizeCommitForPixelCapture(app, viewport, &desiredCapture, &captureNeeded,
                                        &forcePresentationRefresh);
  }

  const Entity suppressedLayerEntity = suppressedCompositedLayerEntity(app);
  if (suppressedLayerEntity != entt::null) {
    compositedPresentation_.discardCachedTexturesForEntity(suppressedLayerEntity);
  }

  // A composited result can carry the selected entity without a promoted drag-target tile (for
  // example, when a mask forces its child into an owning layer). In that case the presenter has
  // no bitmap to move on the UI thread. Render the changing document during the held drag rather
  // than treating the selection's cached metadata as proof that its pixels can move locally.
  const bool renderDragFallback =
      activeDragNeedsRenderedPresentation(app, dragPreview, textures, suppressedLayerEntity);

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
          // A scheduled retry owes a render even when the failed request's only reason for one,
          // such as a presentation refresh, was consumed when it was posted.
          .forcePresentationRefresh = forcePresentationRefresh || requestOverviewInfill ||
                                      nothingToPresentRetry_.retryScheduled(),
          .forceSelectedLayerRasterization = forceSelectedLayerRasterization,
          .currentVersion = currentVersion,
          .currentCanvasSize = currentCanvasSize,
          .currentRasterViewport = requestRasterViewport,
          .dragTranslationRecaptureDistanceDoc =
              kDragTranslationRecaptureScreenPx /
              std::max(std::abs(viewport.pixelsPerDocUnit()), 1e-9),
          .requiresRenderedActiveDragPresentation = renderDragFallback,
          .selectionOnlyPrewarmMayTriggerRender = selectionOnlyPrewarmAllowed,
      });
  if (!schedule.shouldRequestRender()) {
    return false;
  }
  RenderAttemptIdentity attempt = MakeRenderAttempt(
      app.document().documentGeneration(), currentVersion, requestRasterViewport,
      requestOverviewInfill, prewarmEntity, schedule.dragPreview, presentationEpoch_);
  attempt.repair = pendingRepair_;
  if (!nothingToPresentRetry_.mayPost(attempt, schedulingNow())) {
    return false;
  }

  RenderRequest req(renderWorker_.renderer, app.document().document());
  capturePresentationRequest(req, app);
  req.rasterViewport = requestRasterViewport;
  // The presenter places the accepted surface with this transform, so it must be
  // the exact viewport `requestRasterViewport` was derived from.
  req.viewport = viewport;
  req.overviewInfillOnly = requestOverviewInfill;
  configurePresentationRepair(req);
  configurePixelCaptureRequest(&req, requestOverviewInfill, dragPreview.has_value(),
                               requestRasterViewport);
  // Drain any pending structural remap from a recent `setDocumentMaybe
  // Structural` call. Non-empty remap lets the worker preserve the
  // compositor's cached state across the document swap instead of
  // falling into the full-reset path. Must be consumed on every render
  // request - a second render without consumption would re-apply a
  // stale remap against an already-remapped compositor.
  req.structuralRemap = app.document().consumePendingStructuralRemap();
  req.selection = std::nullopt;
  // Carry the current renderable selection on every render so the compositor can keep the selected
  // entity promoted across drag → idle → drag transitions. A selected `display:none` element keeps
  // editor chrome but must not keep or refresh a promoted content layer.
  req.selectedEntity = attempt.selectedEntity;
  req.dragPreview = attempt.dragPreview;
  ++lastFrameCostBreakdown_.renderRequestsPosted;
  if (requestOverviewInfill) {
    ++overviewInfillRenderTotal_;
  }
  renderWorker_.asyncRenderer.requestRender(req);
  lastPostedAttempt_ = attempt;
  recordPixelCaptureRequest(req, desiredCapture);
  pendingPresentationRefresh_ = false;
  return true;
}

void RenderCoordinator::invalidatePresentationAfterDocumentFlush(
    EditorApp& app, const AsyncSVGDocument::FlushResult& flushResult) {
  const Entity selectedEntity = selectedCompositedEntity(app);
  if (selectedEntity == entt::null) {
    return;
  }

  if (std::find(flushResult.cacheInvalidatedElements.begin(),
                flushResult.cacheInvalidatedElements.end(),
                selectedEntity) != flushResult.cacheInvalidatedElements.end()) {
    pendingSelectedLayerRasterizationEntity_ = selectedEntity;
    pendingSelectedLayerRasterizationVersion_ = app.document().currentFrameVersion();
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

bool RenderCoordinator::activeDragNeedsRenderedPresentation(
    EditorApp& app, const std::optional<SelectTool::ActiveDragPreview>& dragPreview,
    const GlTextureCache* textures, Entity suppressedLayerEntity) const {
  if (!dragPreview.has_value()) {
    return false;
  }
  if (textures == nullptr || presentationNeedsRender_ || framePresentation_ == nullptr ||
      !framePresentation_->followsPointer()) {
    return true;
  }
  return !HasPresentableDragTargetTile(*textures, dragPreview, suppressedLayerEntity,
                                       selectedElementIsDisplayNone(app));
}

Entity RenderCoordinator::selectedCompositedEntityForDiagnostics(EditorApp& app) const {
  if (!app.hasDocument() || !app.selectedElement().has_value()) {
    return entt::null;
  }

  const svg::SVGElement& selected = *app.selectedElement();
  return selected.withReadAccess(
      [&selected](svg::DocumentReadAccess&, EntityHandle handle) -> Entity {
        if (!selected.isa<svg::SVGGraphicsElement>() || IsDisplayNone(selected)) {
          return entt::null;
        }
        return handle.entity();
      });
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
  std::optional<svg::DocumentReadAccess> documentAccess =
      app.hasDocument() ? app.document().document().tryReadAccess() : std::nullopt;
  if (app.hasDocument() && !documentAccess.has_value()) {
    return displayNoneSuppressedLayerEntity_;
  }
  const std::optional<svg::SVGElement> selected = SelectedGraphicsElement(app);
  if (!selected.has_value()) {
    return suppressedLayerWithoutSelection(app);
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

  return suppressedLayerForHiddenSelection(*selected);
}

Entity RenderCoordinator::suppressedLayerWithoutSelection(EditorApp& app) {
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

Entity RenderCoordinator::suppressedLayerForHiddenSelection(const svg::SVGElement& selected) {
  const Entity selectedEntity = selected.unsafeEntityHandle().entity();
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
  if (!app.hasDocument() || !app.selectedElement().has_value()) {
    return false;
  }
  const auto documentAccess = app.document().document().tryReadAccess();
  if (!documentAccess.has_value()) {
    return displayNoneSuppressedSelectionEntity_ != entt::null &&
           app.selectedElement()->unsafeEntityHandle().entity() ==
               displayNoneSuppressedSelectionEntity_;
  }
  const std::optional<svg::SVGElement> selected = SelectedGraphicsElement(app);
  return selected.has_value() && IsDisplayNone(*selected);
}

}  // namespace donner::editor
