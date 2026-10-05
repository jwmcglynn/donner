#pragma once
/// @file
/// Private-state access to \ref donner::editor::RenderCoordinator shared by the editor test suites.

#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>

#include "donner/editor/EditorApp.h"
#include "donner/editor/RenderCoordinator.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

struct RenderCoordinatorTestAccess {
  static std::shared_ptr<const CapturedPresentation> pendingOverviewCapture(
      const RenderCoordinator& coordinator) {
    return coordinator.pendingOverviewResult_
               ? coordinator.pendingOverviewResult_->capturedPresentation
               : nullptr;
  }

  static std::shared_ptr<const CapturedPresentation> installedOverviewCapture(
      const RenderCoordinator& coordinator) {
    const auto resources = coordinator.compositedPresentation_.resources();
    return resources ? resources->overviewCapture() : nullptr;
  }

  static std::string overviewScheduleState(const RenderCoordinator& coordinator,
                                           const EditorApp& app, const ViewportState& viewport) {
    const auto cache = coordinator.compositedPresentation_.diagnostics();
    std::ostringstream out;
    out << "current=" << app.document().currentFrameVersion()
        << " actualCanvas=" << app.document().document().canvasSize()
        << " pendingCanvas=" << coordinator.pendingCanvasSize_
        << " commits=" << coordinator.documentCanvasCommitTotal_
        << " displayed=" << coordinator.displayedDocVersion_
        << " overview=" << coordinator.overviewDocVersion_ << " cached=" << cache.cachedVersion
        << " cachedSize=" << cache.cachedCanvasSize
        << " forceRefresh=" << coordinator.pendingPresentationRefresh_
        << " repair=" << coordinator.presentationNeedsRender_
        << " coverage=" << coordinator.presentationNeedsCoverage_
        << " selectedRaster=" << coordinator.pendingSelectedLayerRasterizationVersion_
        << " prewarmRecovery=" << coordinator.selectedPrewarmRecoveryPending_
        << " retry=" << coordinator.nothingToPresentRetry_.retryScheduled()
        << " visible=" << viewport.rasterViewport().outputSizePx
        << " prewarm=" << viewport.selectedPrewarmRasterViewport().outputSizePx;
    const auto resources = coordinator.compositedPresentation_.resources();
    if (resources && resources->capture()) {
      const auto identity = resources->capture()->identity();
      out << " primary=" << identity.captureId << '/' << identity.version << '/'
          << identity.geometryRevision;
    }
    if (resources && resources->overviewCapture()) {
      const auto identity = resources->overviewCapture()->identity();
      out << " overviewCapture=" << identity.captureId << '/' << identity.version << '/'
          << identity.geometryRevision;
    }
    if (coordinator.lastPostedAttempt_) {
      const auto& attempt = *coordinator.lastPostedAttempt_;
      out << " posted=" << attempt.version << " infill=" << attempt.overviewInfillOnly
          << " raster=" << attempt.rasterViewport.outputSizePx;
    }
    return out.str();
  }

  static void seedPreCommitPixelCapture(RenderCoordinator& coordinator, const EditorApp& app,
                                        const ViewportState& viewport) {
    coordinator.documentPixelCaptureEnabled_ = true;
    coordinator.documentPixelCaptureSessionId_ = 1;
    const DocumentPixelCaptureIdentity identity{
        .sessionId = 1,
        .documentGeneration = app.document().documentGeneration(),
        .version = app.document().currentFrameVersion(),
        .fontResourceRevision = app.document().fontResourceRevision(),
        .canvasCommitGeneration = coordinator.documentCanvasCommitTotal_,
        .rasterViewport = viewport.rasterViewport(),
        .viewport = viewport,
    };
    coordinator.documentPixelCapture_ = DocumentPixelCapture{.identity = identity};
    coordinator.requestedPixelCapture_ = identity;
    coordinator.pendingCanvasSize_ = viewport.rasterViewport().semanticCanvasSizePx;
    coordinator.pendingCanvasSizeSince_ = std::chrono::steady_clock::now();
  }

  static void keepCanvasCommitPending(RenderCoordinator& coordinator) {
    coordinator.pendingCanvasSizeSince_ = std::chrono::steady_clock::now() + std::chrono::hours(1);
  }

  static void makeCanvasCommitDue(RenderCoordinator& coordinator) {
    coordinator.pendingCanvasSizeSince_ =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(200);
  }

  static void makeRasterViewportSettled(RenderCoordinator& coordinator) {
    coordinator.pendingRasterViewportSince_ =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(200);
  }

  /// Replaces the steady clock that paces nothing-to-present retries with one the test advances.
  static void useFakeRetryClock(RenderCoordinator& coordinator) {
    fakeRetryNow = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    coordinator.nothingToPresentRetryClockForTesting_ = &FakeRetryNow;
  }

  static void advanceFakeRetryClock(std::chrono::milliseconds step) { fakeRetryNow += step; }

  static std::chrono::steady_clock::time_point FakeRetryNow() { return fakeRetryNow; }

  static bool rejectPreparedResult(RenderCoordinator& coordinator, RenderResult& result,
                                   EditorApp& app, GlTextureCache& textures) {
    coordinator.lastPostedAttempt_ = RenderAttemptIdentity{
        .documentGeneration = result.documentGeneration,
        .version = result.version,
        .rasterViewport = result.rasterViewport,
    };
    return coordinator.prepareResultResources(result, app, textures);
  }

  static bool canReplaceWithOverview(const RenderCoordinator& coordinator,
                                     const RenderResult& result, const EditorApp& app) {
    return coordinator.canReplaceWithOverview(result, app);
  }

  static void changePendingRepairFailure(RenderCoordinator& coordinator,
                                         FramePresentationFailure failure) {
    coordinator.pendingRepair_->failure = failure;
  }

  static bool requiresFreshOverview(RenderCoordinator& coordinator, bool available,
                                    std::uint64_t currentVersion) {
    coordinator.overviewDocVersion_ = currentVersion;
    return coordinator.requiresFreshOverview(available, currentVersion);
  }

  static bool canPresentWithOverview(const RenderCoordinator& coordinator,
                                     const RenderResult& result, const EditorRasterViewport& raster,
                                     EditorApp& app, const GlTextureCache& cache) {
    return coordinator.canPresentWithOverview(result, raster, app, cache);
  }

  static bool matchingPendingOverview(RenderCoordinator& coordinator,
                                      const std::shared_ptr<const CapturedPresentation>& overview,
                                      RenderResult& result, EditorApp& app) {
    coordinator.pendingOverviewResult_.emplace();
    coordinator.pendingOverviewResult_->version = result.version;
    coordinator.pendingOverviewResult_->documentGeneration = result.documentGeneration;
    coordinator.pendingOverviewResult_->fontResourceRevision = result.fontResourceRevision;
    coordinator.pendingOverviewResult_->capturedPresentation = overview;
    return coordinator.hasMatchingPendingOverview(result, app);
  }

  static inline std::chrono::steady_clock::time_point fakeRetryNow{};

  static std::optional<std::uint64_t> requestedCommitGeneration(
      const RenderCoordinator& coordinator) {
    if (!coordinator.requestedPixelCapture_.has_value()) {
      return std::nullopt;
    }
    return coordinator.requestedPixelCapture_->canvasCommitGeneration;
  }

  static void noteSelectedPrewarmFailure(RenderCoordinator& coordinator,
                                         std::uint64_t documentGeneration, std::uint64_t version,
                                         Entity selectedEntity, const ViewportState& viewport) {
    const EditorRasterViewport expanded = viewport.selectedPrewarmRasterViewport();
    coordinator.lastPostedAttempt_ = RenderAttemptIdentity{
        .documentGeneration = documentGeneration,
        .version = version,
        .rasterViewport = expanded,
        .selectedEntity = selectedEntity,
    };
    std::optional<RenderResult> result(std::in_place);
    result->documentGeneration = documentGeneration;
    result->version = version;
    result->rasterViewport = expanded;
    result->viewport = viewport;
    coordinator.noteResultWithNothingToPresent(result);
  }

  static std::optional<EditorRasterViewport> lastPostedRasterViewport(
      const RenderCoordinator& coordinator) {
    return coordinator.lastPostedAttempt_.has_value()
               ? std::optional<EditorRasterViewport>(coordinator.lastPostedAttempt_->rasterViewport)
               : std::nullopt;
  }

  static std::optional<RenderAttemptIdentity> lastPostedAttempt(
      const RenderCoordinator& coordinator) {
    return coordinator.lastPostedAttempt_;
  }

  static void noteRenderCompleted(RenderCoordinator& coordinator, std::uint64_t version,
                                  const EditorRasterViewport& rasterViewport) {
    coordinator.renderScheduler_.noteRenderCompleted(version, rasterViewport.outputSizePx,
                                                     rasterViewport);
  }

  static bool selectedPrewarmFallbackApplies(RenderCoordinator& coordinator,
                                             std::uint64_t documentGeneration,
                                             Entity selectedEntity,
                                             const EditorRasterViewport& visibleRaster) {
    return coordinator.selectedPrewarmFallbackApplies(documentGeneration, selectedEntity,
                                                      visibleRaster);
  }

  static bool selectedPrewarmRecoveryPending(const RenderCoordinator& coordinator) {
    return coordinator.selectedPrewarmRecoveryPending_;
  }
};

}  // namespace donner::editor
