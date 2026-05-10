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

#include "donner/base/Vector2.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/SelectTool.h"
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

class RnrReplayTest : public ::testing::Test {
protected:
  struct ReplaySnapshot {
    svg::RendererBitmap bitmap;
    std::uint64_t frameIndex = 0;
    std::size_t mouseUpCount = 0;
    bool stoppedForBisection = false;
    std::filesystem::path diagnosticPath;
  };

  void ReplayRecording(const std::filesystem::path& reproPath, ReplaySnapshot* out) {
    ASSERT_NE(out, nullptr);

    auto replay = repro::ReadReproFile(reproPath);
    ASSERT_TRUE(replay.has_value()) << "Failed to parse replay file: " << reproPath;

    const std::filesystem::path svgPath = ResolveSvgPath(reproPath, replay->metadata.svgPath);
    const std::string svgSource = LoadFileOrEmpty(svgPath);
    ASSERT_FALSE(svgSource.empty()) << "SVG source missing: " << svgPath;

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
      if (!bisectFrame.has_value() && out->mouseUpCount >= kTargetMouseUpCount) {
        break;
      }
    }

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

}  // namespace
}  // namespace donner::editor
