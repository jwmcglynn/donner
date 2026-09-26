#pragma once
/// @file
/// Private-state access to \ref donner::editor::RenderCoordinator shared by the editor test suites.

#include <chrono>
#include <cstdint>
#include <optional>

#include "donner/editor/EditorApp.h"
#include "donner/editor/RenderCoordinator.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

struct RenderCoordinatorTestAccess {
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
