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
