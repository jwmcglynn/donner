/// @file
///
/// Golden replay coverage for `.rnr` recordings against main's async
/// editor stack (`EditorApp` + `SelectTool` + `AsyncSVGDocument` +
/// `AsyncRenderer`). The first seeded repro is `filter_elm_disappear-3`
/// from issue #582: replay through the real editor path, stop at the
/// post-second-mouse-up checkpoint, and pin the landed bitmap against
/// a committed PNG.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/ExperimentalDragPresentation.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TextPatch.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererImageIO.h"  // IWYU pragma: keep
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::size_t kTargetMouseUpCount = 2;
constexpr char kReplayPath[] = "donner/editor/tests/filter_elm_disappear-3.rnr";
constexpr char kGoldenPath[] = "donner/editor/tests/testdata/filter_disappear_rnr3_after_mup2.png";

std::optional<std::uint64_t> ParseBisectFrame() {
  const char* value = std::getenv("BISECTION_FRAME");
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }

  char* end = nullptr;
  const std::uint64_t parsed = static_cast<std::uint64_t>(std::strtoull(value, &end, 10));
  if (end == value || (end != nullptr && *end != '\0')) {
    return std::nullopt;
  }

  return parsed;
}

std::filesystem::path DiagnosticOutputDir() {
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR"); dir != nullptr) {
    return std::filesystem::path(dir);
  }
  return std::filesystem::temp_directory_path();
}

std::string LoadFileOrEmpty(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return {};
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::filesystem::path ResolveSvgPath(const std::filesystem::path& reproPath,
                                     std::string_view recordingSvgPath) {
  const std::filesystem::path direct(recordingSvgPath);
  if (std::filesystem::exists(direct)) {
    return direct;
  }

  const std::filesystem::path alongside = reproPath.parent_path() / direct;
  if (std::filesystem::exists(alongside)) {
    return alongside;
  }

  return direct;
}

void ApplyRecordedViewport(ViewportState& viewport, const repro::ReproViewport& recordedViewport) {
  viewport.paneOrigin = Vector2d(recordedViewport.paneOriginX, recordedViewport.paneOriginY);
  viewport.paneSize = Vector2d(recordedViewport.paneSizeW, recordedViewport.paneSizeH);
  viewport.devicePixelRatio = recordedViewport.devicePixelRatio;
  viewport.zoom = recordedViewport.zoom;
  viewport.panDocPoint = Vector2d(recordedViewport.panDocX, recordedViewport.panDocY);
  viewport.panScreenPoint = Vector2d(recordedViewport.panScreenX, recordedViewport.panScreenY);
  viewport.documentViewBox = Box2d::FromXYWH(recordedViewport.viewBoxX, recordedViewport.viewBoxY,
                                             recordedViewport.viewBoxW, recordedViewport.viewBoxH);
}

MouseModifiers DecodeMouseModifiers(int modifierMask) {
  MouseModifiers modifiers;
  modifiers.shift = (modifierMask & (1 << 1)) != 0;
  return modifiers;
}

bool HasPrimaryShortcutModifier(int modifierMask) {
  return (modifierMask & (1 << 0)) != 0 || (modifierMask & (1 << 3)) != 0;
}

void DeleteCurrentSelection(EditorApp& app) {
  const std::vector<svg::SVGElement> selected = app.selectedElements();
  app.clearSelection();
  for (const auto& element : selected) {
    if (auto target = captureAttributeWritebackTarget(element); target.has_value()) {
      app.enqueueElementRemoveWriteback(
          EditorApp::CompletedElementRemoveWriteback{.target = *target});
    }
    app.applyMutation(EditorCommand::DeleteElementCommand(element));
  }
}

bool HandleKeyDown(EditorApp& app, const repro::ReproEvent& event) {
  const bool primaryModifier = HasPrimaryShortcutModifier(event.modifiers);
  const bool shiftHeld = (event.modifiers & (1 << 1)) != 0;

  switch (event.key) {
    case ImGuiKey_Z:
      if (!primaryModifier) {
        return false;
      }
      if (shiftHeld) {
        app.redo();
      } else if (app.canUndo()) {
        app.undo();
      }
      return true;

    case ImGuiKey_Escape:
      if (!app.hasSelection()) {
        return false;
      }
      app.clearSelection();
      return true;

    case ImGuiKey_Delete:
    case ImGuiKey_Backspace:
      if (!app.hasSelection()) {
        return false;
      }
      DeleteCurrentSelection(app);
      return true;

    default: return false;
  }
}

bool SyncCanvasSize(EditorApp& app, const ViewportState& viewport) {
  const Vector2i desired = viewport.desiredCanvasSize();
  const Vector2i current = app.document().document().canvasSize();
  if (desired == current) {
    return false;
  }

  app.document().document().setCanvasSize(desired.x, desired.y);
  return true;
}

/// Production-style debounced canvas-size commit. Mirrors
/// `RenderCoordinator::kCanvasSizeCommitDelay` (120 ms throttle) keyed
/// on each replay frame's `timestampSeconds` so the harness's commit
/// cadence matches the original gesture cadence regardless of how
/// fast the test runner walks frames.
constexpr double kCanvasSizeCommitDelayMs = 120.0;

bool SyncCanvasSizeDebounced(EditorApp& app, const ViewportState& viewport,
                             double frameTimestampSeconds, double* lastCommitTimestampSeconds) {
  const Vector2i desired = viewport.desiredCanvasSize();
  const Vector2i current = app.document().document().canvasSize();
  if (desired == current) {
    return false;
  }
  const bool firstCommit = current == Vector2i::Zero();
  const double elapsedMs = (frameTimestampSeconds - *lastCommitTimestampSeconds) * 1000.0;
  if (!firstCommit && elapsedMs < kCanvasSizeCommitDelayMs) {
    return false;
  }
  app.document().document().setCanvasSize(desired.x, desired.y);
  *lastCommitTimestampSeconds = frameTimestampSeconds;
  return true;
}

std::optional<RenderResult> RequestRenderAndWait(AsyncRenderer& asyncRenderer,
                                                 svg::Renderer& renderer, EditorApp& app,
                                                 SelectTool& selectTool) {
  RenderRequest request;
  request.renderer = &renderer;
  request.document = &app.document().document();
  request.version = app.document().currentFrameVersion();
  request.documentGeneration = app.document().documentGeneration();
  request.structuralRemap = app.document().consumePendingStructuralRemap();
  if (app.selectedElement().has_value() && app.selectedElement()->isa<svg::SVGGraphicsElement>()) {
    request.selectedEntity = app.selectedElement()->entityHandle().entity();
  }
  if (auto preview = selectTool.activeDragPreview(); preview.has_value()) {
    request.dragPreview = RenderRequest::DragPreview{
        .entity = preview->entity,
        .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
    };
  }

  asyncRenderer.requestRender(request);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    auto result = asyncRenderer.pollResult();
    if (result.has_value()) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return std::nullopt;
}

// Drain SelectTool's completed drag writeback, patch the SVG source
// in place, and dispatch a `ReplaceDocumentCommand(preserveUndoOnReparse=true)`.
// Mirrors `DocumentSyncController::applyPendingWritebacks` minus the
// TextEditor widget: the in-memory source is the equivalent of
// `textEditor.getText()`. Returns true if a writeback was applied —
// caller should request a render to pick up the new docGeneration.
bool DrainWritebackAndReparse(EditorApp& app, SelectTool& selectTool, std::string* source) {
  auto completed = selectTool.consumeCompletedDragWriteback();
  if (!completed.has_value()) {
    return false;
  }

  std::vector<TextPatch> patches;
  patches.reserve(1u + completed->extras.size());

  const auto appendPatchForTarget = [&](const AttributeWritebackTarget& target,
                                        const Transform2d& transform) {
    const RcString serialized = toSVGTransformString(transform);
    std::optional<TextPatch> patch;
    if (std::string_view(serialized).empty()) {
      patch = buildAttributeRemoveWriteback(*source, target, "transform");
    } else {
      patch = buildAttributeWriteback(*source, target, "transform", std::string_view(serialized));
    }
    if (patch.has_value()) {
      patches.push_back(*std::move(patch));
    }
  };

  appendPatchForTarget(completed->target, completed->transform);
  for (const auto& extra : completed->extras) {
    appendPatchForTarget(extra.target, extra.transform);
  }
  if (patches.empty()) {
    return false;
  }

  applyPatches(*source, patches);
  app.applyMutation(EditorCommand::ReplaceDocumentCommand(*source, /*preserveUndoOnReparse=*/true));
  return true;
}

class RnrReplayTest : public ::testing::Test {
protected:
  bool stopAfterAllFrames_ = false;
  // When true, the per-frame loop drains SelectTool's completed drag
  // writeback after the input events are processed and dispatches a
  // `ReplaceDocumentCommand` against an in-memory copy of the source
  // text. Production fires this from `DocumentSyncController::apply
  // PendingWritebacks` every frame; the legacy replay tests don't need
  // it (they assert on pre-writeback bitmaps), so default-off keeps
  // their behavior unchanged.
  bool drainWritebacksEachFrame_ = false;
  /// In the async harness only: when the slow-path mouseDown is
  /// deferred on `isBusy()`, call `AsyncRenderer::cancelInFlight()`
  /// to ask the worker to bail. Whether this actually helps the
  /// user-visible click latency depends on the §M4 cancel-poll
  /// granularity inside the compositor's `renderFrame`.
  bool cancelInFlightOnDeferredClick_ = false;
  /// In-memory mirror of the source pane's text. The writeback drain
  /// patches this in place and dispatches a ReplaceDocumentCommand
  /// using its bytes, mimicking `DocumentSyncController` without
  /// pulling in the TextEditor widget.
  std::string liveSource_;

  struct ReplaySnapshot {
    svg::RendererBitmap bitmap;
    std::uint64_t frameIndex = 0;
    std::size_t mouseUpCount = 0;
    bool stoppedForBisection = false;
    std::filesystem::path diagnosticPath;
    std::uint64_t compositorReconstructCount = 0;
  };

  /// Per-mouse-down measurement for `ReplayRecordingAsync`.
  struct AsyncMouseDownLatency {
    std::uint64_t mouseDownFrameIndex = 0;
    /// Wall-clock from bufferPendingClick to the first render result
    /// landing AFTER the slow-path mouseDown was dispatched and a
    /// drag-bearing render request was posted. With design doc 0034
    /// progressive rendering, this is typically the `Intermediate`
    /// result emitted after the drag-target layer rasterize but
    /// before the canvas-sized refinement. Equivalent to click →
    /// first drag-pixel-visible in the live editor.
    double clickToFirstDragPixelMs = 0.0;
    /// Wall-clock from bufferPendingClick to the `Final`-stage
    /// render result landing. If progressive rendering isn't
    /// engaging (small canvas, no `Intermediate` emitted), equal to
    /// `clickToFirstDragPixelMs`. Otherwise typically much larger:
    /// the canvas-sized work runs in the background after the
    /// intermediate ships.
    double clickToFinalPixelMs = 0.0;
    /// Time from bufferPendingClick to the slow-path mouseDown firing
    /// (i.e. how long the click was deferred waiting for the worker
    /// to be `!isBusy()`). On the buggy code this is the dominant
    /// term of clickToFirstDragPixelMs at high zoom — the click
    /// waits for a multi-second prewarm to finish.
    double clickToSlowPathMs = 0.0;
  };

  struct AsyncReplaySnapshot {
    std::vector<AsyncMouseDownLatency> latencies;
    std::uint64_t rendersCompleted = 0;
    std::uint64_t rendersDeferredOnBusy = 0;
    double maxWorkerRenderMs = 0.0;
  };

  void ReplayRecording(const std::filesystem::path& reproPath, ReplaySnapshot* out) {
    ASSERT_NE(out, nullptr);

    auto replay = repro::ReadReproFile(reproPath);
    ASSERT_TRUE(replay.has_value()) << "Failed to parse replay file: " << reproPath;

    const std::filesystem::path svgPath = ResolveSvgPath(reproPath, replay->metadata.svgPath);
    const std::string svgSource = LoadFileOrEmpty(svgPath);
    ASSERT_FALSE(svgSource.empty()) << "SVG source missing: " << svgPath;
    liveSource_ = svgSource;

    EditorApp app;
    ASSERT_TRUE(app.loadFromString(svgSource)) << "Failed to load SVG fixture: " << svgPath;

    auto documentViewBox = app.document().document().svgElement().viewBox();
    ASSERT_TRUE(documentViewBox.has_value()) << "Loaded SVG is missing a root viewBox";

    ViewportState viewport;
    viewport.documentViewBox = *documentViewBox;
    viewport.devicePixelRatio = replay->metadata.displayScale;
    viewport.paneOrigin = Vector2d::Zero();
    viewport.paneSize = Vector2d(static_cast<double>(replay->metadata.windowWidth),
                                 static_cast<double>(replay->metadata.windowHeight));
    viewport.resetTo100Percent();

    for (const auto& frame : replay->frames) {
      if (frame.viewport.has_value()) {
        ApplyRecordedViewport(viewport, *frame.viewport);
        break;
      }
    }

    SelectTool selectTool;
    selectTool.setCompositedDragPreviewEnabled(replay->metadata.experimentalMode);

    AsyncRenderer asyncRenderer;
    svg::Renderer renderer;

    SyncCanvasSize(app, viewport);
    auto initialResult = RequestRenderAndWait(asyncRenderer, renderer, app, selectTool);
    ASSERT_TRUE(initialResult.has_value()) << "Initial render timed out";
    ASSERT_FALSE(initialResult->bitmap.empty()) << "Initial render produced an empty bitmap";

    out->bitmap = initialResult->bitmap;
    out->frameIndex = 0;
    out->mouseUpCount = 0;

    const std::optional<std::uint64_t> bisectFrame = ParseBisectFrame();
    bool leftButtonHeld = false;

    for (const auto& frame : replay->frames) {
      bool frameNeedsRender = false;

      if (frame.viewport.has_value()) {
        ApplyRecordedViewport(viewport, *frame.viewport);
        frameNeedsRender |= SyncCanvasSize(app, viewport);
      }

      const Vector2d mouseScreen(frame.mouseX, frame.mouseY);
      const Vector2d mouseDoc = frame.mouseDocX.has_value() && frame.mouseDocY.has_value()
                                    ? Vector2d(*frame.mouseDocX, *frame.mouseDocY)
                                    : viewport.screenToDocument(mouseScreen);
      const bool nowHeld = (frame.mouseButtonMask & 1) != 0;

      for (const auto& event : frame.events) {
        switch (event.kind) {
          case repro::ReproEvent::Kind::MouseDown:
            if (event.mouseButton == 0) {
              selectTool.onMouseDown(app, mouseDoc, DecodeMouseModifiers(frame.modifiers));
              frameNeedsRender = true;
            }
            break;

          case repro::ReproEvent::Kind::MouseUp:
            if (event.mouseButton == 0) {
              selectTool.onMouseUp(app, mouseDoc);
              ++out->mouseUpCount;
              frameNeedsRender = true;
            }
            break;

          case repro::ReproEvent::Kind::KeyDown:
            frameNeedsRender |= HandleKeyDown(app, event);
            break;

          case repro::ReproEvent::Kind::Resize:
          case repro::ReproEvent::Kind::Char:
          case repro::ReproEvent::Kind::Wheel:
          case repro::ReproEvent::Kind::Focus:
          case repro::ReproEvent::Kind::KeyUp: break;
        }
      }

      if (nowHeld && leftButtonHeld) {
        selectTool.onMouseMove(app, mouseDoc, /*buttonHeld=*/true);
        frameNeedsRender = true;
      }
      leftButtonHeld = nowHeld;

      if (drainWritebacksEachFrame_) {
        frameNeedsRender |= DrainWritebackAndReparse(app, selectTool, &liveSource_);
      }

      frameNeedsRender |= app.flushFrame();

      if (frameNeedsRender) {
        auto result = RequestRenderAndWait(asyncRenderer, renderer, app, selectTool);
        ASSERT_TRUE(result.has_value()) << "Render timed out at replay frame " << frame.index;
        ASSERT_FALSE(result->bitmap.empty())
            << "Render produced an empty bitmap at frame " << frame.index;
        out->bitmap = result->bitmap;
      }

      out->frameIndex = frame.index;

      if (bisectFrame.has_value() && frame.index >= *bisectFrame) {
        out->stoppedForBisection = true;
        break;
      }
      if (!bisectFrame.has_value() && stopAfterAllFrames_ == false &&
          out->mouseUpCount >= kTargetMouseUpCount) {
        break;
      }
    }

    out->compositorReconstructCount = asyncRenderer.compositorReconstructCountForTesting();

    if (out->stoppedForBisection) {
      out->diagnosticPath = DiagnosticOutputDir() /
                            ("rnr_replay_bisect_frame_" + std::to_string(out->frameIndex) + ".png");
      ASSERT_FALSE(out->bitmap.empty()) << "No bitmap available to dump for BISECTION_FRAME";
      svg::RendererImageIO::writeRgbaPixelsToPngFile(
          out->diagnosticPath.string().c_str(), out->bitmap.pixels, out->bitmap.dimensions.x,
          out->bitmap.dimensions.y, out->bitmap.rowBytes / 4u);
    }
  }

  /// Async-mode replay that mirrors `EditorShell::runFrame`'s
  /// post-and-poll structure rather than the synchronous "post one
  /// request, block until result, advance frame" loop in
  /// `ReplayRecording`. Required to reproduce timing-sensitive bugs
  /// like the post-pinch drag-start hang: the live editor's
  /// `RenderCoordinator::maybeRequestRender` early-returns when
  /// `asyncRenderer_.isBusy()`, so renders only fire on idle frames
  /// and the worker accumulates a backlog of progressively-larger
  /// selection prewarms during continuous zoom. A click that lands
  /// while one of those prewarms is in flight is gated on
  /// `!isBusy()` (`EditorShell.cc` slow-path) and waits seconds for
  /// the prewarm to finish — exactly the bug we're chasing.
  ///
  /// The harness mirrors production's request-posting policy
  /// (selection prewarm + active drag + canvas/version invalidation)
  /// against a simplified `ExperimentalDragPresentation` state
  /// machine. GL upload is skipped — the bug lives in worker
  /// scheduling, not GL.
  void ReplayRecordingAsync(const std::filesystem::path& reproPath, AsyncReplaySnapshot* out) {
    ASSERT_NE(out, nullptr);

    auto replay = repro::ReadReproFile(reproPath);
    ASSERT_TRUE(replay.has_value()) << "Failed to parse replay file: " << reproPath;

    const std::filesystem::path svgPath = ResolveSvgPath(reproPath, replay->metadata.svgPath);
    const std::string svgSource = LoadFileOrEmpty(svgPath);
    ASSERT_FALSE(svgSource.empty()) << "SVG source missing: " << svgPath;
    liveSource_ = svgSource;

    EditorApp app;
    ASSERT_TRUE(app.loadFromString(svgSource)) << "Failed to load SVG fixture: " << svgPath;

    auto documentViewBox = app.document().document().svgElement().viewBox();
    ASSERT_TRUE(documentViewBox.has_value()) << "Loaded SVG is missing a root viewBox";

    ViewportState viewport;
    viewport.documentViewBox = *documentViewBox;
    viewport.devicePixelRatio = replay->metadata.displayScale;
    viewport.paneOrigin = Vector2d::Zero();
    viewport.paneSize = Vector2d(static_cast<double>(replay->metadata.windowWidth),
                                 static_cast<double>(replay->metadata.windowHeight));
    viewport.resetTo100Percent();
    for (const auto& frame : replay->frames) {
      if (frame.viewport.has_value()) {
        ApplyRecordedViewport(viewport, *frame.viewport);
        break;
      }
    }

    SelectTool selectTool;
    selectTool.setCompositedDragPreviewEnabled(replay->metadata.experimentalMode);

    AsyncRenderer asyncRenderer;
    svg::Renderer renderer;

    SyncCanvasSize(app, viewport);

    // Production-style state shadowed in the harness.
    ExperimentalDragPresentation experimentalDragPresentation;
    Vector2i lastRenderedCanvasSize = Vector2i::Zero();
    std::uint64_t lastRenderedVersion = 0;
    Entity lastRenderedSelectedEntity = entt::null;
    bool lastRenderedHadDrag = false;
    bool leftButtonHeld = false;
    double lastCanvasCommitTimestamp = 0.0;

    // Tracking the in-flight click measurement.
    struct PendingClick {
      Vector2d documentPoint = Vector2d::Zero();
      MouseModifiers modifiers;
      std::uint64_t frameIndex = 0;
      std::chrono::steady_clock::time_point queuedAt;
      bool dispatched = false;
      std::chrono::steady_clock::time_point dispatchedAt;
      bool awaitingFirstDragRender = false;
      // Design doc 0034 progressive rendering: track whether the
      // first result (intermediate or final) has landed. When it
      // does we record `clickToFirstDragPixelMs` and push a latency
      // entry; we keep the latency entry in-flight until the
      // matching `Final` result lands so we can fill in
      // `clickToFinalPixelMs`.
      bool firstDragRenderRecorded = false;
      std::size_t latencyIndex = 0;
    };
    std::optional<PendingClick> pendingClick;

    const auto pollAndRecord = [&]() {
      auto result = asyncRenderer.pollResult();
      if (!result.has_value()) {
        return;
      }
      ++out->rendersCompleted;
      out->maxWorkerRenderMs = std::max(out->maxWorkerRenderMs, result->workerMs);
      // First result after the click was dispatched — record the
      // click-to-first-pixel latency. With progressive rendering
      // this is the intermediate (drag-target layer fresh, segments
      // stale); the matching final result lands shortly after and
      // fills in `clickToFinalPixelMs`.
      if (pendingClick.has_value() && pendingClick->dispatched &&
          pendingClick->awaitingFirstDragRender && !pendingClick->firstDragRenderRecorded) {
        AsyncMouseDownLatency latency;
        latency.mouseDownFrameIndex = pendingClick->frameIndex;
        latency.clickToFirstDragPixelMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                      pendingClick->queuedAt)
                .count();
        latency.clickToSlowPathMs = std::chrono::duration<double, std::milli>(
                                        pendingClick->dispatchedAt - pendingClick->queuedAt)
                                        .count();
        out->latencies.push_back(latency);
        pendingClick->latencyIndex = out->latencies.size() - 1;
        pendingClick->firstDragRenderRecorded = true;
      }
      // Fill in `clickToFinalPixelMs` once the final lands and
      // close out the click tracking.
      if (pendingClick.has_value() && pendingClick->firstDragRenderRecorded &&
          result->stage == RenderResult::Stage::Final) {
        out->latencies[pendingClick->latencyIndex].clickToFinalPixelMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                      pendingClick->queuedAt)
                .count();
        pendingClick.reset();
      }
      // Update prewarm-cache state on landed result so future frames
      // don't re-fire the same render.
      if (result->compositedPreview.has_value()) {
        experimentalDragPresentation.noteCachedTextures(result->compositedPreview->entity,
                                                        result->version,
                                                        app.document().document().canvasSize());
      }
    };

    // Mirrors `RenderCoordinator::maybeRequestRender`'s posting
    // policy without the GL/overlay sub-steps. Skipped when the
    // worker is busy — exactly the production gate that lets a
    // long prewarm tie up the click slow-path.
    const auto maybePostRender = [&]() {
      if (asyncRenderer.isBusy()) {
        ++out->rendersDeferredOnBusy;
        return;
      }

      const Vector2i currentCanvasSize = app.document().document().canvasSize();
      const std::uint64_t currentVersion = app.document().currentFrameVersion();
      const auto dragPreview = selectTool.activeDragPreview();
      const Entity prewarmEntity =
          app.selectedElement().has_value() && app.selectedElement()->isa<svg::SVGGraphicsElement>()
              ? app.selectedElement()->entityHandle().entity()
              : entt::null;

      // Drop stale settling state on selection change. (Lighter than
      // production's full handling — sufficient for the bug.)
      experimentalDragPresentation.clearSettlingIfSelectionChanged(prewarmEntity,
                                                                   dragPreview.has_value());

      const bool selectionChanged = prewarmEntity != lastRenderedSelectedEntity;
      const bool canvasChanged = currentCanvasSize != lastRenderedCanvasSize;
      const bool versionChanged = currentVersion != lastRenderedVersion;
      const bool dragChanged = dragPreview.has_value() != lastRenderedHadDrag;

      const bool needsRegularRender = canvasChanged || versionChanged;
      const bool needsExperimentalPrewarm = experimentalDragPresentation.shouldPrewarm(
          prewarmEntity, currentVersion, currentCanvasSize, /*dragActive=*/false);

      if (!needsRegularRender && !needsExperimentalPrewarm && !dragChanged && !selectionChanged) {
        return;
      }

      RenderRequest req;
      req.renderer = &renderer;
      req.document = &app.document().document();
      req.version = currentVersion;
      req.documentGeneration = app.document().documentGeneration();
      req.structuralRemap = app.document().consumePendingStructuralRemap();
      if (prewarmEntity != entt::null) {
        req.selectedEntity = prewarmEntity;
      }
      if (dragPreview.has_value()) {
        req.dragPreview = RenderRequest::DragPreview{
            .entity = dragPreview->entity,
            .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
        };
      } else if (needsExperimentalPrewarm && prewarmEntity != entt::null) {
        req.dragPreview = RenderRequest::DragPreview{
            .entity = prewarmEntity,
            .interactionKind = svg::compositor::InteractionHint::Selection,
        };
        experimentalDragPresentation.notePrewarmAttempted(prewarmEntity, currentVersion,
                                                          currentCanvasSize);
      }
      asyncRenderer.requestRender(req);

      lastRenderedCanvasSize = currentCanvasSize;
      lastRenderedVersion = currentVersion;
      lastRenderedSelectedEntity = prewarmEntity;
      lastRenderedHadDrag = dragPreview.has_value();
    };

    // Track each per-mouse-down measurement separately (production
    // buffers ONE click at a time; if a second arrives before the
    // first dispatches the buffer gets overwritten — exactly what we
    // want to reproduce here too).
    std::vector<AsyncMouseDownLatency> capturedLatencies;
    const auto captureCurrentLatency = [&]() {
      if (pendingClick.has_value() && pendingClick->dispatched &&
          pendingClick->awaitingFirstDragRender) {
        // Will be captured by pollAndRecord on next result.
      }
    };
    (void)captureCurrentLatency;

    // Pace the replay loop against the recording's `timestampSeconds`.
    // Replay frames take microseconds to process while real renders
    // take ms-to-seconds, so without pacing the loop's busy-gate
    // skips every registry-mutating frame and MouseDown #2 overwrites
    // the still-undispatched MouseDown #1. With pacing, the harness
    // honors the original gesture cadence and each click gets a
    // chance to dispatch in its own idle window.
    const auto replayStart = std::chrono::steady_clock::now();
    for (const auto& frame : replay->frames) {
      const auto targetTime =
          replayStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(frame.timestampSeconds));
      while (std::chrono::steady_clock::now() < targetTime) {
        pollAndRecord();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      // pollResult is always race-safe; drain at start of every frame.
      pollAndRecord();

      // Viewport state (zoom/pan/pane) is harness-local — safe to
      // update even mid-render. The canvas-size COMMIT (which writes
      // into the registry via `SVGDocument::setCanvasSize`) is the
      // one that races with the worker, and is gated below.
      if (frame.viewport.has_value()) {
        ApplyRecordedViewport(viewport, *frame.viewport);
      }

      // Click buffering is harness-local. Buffer the click on the
      // frame it arrives regardless of worker state — production's
      // `ViewportInteractionController::bufferPendingClick` is also
      // not gated on `isBusy()`.
      const Vector2d mouseDoc =
          frame.mouseDocX.has_value() && frame.mouseDocY.has_value()
              ? Vector2d(*frame.mouseDocX, *frame.mouseDocY)
              : viewport.screenToDocument(Vector2d(frame.mouseX, frame.mouseY));
      const bool nowHeld = (frame.mouseButtonMask & 1) != 0;
      for (const auto& event : frame.events) {
        if (event.kind == repro::ReproEvent::Kind::MouseDown && event.mouseButton == 0) {
          PendingClick click;
          click.documentPoint = mouseDoc;
          click.modifiers = DecodeMouseModifiers(frame.modifiers);
          click.frameIndex = frame.index;
          click.queuedAt = std::chrono::steady_clock::now();
          pendingClick = click;
        }
      }

      // Everything below this point touches the registry — race-
      // unsafe while the worker is busy reading. Production gates
      // identically via `!asyncRenderer_.isBusy()` checks scattered
      // throughout `EditorShell::runFrame`. The user-visible bug
      // lives exactly in the FRAMES this branch is skipped: a
      // long-running prewarm holds the worker busy and the click
      // queues up here waiting for `!isBusy()`.
      if (asyncRenderer.isBusy()) {
        // Mirror production: a deferred click pre-empts the in-
        // flight render. Whether `cancelInFlight()` actually saves
        // wall-clock depends on the §M4 cancel-poll granularity in
        // the compositor — if the worker is mid-segment with no
        // poll until segment-end, this is a no-op for the user.
        if (cancelInFlightOnDeferredClick_ && pendingClick.has_value() &&
            !pendingClick->dispatched) {
          asyncRenderer.cancelInFlight();
        }
        continue;
      }

      // MouseUp / keyboard events are registry-touching.
      for (const auto& event : frame.events) {
        if (event.kind == repro::ReproEvent::Kind::MouseUp && event.mouseButton == 0) {
          selectTool.onMouseUp(app, mouseDoc);
          leftButtonHeld = false;
        } else if (event.kind == repro::ReproEvent::Kind::KeyDown) {
          HandleKeyDown(app, event);
        }
      }

      // Slow-path click drain (race-unsafe; gated on !isBusy).
      if (pendingClick.has_value() && !pendingClick->dispatched) {
        selectTool.onMouseDown(app, pendingClick->documentPoint, pendingClick->modifiers);
        pendingClick->dispatched = true;
        pendingClick->dispatchedAt = std::chrono::steady_clock::now();
        pendingClick->awaitingFirstDragRender = true;
      }

      if (nowHeld && leftButtonHeld) {
        selectTool.onMouseMove(app, mouseDoc, /*buttonHeld=*/true);
      }
      leftButtonHeld = nowHeld;

      if (drainWritebacksEachFrame_) {
        DrainWritebackAndReparse(app, selectTool, &liveSource_);
      }
      app.flushFrame();

      // Canvas resize commit (registry write) + render request post.
      SyncCanvasSizeDebounced(app, viewport, frame.timestampSeconds, &lastCanvasCommitTimestamp);
      maybePostRender();

      pollAndRecord();
    }

    // Replay frames iterate in microseconds while real renders take
    // ms-to-seconds, so by loop end pendingClicks are typically still
    // un-dispatched (every frame hit the `isBusy()` gate). Drive
    // virtual frames at the replay's own cadence to flush them: each
    // tick polls for a completed result, if the worker is idle runs
    // the slow-path click + posts a follow-up render, and bails once
    // both clicks have been measured and the worker is idle.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
      pollAndRecord();
      if (asyncRenderer.isBusy()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      // Worker is idle. Drain pending click if needed.
      if (pendingClick.has_value() && !pendingClick->dispatched) {
        selectTool.onMouseDown(app, pendingClick->documentPoint, pendingClick->modifiers);
        pendingClick->dispatched = true;
        pendingClick->dispatchedAt = std::chrono::steady_clock::now();
        pendingClick->awaitingFirstDragRender = true;
      }
      app.flushFrame();
      maybePostRender();
      // Termination: pendingClick fully resolved (dispatched + first
      // drag render captured → pendingClick reset by pollAndRecord)
      // AND worker is idle AND nothing further to post.
      if (!pendingClick.has_value() && !asyncRenderer.isBusy()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
};

TEST_F(RnrReplayTest, FilterDisappearRepro3MatchesGoldenAfterSecondMouseUp) {
  ReplaySnapshot snapshot;
  ReplayRecording(kReplayPath, &snapshot);

  if (snapshot.stoppedForBisection) {
    GTEST_SKIP() << "Stopped at BISECTION_FRAME=" << snapshot.frameIndex
                 << "; wrote diagnostic snapshot to " << snapshot.diagnosticPath;
  }

  ASSERT_GE(snapshot.mouseUpCount, kTargetMouseUpCount)
      << "Replay ended before the second mouse-up checkpoint";
  ASSERT_FALSE(snapshot.bitmap.empty()) << "Replay produced an empty final bitmap";

  tests::BitmapGoldenCompareParams params;
  params.threshold = 0.03f;
  params.maxMismatchedPixels = 500;
  tests::CompareBitmapToGolden(snapshot.bitmap, kGoldenPath, "rnr_replay_repro3_after_mup2",
                               params);
}

// Regression for the filter-group "snap back to original position"
// drag-release bug from design doc 0033. The `.rnr` covers the
// minimal repro: drag `Big_lightning_glow` on the splash, release,
// then move the mouse. On the post-release frame the source-pane
// reparse swaps the `SVGDocumentHandle`, which `AsyncRenderer`'s
// per-iteration `documentChanged` check used to interpret as "throw
// the compositor away" — even when a valid `structuralRemap` (size
// 445 on this replay) accompanied the request. The full reconstruct
// flushed every cached filter-group bitmap and `canvasFromBitmap`
// stamp, the editor's cached GL textures then showed the dragged
// element at its pre-drag rasterize-time position, and the drag
// preview's translation had already dropped to zero on release →
// the user saw a visible "snap" until the slow render settled.
//
// The fix routes a non-empty structural remap through
// `CompositorController::remapAfterStructuralReplace` even when the
// handle pointer changed, preserving the in-flight compositor state.
// Assertion: `compositorReconstructCount == 1` (the initial
// construction) for the entire session. A second reconstruct means
// the post-drag handle swap fell through to the destructive path
// again.
// Async-mode regression for the post-zoom drag-start hang.
// Drives `drag_start_hang_repro.rnr` through the production-faithful
// async replay harness: per-frame `maybeRequestRender`-equivalent
// with `isBusy()` gating, prewarm-on-canvas-change driven by
// `ExperimentalDragPresentation::shouldPrewarm`, 120 ms debounced
// canvas-size commit, source-text writeback drain +
// `ReplaceDocumentCommand` reparse, slow-path mouseDown gated on
// `!isBusy()`, deferred-click `cancelInFlight`, and (design doc
// 0034) progressive-rendering `Intermediate` emit between the drag-
// target layer rasterize and the canvas-sized work.
//
// The recording's second mouse-down lands while a post-pinch
// selection prewarm is in flight at ~3× canvas. With progressive
// rendering enabled, the worker ships an `Intermediate` result
// carrying the freshly-rasterized drag-target layer + the prior
// frame's flat baseline as soon as the layer rasterize completes —
// the user sees their drag target move at the new position within
// ~100 ms, even though the full canvas-sized refinement takes
// seconds.
//
// Budgets:
//   * `clickToFirstDragPixelMs` (= Intermediate) < 200 ms —
//     the user-visible click responsiveness. Tightens 0033's
//     "click-to-first-pixel < 100 ms" target without busting on
//     CI scheduler jitter.
//   * `clickToFinalPixelMs` is informational only — the canvas-
//     sized refinement may take seconds at high zoom; the user
//     experience target is satisfied by the intermediate.
TEST_F(RnrReplayTest, DragStartAfterZoomAsyncHarnessDoesNotHang) {
  AsyncReplaySnapshot snapshot;
  drainWritebacksEachFrame_ = true;
  cancelInFlightOnDeferredClick_ = true;  // 0034 keeps `cancelInFlight`
  ReplayRecordingAsync("donner/editor/tests/drag_start_hang_repro.rnr", &snapshot);

  std::cerr << "[DragStartAfterZoomAsync] renders completed=" << snapshot.rendersCompleted
            << " deferredOnBusy=" << snapshot.rendersDeferredOnBusy
            << " maxWorkerRenderMs=" << snapshot.maxWorkerRenderMs << "\n";
  for (const auto& l : snapshot.latencies) {
    std::cerr << "[DragStartAfterZoomAsync] mouseDown@frame=" << l.mouseDownFrameIndex
              << " clickToFirstDragPixelMs=" << l.clickToFirstDragPixelMs
              << " clickToFinalPixelMs=" << l.clickToFinalPixelMs
              << " clickToSlowPathMs=" << l.clickToSlowPathMs << "\n";
  }

  ASSERT_GE(snapshot.latencies.size(), 2u)
      << "Replay did not capture both mouse-down latencies (expected 2 drags)";

  // Budget: 500 ms. Headline observation on `donner_splash.svg` at
  // 3× canvas — without progressive rendering this latency is
  // ~6600 ms; with progressive rendering + cancelInFlight it lands
  // around 300 ms. The 500 ms budget catches a regression to the
  // pre-0034 multi-second hang while leaving headroom for CI
  // scheduler jitter. Tightening to < 200 ms is a follow-on
  // optimization — see design doc 0034's "Open Questions".
  constexpr double kPostZoomFirstPixelBudgetMs = 500.0;
  EXPECT_LT(snapshot.latencies[1].clickToFirstDragPixelMs, kPostZoomFirstPixelBudgetMs)
      << "Post-zoom click → first drag pixel exceeded budget. Measured "
      << snapshot.latencies[1].clickToFirstDragPixelMs << " ms (of which "
      << snapshot.latencies[1].clickToSlowPathMs
      << " ms was the click sitting deferred on `isBusy()`). Budget " << kPostZoomFirstPixelBudgetMs
      << " ms. Progressive rendering (design doc 0034) must ship an `Intermediate` result with "
         "the drag-target layer + prior flat baseline before the canvas-sized refinement begins.";
}

TEST_F(RnrReplayTest, FilterSnapbackReproPreservesCompositorAcrossWriteback) {
  ReplaySnapshot snapshot;
  stopAfterAllFrames_ = true;  // .rnr has exactly one mouse-up; play to EOF.
  drainWritebacksEachFrame_ = true;
  ReplayRecording("donner/editor/tests/filter_snapback_repro.rnr", &snapshot);

  ASSERT_FALSE(snapshot.bitmap.empty()) << "Replay produced an empty final bitmap";
  EXPECT_EQ(snapshot.compositorReconstructCount, 1u)
      << "Compositor was reconstructed " << snapshot.compositorReconstructCount
      << " times — the post-drag-release source reparse must route through "
         "remapAfterStructuralReplace, not a destructive `compositor_ = "
         "make_unique<...>` rebuild that flushes the in-flight drag bitmap and "
         "leaves the editor blitting cached textures at zero offset.";
}

}  // namespace
}  // namespace donner::editor
