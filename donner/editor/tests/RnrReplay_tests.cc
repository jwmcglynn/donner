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
#include "donner/base/tests/TestTempDir.h"
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

bool IsGraphicsElement(const svg::SVGElement& element) {
  return element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
    return element.isa<svg::SVGGraphicsElement>();
  });
}

Entity SelectedGraphicsEntity(EditorApp& app) {
  if (!app.selectedElement().has_value() || !IsGraphicsElement(*app.selectedElement())) {
    return entt::null;
  }

  return app.selectedElement()->unsafeEntityHandle().entity();
}

constexpr std::size_t kTargetMouseUpCount = 2;
constexpr char kReplayPath[] = "donner/editor/tests/filter_elm_disappear-3.rnr";
constexpr char kTinySkiaGoldenPath[] =
    "donner/editor/tests/testdata/filter_disappear_rnr3_after_mup2.png";
constexpr char kGeodeGoldenPath[] =
    "donner/editor/tests/testdata/filter_disappear_rnr3_after_mup2_geode.png";

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
  return TestTempDir();
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

struct RecordedSvgInput {
  std::filesystem::path displayPath;
  std::string source;
};

std::filesystem::path EmbeddedSvgDisplayPath(const repro::ReproMetadata& metadata) {
  if (!metadata.svgBasename.empty()) {
    return std::filesystem::path(metadata.svgBasename);
  }
  if (!metadata.svgPath.empty()) {
    return std::filesystem::path(metadata.svgPath).filename();
  }
  return "embedded.svg";
}

RecordedSvgInput LoadRecordedSvgInput(const std::filesystem::path& reproPath,
                                      const repro::ReproMetadata& metadata) {
  if (metadata.svgSource.has_value()) {
    return RecordedSvgInput{
        .displayPath = EmbeddedSvgDisplayPath(metadata),
        .source = *metadata.svgSource,
    };
  }

  const std::filesystem::path svgPath = ResolveSvgPath(reproPath, metadata.svgPath);
  return RecordedSvgInput{
      .displayPath = svgPath,
      .source = LoadFileOrEmpty(svgPath),
  };
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

MouseModifiers DecodeMouseModifiers(int modifierMask, double pixelsPerDocUnit = 1.0) {
  MouseModifiers modifiers;
  modifiers.shift = (modifierMask & (1 << 1)) != 0;
  modifiers.option = (modifierMask & (1 << 2)) != 0;
  modifiers.pixelsPerDocUnit = pixelsPerDocUnit;
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
  const Vector2i desired = viewport.rasterViewport().semanticCanvasSizePx;
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
  RenderRequest request(renderer, app.document().document());
  request.captureCpuSnapshot = true;
  request.version = app.document().currentFrameVersion();
  request.documentGeneration = app.document().documentGeneration();
  request.structuralRemap = app.document().consumePendingStructuralRemap();
  request.selectedEntity = SelectedGraphicsEntity(app);
  if (auto preview = selectTool.activeDragPreview(); preview.has_value()) {
    request.dragPreview = RenderRequest::DragPreview{
        .entity = preview->entity,
        .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
        .translation = preview->translation,
        .documentFromCachedDocument = preview->documentFromCachedDocument,
        .dragGeneration = preview->dragGeneration,
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
// `textEditor.getText()`. Returns true if a writeback was applied -
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
    bool usesTexturePresentation = false;
  };

  void ReplayRecording(const std::filesystem::path& reproPath, ReplaySnapshot* out) {
    ASSERT_NE(out, nullptr);

    auto replay = repro::ReadReproFile(reproPath);
    ASSERT_TRUE(replay.has_value()) << "Failed to parse replay file: " << reproPath;

    const RecordedSvgInput svgInput = LoadRecordedSvgInput(reproPath, replay->metadata);
    ASSERT_FALSE(svgInput.source.empty()) << "SVG source missing: " << svgInput.displayPath;
    liveSource_ = svgInput.source;

    EditorApp app;
    ASSERT_TRUE(app.loadFromString(svgInput.source))
        << "Failed to load SVG fixture: " << svgInput.displayPath;

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

    AsyncRenderer asyncRenderer;
    svg::Renderer renderer;
    out->usesTexturePresentation = renderer.requiresTextureSnapshotPresentation();

    SyncCanvasSize(app, viewport);
    auto initialResult = RequestRenderAndWait(asyncRenderer, renderer, app, selectTool);
    ASSERT_TRUE(initialResult.has_value()) << "Initial render timed out";
    ASSERT_FALSE(initialResult->bitmap.empty()) << "Initial render produced an empty bitmap";

    out->bitmap = initialResult->bitmap;
    out->frameIndex = 0;
    out->mouseUpCount = 0;

    const std::optional<std::uint64_t> bisectFrame = ParseBisectFrame();
    bool leftButtonHeld = false;
    bool renderedActiveDragFrame = false;
    std::optional<Vector2d> lastDragMouseDoc;

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
      bool suppressUnsampledDragRender = false;

      for (const auto& event : frame.events) {
        switch (event.kind) {
          case repro::ReproEvent::Kind::MouseDown:
            if (event.mouseButton == 0) {
              selectTool.onMouseDown(
                  app, mouseDoc,
                  DecodeMouseModifiers(frame.modifiers, viewport.pixelsPerDocUnit()));
              renderedActiveDragFrame = false;
              lastDragMouseDoc = mouseDoc;
              frameNeedsRender = true;
            }
            break;

          case repro::ReproEvent::Kind::MouseUp:
            if (event.mouseButton == 0) {
              selectTool.onMouseUp(app, mouseDoc);
              lastDragMouseDoc.reset();
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

      if (nowHeld && leftButtonHeld &&
          (!lastDragMouseDoc.has_value() || mouseDoc != *lastDragMouseDoc)) {
        selectTool.onMouseMove(app, mouseDoc, /*buttonHeld=*/true,
                               DecodeMouseModifiers(frame.modifiers, viewport.pixelsPerDocUnit()));
        lastDragMouseDoc = mouseDoc;
        // One active frame preserves the promotion transition; later moves coalesce before release.
        if (renderedActiveDragFrame) {
          suppressUnsampledDragRender = true;
        } else {
          renderedActiveDragFrame = true;
          frameNeedsRender = true;
        }
      }
      leftButtonHeld = nowHeld;

      if (drainWritebacksEachFrame_) {
        frameNeedsRender |= DrainWritebackAndReparse(app, selectTool, &liveSource_);
      }

      const bool flushedFrame = app.flushFrame();
      frameNeedsRender |= flushedFrame && !suppressUnsampledDragRender;

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

  // TinySkia is stable across the supported toolchains to one 8-bit channel step, but hosted
  // Linux differs from the golden host at ten isolated pixels in this filtered image. Keep the
  // comparison identity-strict above that cross-toolchain rounding step, with zero pixels allowed
  // to exceed it.
  const std::string_view goldenPath =
      snapshot.usesTexturePresentation ? kGeodeGoldenPath : kTinySkiaGoldenPath;
  tests::CompareBitmapToGolden(snapshot.bitmap, goldenPath, "rnr_replay_repro3_after_mup2",
                               snapshot.usesTexturePresentation
                                   ? tests::PixelmatchIdentityParams()
                                   : tests::RgbaLInfToleranceParams(1, 10));
}

// Structural remaps must preserve the compositor across drag-release source
// reparses. Reconstruct count stays at the initial construction.
TEST_F(RnrReplayTest, FilterSnapbackReproPreservesCompositorAcrossWriteback) {
  ReplaySnapshot snapshot;
  stopAfterAllFrames_ = true;  // .rnr has exactly one mouse-up; play to EOF.
  drainWritebacksEachFrame_ = true;
  ReplayRecording("donner/editor/tests/filter_snapback_repro.rnr", &snapshot);

  ASSERT_FALSE(snapshot.bitmap.empty()) << "Replay produced an empty final bitmap";
  EXPECT_EQ(snapshot.compositorReconstructCount, 1u)
      << "Compositor was reconstructed " << snapshot.compositorReconstructCount
      << " times - the post-drag-release source reparse must route through "
         "remapAfterStructuralReplace, not a destructive `compositor_ = "
         "make_unique<...>` rebuild that flushes the in-flight drag bitmap and "
         "leaves the editor blitting cached textures at zero offset.";
}

// Programmatic regression for the operator-visible "delete-element flash"
// bug: after moving several shapes (drag-end writebacks → reparse), the
// user clicks a different filter group (`Lightning_glow_dark`) and
// presses Delete. The user reports that for a few frames the
// previously-moved shapes appear at their pre-move positions and
// neighboring filter groups render brighter, before settling back to
// the correct state.
//
// This test drives the same pipeline programmatically without ImGui /
// SelectTool: writebacks are simulated by running source-text patches
// through `ReplaceDocumentCommand(preserveUndoOnReparse=true)` - the
// production path the source-pane uses for any change the classifier
// can't reduce to a single `SetAttribute`. The delete writeback's
// element-remove patch is multi-byte and always falls through to
// `ReplaceDocumentCommand`, matching what `DocumentSyncController`
// dispatches in the editor.
//
// The test captures bitmaps at three checkpoints - after the moves,
// immediately after the delete, and after the delete's reparse - and
// compares the per-pixel difference between "after moves" and "after
// delete" against an independently-rendered ground truth: the same
// source with `Lightning_glow_dark` removed up-front via the SVG
// parser. If the post-delete bitmap differs from the ground truth, the
// previously-moved shapes are NOT where they should be.
TEST_F(RnrReplayTest, DeleteElementDoesNotResetPreviouslyMovedShapes) {
  // Load the splash directly - no .rnr, just programmatic mutations.
  const std::filesystem::path splashPath("donner_splash.svg");
  const std::string svgSource = LoadFileOrEmpty(splashPath);
  ASSERT_FALSE(svgSource.empty()) << "SVG source missing: " << splashPath;
  liveSource_ = svgSource;

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(svgSource));
  auto documentViewBox = app.document().document().svgElement().viewBox();
  ASSERT_TRUE(documentViewBox.has_value());

  ViewportState viewport;
  viewport.documentViewBox = *documentViewBox;
  viewport.devicePixelRatio = 1.0;
  viewport.paneOrigin = Vector2d::Zero();
  viewport.paneSize = Vector2d(800.0, 460.0);
  viewport.resetTo100Percent();

  SelectTool selectTool;
  AsyncRenderer asyncRenderer;
  svg::Renderer renderer;
  SyncCanvasSize(app, viewport);

  // Initial render so the compositor is warm.
  {
    auto initial = RequestRenderAndWait(asyncRenderer, renderer, app, selectTool);
    ASSERT_TRUE(initial.has_value());
    ASSERT_FALSE(initial->bitmap.empty());
  }

  // For each "dragged" shape, simulate the drag-end writeback by
  // patching the source text to insert a `transform="translate(...)"`
  // attribute on the element's opening tag, then dispatch a
  // `ReplaceDocumentCommand(preserveUndoOnReparse=true)` - the exact
  // command shape the classifier emits when it falls through to the
  // structural path (and what `DocumentSyncController` re-dispatches
  // for any unclassifiable diff).
  //
  // Per-shape translation chosen large enough to be visible in any
  // pixel-diff but small enough to keep the shape on-canvas. The
  // values are arbitrary - they just need to be distinct so a
  // "flash to pre-move" would show a clear delta against the
  // ground-truth bitmap.
  struct ShapeDrag {
    std::string_view id;
    int dx;
    int dy;
  };
  const std::array<ShapeDrag, 2> drags = {
      ShapeDrag{"Donner_line", 25, 35},
      ShapeDrag{"Lightning_glow_bright", -30, 20},
  };
  for (const auto& drag : drags) {
    const std::string openingTag = std::string("<g id=\"") + std::string(drag.id) + "\"";
    const auto pos = liveSource_.find(openingTag);
    ASSERT_NE(pos, std::string::npos) << "Could not find shape " << drag.id << " in source";
    const auto endOfTag = pos + openingTag.size();
    const std::string transformAttr =
        " transform=\"translate(" + std::to_string(drag.dx) + "," + std::to_string(drag.dy) + ")\"";
    liveSource_.insert(endOfTag, transformAttr);

    app.applyMutation(
        EditorCommand::ReplaceDocumentCommand(liveSource_, /*preserveUndoOnReparse=*/true));
    ASSERT_TRUE(app.flushFrame());
    auto result = RequestRenderAndWait(asyncRenderer, renderer, app, selectTool);
    ASSERT_TRUE(result.has_value()) << "Render after drag of " << drag.id << " timed out";
    ASSERT_FALSE(result->bitmap.empty());
  }

  // Capture the post-drags state. This is the bitmap the user sees
  // right before pressing Delete.
  auto stateAfterMoves = RequestRenderAndWait(asyncRenderer, renderer, app, selectTool);
  ASSERT_TRUE(stateAfterMoves.has_value());
  ASSERT_FALSE(stateAfterMoves->bitmap.empty());

  // Find Lightning_glow_dark in the live DOM and select it (matches
  // the user's "I clicked the lighting_glow_dark in my repro" step).
  auto target = app.document().document().querySelector("#Lightning_glow_dark");
  ASSERT_TRUE(target.has_value()) << "Lightning_glow_dark missing from live DOM";
  auto removeTarget = captureAttributeWritebackTarget(*target);
  ASSERT_TRUE(removeTarget.has_value());
  app.setSelection(*target);
  // One render with the selection active so the compositor's
  // promote-on-selection path runs (the user's repro has the element
  // selected at the moment Delete is pressed).
  {
    auto withSelection = RequestRenderAndWait(asyncRenderer, renderer, app, selectTool);
    ASSERT_TRUE(withSelection.has_value());
  }

  // Press Delete: the editor's shortcut handler clears selection
  // first, then enqueues the element-remove writeback and dispatches
  // `DeleteElementCommand`. Mirror that order exactly.
  app.setSelection(std::nullopt);
  app.enqueueElementRemoveWriteback(
      EditorApp::CompletedElementRemoveWriteback{.target = *removeTarget});
  app.applyMutation(EditorCommand::DeleteElementCommand(*target));
  ASSERT_TRUE(app.flushFrame());

  auto stateAfterDelete = RequestRenderAndWait(asyncRenderer, renderer, app, selectTool);
  ASSERT_TRUE(stateAfterDelete.has_value()) << "Post-delete render timed out";
  ASSERT_FALSE(stateAfterDelete->bitmap.empty());

  // Pump enough renders to clear `processPendingDemotions`' 30-frame
  // hysteresis (`kDemotionHysteresisFrames`), then compare the settled
  // compositor against ground truth.
  std::optional<RenderResult> stateAfterHysteresis;
  for (int i = 0; i < 35; ++i) {
    stateAfterHysteresis = RequestRenderAndWait(asyncRenderer, renderer, app, selectTool);
    ASSERT_TRUE(stateAfterHysteresis.has_value());
  }

  // Drain the element-remove writeback to patch source text and
  // dispatch `ReplaceDocumentCommand(preserveUndoOnReparse=true)` -
  // exactly what `DocumentSyncController::applyPendingWritebacks`
  // does for the post-delete source-pane sync.
  auto removals = app.consumeElementRemoveWritebacks();
  ASSERT_EQ(removals.size(), 1u);
  auto patch = buildElementRemoveWriteback(liveSource_, removals[0].target);
  ASSERT_TRUE(patch.has_value()) << "Failed to build element-remove patch";
  applyPatches(liveSource_, {{*patch}});
  app.applyMutation(
      EditorCommand::ReplaceDocumentCommand(liveSource_, /*preserveUndoOnReparse=*/true));
  ASSERT_TRUE(app.flushFrame());

  auto stateAfterReparse = RequestRenderAndWait(asyncRenderer, renderer, app, selectTool);
  ASSERT_TRUE(stateAfterReparse.has_value()) << "Post-reparse render timed out";
  ASSERT_FALSE(stateAfterReparse->bitmap.empty());

  // Independent ground truth: render the FINAL source (post-drag,
  // post-delete) through a fresh `EditorApp` + `AsyncRenderer`. No
  // compositor-state carryover, no drag/select/delete path. If
  // anything in the carry-over pipeline is producing wrong pixels,
  // this comparison surfaces it.
  EditorApp groundTruthApp;
  AsyncRenderer groundTruthAsync;
  svg::Renderer groundTruthRenderer;
  ASSERT_TRUE(groundTruthApp.loadFromString(liveSource_));
  SyncCanvasSize(groundTruthApp, viewport);
  SelectTool groundTruthSelectTool;
  auto groundTruth = RequestRenderAndWait(groundTruthAsync, groundTruthRenderer, groundTruthApp,
                                          groundTruthSelectTool);
  ASSERT_TRUE(groundTruth.has_value());
  ASSERT_FALSE(groundTruth->bitmap.empty());

  // Dump all four bitmaps so the human investigator can inspect the
  // delta when this trips.
  const auto outDir = DiagnosticOutputDir();
  const auto dump = [&](const svg::RendererBitmap& bmp, std::string_view label) {
    const auto path = outDir / (std::string("delete_flash_") + std::string(label) + ".png");
    svg::RendererImageIO::writeRgbaPixelsToPngFile(
        path.string().c_str(), bmp.pixels, bmp.dimensions.x, bmp.dimensions.y, bmp.rowBytes / 4u);
  };
  dump(stateAfterMoves->bitmap, "01_after_moves");
  dump(stateAfterDelete->bitmap, "02_after_delete");
  dump(stateAfterHysteresis->bitmap, "02b_after_hysteresis");
  dump(stateAfterReparse->bitmap, "03_after_reparse");
  dump(groundTruth->bitmap, "04_ground_truth");

  // Approved exception: the replay and fresh-load ground truth still differ at AA edges. Keep the
  // tolerance explicit so converting this case to identity remains visible follow-up work.
  const tests::BitmapGoldenCompareParams deleteReplayTolerance =
      tests::ApprovedPixelToleranceParams(0.03f, 500);
  tests::CompareBitmapToBitmap(stateAfterDelete->bitmap, groundTruth->bitmap,
                               "delete_flash_post_delete_vs_ground_truth", deleteReplayTolerance);
  tests::CompareBitmapToBitmap(stateAfterHysteresis->bitmap, groundTruth->bitmap,
                               "delete_flash_post_hysteresis_vs_ground_truth",
                               deleteReplayTolerance);
  tests::CompareBitmapToBitmap(stateAfterReparse->bitmap, groundTruth->bitmap,
                               "delete_flash_post_reparse_vs_ground_truth", deleteReplayTolerance);
}

// Replay must match fresh-load ground truth after a structural-remap drag
// session. This rejects stale composed snapshots after mouse-up while keeping
// compositor preservation coverage intact. Diff artifacts are written under
// `$TEST_UNDECLARED_OUTPUTS_DIR/filter_post_drag_jump_*.png` on failure.
TEST_F(RnrReplayTest, FilterPostDragJumpReplayMatchesGroundTruth) {
  ReplaySnapshot snapshot;
  stopAfterAllFrames_ = true;
  drainWritebacksEachFrame_ = true;
  ReplayRecording("donner/editor/tests/filter_post_drag_jump.rnr", &snapshot);

  ASSERT_FALSE(snapshot.bitmap.empty());

  EditorApp groundTruthApp;
  AsyncRenderer groundTruthAsync;
  svg::Renderer groundTruthRenderer;
  ASSERT_TRUE(groundTruthApp.loadFromString(liveSource_));
  groundTruthApp.document().document().setCanvasSize(snapshot.bitmap.dimensions.x,
                                                     snapshot.bitmap.dimensions.y);
  SelectTool groundTruthSelectTool;
  auto groundTruth = RequestRenderAndWait(groundTruthAsync, groundTruthRenderer, groundTruthApp,
                                          groundTruthSelectTool);
  ASSERT_TRUE(groundTruth.has_value());

  const auto outDir = DiagnosticOutputDir();
  const auto dump = [&](const svg::RendererBitmap& bmp, std::string_view label) {
    const auto path =
        outDir / (std::string("filter_post_drag_jump_") + std::string(label) + ".png");
    svg::RendererImageIO::writeRgbaPixelsToPngFile(
        path.string().c_str(), bmp.pixels, bmp.dimensions.x, bmp.dimensions.y, bmp.rowBytes / 4u);
  };
  dump(snapshot.bitmap, "01_replay_final");
  dump(groundTruth->bitmap, "02_ground_truth");

  {
    const auto path = outDir / "filter_post_drag_jump_final_source.svg";
    std::ofstream out(path, std::ios::binary);
    out.write(liveSource_.data(), static_cast<std::streamsize>(liveSource_.size()));
  }

  // Approved exception: this fixture compares two independently rendered final states and is not
  // identity-stable yet; preserve the pre-existing tolerance without widening it.
  tests::CompareBitmapToBitmap(snapshot.bitmap, groundTruth->bitmap,
                               "filter_post_drag_jump_replay_vs_ground_truth",
                               tests::ApprovedPixelToleranceParams(0.03f, 2000));
}

}  // namespace
}  // namespace donner::editor
