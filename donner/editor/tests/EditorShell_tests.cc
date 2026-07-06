#include "donner/editor/EditorShell.h"

#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "donner/css/Color.h"
#include "donner/editor/EditorShellInternal.h"
#include "donner/editor/InMemoryClipboard.h"
#include "donner/editor/gui/EditorWindow.h"
#include "donner/editor/repro/ReproFile.h"

namespace donner::editor {
namespace {

constexpr std::string_view kInitialSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <rect id="background" width="120" height="80" fill="#ffffff"/>
  <rect id="target" x="10" y="12" width="40" height="24" fill="#3366cc"/>
  <text id="label" x="12" y="60">Donner</text>
</svg>
)svg";

constexpr std::string_view kStyledSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <style>
    .hit { fill: red; stroke: blue; }
  </style>
  <rect id="target" class="hit" x="10" y="12" width="40" height="24"/>
  <circle id="other" cx="80" cy="40" r="10" fill="url(#paint)"/>
  <linearGradient id="paint">
    <stop offset="0" stop-color="red"/>
  </linearGradient>
</svg>
)svg";

constexpr std::string_view kReferencedSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <defs>
    <linearGradient id="paint">
      <stop offset="0" stop-color="red"/>
    </linearGradient>
    <clipPath id="clip">
      <rect width="100" height="60"/>
    </clipPath>
  </defs>
  <rect id="target" x="10" y="12" width="40" height="24" fill="url(#paint)" clip-path="url(#clip)"/>
  <circle id="referrer" cx="80" cy="40" r="10" fill="url(#paint)"/>
</svg>
)svg";

constexpr std::string_view kPaintToolbarSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <defs>
    <linearGradient id="paint">
      <stop offset="0" stop-color="red"/>
    </linearGradient>
  </defs>
  <rect id="local" x="10" y="12" width="20" height="20" fill="url(#paint)" stroke="url(#paint)"/>
  <rect id="missing" x="40" y="12" width="20" height="20" fill="url(#missing)"/>
  <rect id="contextual" x="70" y="12" width="20" height="20" fill="context-fill"/>
  <rect id="styled-none-attribute" x="70" y="42" width="20" height="20"
        fill="context-stroke" style="fill: none"/>
  <rect id="external" x="10" y="42" width="20" height="20"
        fill="url(https://example.invalid/paint.svg#paint)"/>
</svg>
)svg";

constexpr std::string_view kIntrinsicSizeSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="160" height="90">
  <rect id="target" width="160" height="90"/>
</svg>
)svg";

gui::EditorWindow MakeHiddenWindow() {
  return gui::EditorWindow(gui::EditorWindowOptions{
      .title = "Donner EditorShell test",
      .initialWidth = 640,
      .initialHeight = 480,
      .visible = false,
  });
}

std::filesystem::path TempPathForTest(std::string_view suffix) {
  const testing::TestInfo* testInfo = testing::UnitTest::GetInstance()->current_test_info();
  const std::string testName = testInfo != nullptr ? testInfo->name() : "unknown";
  return std::filesystem::temp_directory_path() /
         ("donner_editor_shell_" + testName + "_" + std::string(suffix));
}

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path);
  ASSERT_TRUE(output.good()) << "Could not open " << path;
  output << text;
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string result((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return result;
}

EditorShellOptions OptionsWithSource(std::string_view source,
                                     std::string_view path = "memory.svg") {
  EditorShellOptions options;
  options.initialSource = std::string(source);
  options.initialPath = std::string(path);
  return options;
}

}  // namespace

TEST(EditorShellInternalTest, MapsPresentationResourcesToTelemetrySamples) {
  PresentationResourceStats resources{
      .overlayBytes = 11,
      .activeTileBytes = 22,
      .overviewTileBytes = 33,
      .pendingRetiredBytes = 44,
      .agedRetiredBytes = 55,
      .totalTrackedBytes = 66,
      .peakTrackedBytes = 77,
      .wgpuLifetimeTextureCreates = 88,
      .wgpuLifetimeBufferCreates = 99,
  };

  const FrameMemorySample memory = internal::MemorySampleFromPresentationResources(resources);
  EXPECT_EQ(memory.overlayBytes, 11u);
  EXPECT_EQ(memory.activeTileBytes, 22u);
  EXPECT_EQ(memory.overviewTileBytes, 33u);
  EXPECT_EQ(memory.retiredBytes, 99u);
  EXPECT_EQ(memory.totalTrackedBytes, 66u);
  EXPECT_EQ(memory.peakTrackedBytes, 77u);
  EXPECT_EQ(memory.wgpuLifetimeTextureCreates, 88u);
  EXPECT_EQ(memory.wgpuLifetimeBufferCreates, 99u);

  const FrameMissResourceTelemetry telemetry =
      internal::FrameMissTelemetryFromPresentationResources(resources);
  EXPECT_EQ(telemetry.overlayBytes, memory.overlayBytes);
  EXPECT_EQ(telemetry.activeTileBytes, memory.activeTileBytes);
  EXPECT_EQ(telemetry.overviewTileBytes, memory.overviewTileBytes);
  EXPECT_EQ(telemetry.retiredBytes, memory.retiredBytes);
  EXPECT_EQ(telemetry.totalTrackedBytes, memory.totalTrackedBytes);
  EXPECT_EQ(telemetry.peakTrackedBytes, memory.peakTrackedBytes);
  EXPECT_EQ(telemetry.wgpuLifetimeTextureCreates, memory.wgpuLifetimeTextureCreates);
  EXPECT_EQ(telemetry.wgpuLifetimeBufferCreates, memory.wgpuLifetimeBufferCreates);
}

TEST(EditorShellInternalTest, CursorForTransformHandleIntentMapsResizeAndRotateHandles) {
  EXPECT_EQ(internal::CursorForTransformHandleIntent(
                SelectionTransformHandleIntent{.kind = SelectionTransformHandleKind::None}),
            ImGuiMouseCursor_Arrow);
  EXPECT_EQ(internal::CursorForTransformHandleIntent(
                SelectionTransformHandleIntent{.kind = SelectionTransformHandleKind::Rotate}),
            ImGuiMouseCursor_ResizeAll);
  EXPECT_EQ(internal::CursorForTransformHandleIntent(SelectionTransformHandleIntent{
                .kind = SelectionTransformHandleKind::Resize,
                .corner = SelectionTransformCorner::TopLeft,
            }),
            ImGuiMouseCursor_ResizeNWSE);
  EXPECT_EQ(internal::CursorForTransformHandleIntent(SelectionTransformHandleIntent{
                .kind = SelectionTransformHandleKind::Resize,
                .corner = SelectionTransformCorner::BottomRight,
            }),
            ImGuiMouseCursor_ResizeNWSE);
  EXPECT_EQ(internal::CursorForTransformHandleIntent(SelectionTransformHandleIntent{
                .kind = SelectionTransformHandleKind::Resize,
                .corner = SelectionTransformCorner::TopRight,
            }),
            ImGuiMouseCursor_ResizeNESW);
  EXPECT_EQ(internal::CursorForTransformHandleIntent(SelectionTransformHandleIntent{
                .kind = SelectionTransformHandleKind::Resize,
                .corner = SelectionTransformCorner::BottomLeft,
            }),
            ImGuiMouseCursor_ResizeNESW);
}

TEST(EditorShellInternalTest, TextToolHintShowsOnlyWhileIdle) {
  // Idle text tool: the double-click / drag affordances are invisible, so
  // the hint must teach them.
  const std::string_view idleHint =
      internal::TextToolHintLabel(/*isEditing=*/false, /*isDraggingBox=*/false);
  EXPECT_NE(idleHint.find("Double-click"), std::string_view::npos) << idleHint;
  EXPECT_NE(idleHint.find("Drag"), std::string_view::npos) << idleHint;

  // While a session or a box drag is active the hint would be noise.
  EXPECT_TRUE(internal::TextToolHintLabel(/*isEditing=*/true, /*isDraggingBox=*/false).empty());
  EXPECT_TRUE(internal::TextToolHintLabel(/*isEditing=*/false, /*isDraggingBox=*/true).empty());
  EXPECT_TRUE(internal::TextToolHintLabel(/*isEditing=*/true, /*isDraggingBox=*/true).empty());
}

TEST(EditorShellInternalTest, ActiveAttributePaintSlotHandlesNoneColorAndCustomValues) {
  const internal::ToolbarPaintSlotState none =
      internal::ToolbarPaintSlotStateForActiveAttribute("none");
  EXPECT_TRUE(none.isNone);
  EXPECT_FALSE(none.isCustom);

  const internal::ToolbarPaintSlotState color =
      internal::ToolbarPaintSlotStateForActiveAttribute("#123456");
  EXPECT_FALSE(color.isNone);
  EXPECT_FALSE(color.isCustom);
  EXPECT_EQ(color.color, css::RGBA::RGB(0x12, 0x34, 0x56));

  const internal::ToolbarPaintSlotState custom =
      internal::ToolbarPaintSlotStateForActiveAttribute("url(#paint)");
  EXPECT_FALSE(custom.isNone);
  EXPECT_TRUE(custom.isCustom);
  EXPECT_EQ(custom.color, internal::PaintServerFallbackColor());
  EXPECT_EQ(custom.customLabel, "url(#paint)");
}

TEST(EditorShellInternalTest, PaintServerSlotHandlesVariantPaintSources) {
  const css::RGBA currentColor = css::RGBA::RGB(0x10, 0x20, 0x30);

  const internal::ToolbarPaintSlotState none = internal::ToolbarPaintSlotStateForPaintServer(
      svg::PaintServer(svg::PaintServer::None{}), currentColor, nullptr, std::nullopt);
  EXPECT_TRUE(none.isNone);

  const internal::ToolbarPaintSlotState solid = internal::ToolbarPaintSlotStateForPaintServer(
      svg::PaintServer(svg::PaintServer::Solid(css::Color(css::Color::CurrentColor{}))),
      currentColor, nullptr, std::nullopt);
  EXPECT_FALSE(solid.isNone);
  EXPECT_FALSE(solid.isCustom);
  EXPECT_EQ(solid.color, currentColor);

  const internal::ToolbarPaintSlotState referenced = internal::ToolbarPaintSlotStateForPaintServer(
      svg::PaintServer(svg::PaintServer::ElementReference(
          svg::Reference("other.svg#paint"), css::Color(css::RGBA::RGB(0x80, 0x40, 0x20)))),
      currentColor, nullptr, std::nullopt);
  EXPECT_FALSE(referenced.isNone);
  EXPECT_TRUE(referenced.isCustom);
  ASSERT_TRUE(referenced.reference.has_value());
  EXPECT_EQ(referenced.reference->href, "other.svg#paint");
  EXPECT_TRUE(referenced.reference->external);
  EXPECT_EQ(referenced.color, css::RGBA::RGB(0x80, 0x40, 0x20));

  const internal::ToolbarPaintSlotState contextFill = internal::ToolbarPaintSlotStateForPaintServer(
      svg::PaintServer(svg::PaintServer::ContextFill{}), currentColor, nullptr, std::nullopt);
  EXPECT_FALSE(contextFill.isNone);
  EXPECT_TRUE(contextFill.isCustom);
  EXPECT_EQ(contextFill.customLabel, "context-fill");

  const internal::ToolbarPaintSlotState contextStroke =
      internal::ToolbarPaintSlotStateForPaintServer(
          svg::PaintServer(svg::PaintServer::ContextStroke{}), currentColor, nullptr, std::nullopt);
  EXPECT_FALSE(contextStroke.isNone);
  EXPECT_TRUE(contextStroke.isCustom);
  EXPECT_EQ(contextStroke.customLabel, "context-stroke");
}

TEST(EditorShellInternalTest, PaintChipLabelUsesReferenceCustomAndFallbackText) {
  internal::ToolbarPaintSlotState referenced;
  referenced.reference = internal::ToolbarPaintReferenceState{.href = "#very_long_gradient_name"};
  EXPECT_EQ(internal::PaintChipLabel("Fill", referenced), "Fill #very_lon...");

  internal::ToolbarPaintSlotState custom;
  custom.customLabel = "context-fill";
  EXPECT_EQ(internal::PaintChipLabel("Stroke", custom), "Stroke context-fill");

  internal::ToolbarPaintSlotState fallback;
  EXPECT_EQ(internal::PaintChipLabel("Fill", fallback), "Fill custom");
}

TEST(EditorShellInternalTest, SelectionChipLabelsClampAndNormalizeValues) {
  EXPECT_EQ(internal::SelectionSizeChipLabel(Box2d::FromXYWH(10.0, 20.0, -13.6, 7.4)), "14 x 7");
  EXPECT_EQ(internal::SelectionPositionChipLabel(Box2d::FromXYWH(-2.4, 3.6, 10.0, 20.0)),
            "(-2, 4)");
  EXPECT_EQ(internal::SelectionAngleChipLabel(Transform2d::Rotate(0.0)), "0 deg");
  EXPECT_EQ(internal::SelectionAngleChipLabel(Transform2d::Rotate(3.5)), "-159 deg");
}

TEST(EditorShellInternalTest, GeometryHelpersClampPaneWidthAndTransformBounds) {
  EXPECT_TRUE(
      internal::ContainsScreenPoint(Box2d::FromXYWH(10.0, 20.0, 30.0, 40.0), ImVec2(40.0f, 60.0f)));
  EXPECT_FALSE(
      internal::ContainsScreenPoint(Box2d::FromXYWH(10.0, 20.0, 30.0, 40.0), ImVec2(40.1f, 60.0f)));

  EXPECT_FLOAT_EQ(internal::ClampSourcePaneWidthForWindow(500.0f, 1100.0f), 500.0f);
  EXPECT_FLOAT_EQ(internal::ClampSourcePaneWidthForWindow(50.0f, 1100.0f), 240.0f);
  EXPECT_FLOAT_EQ(internal::ClampSourcePaneWidthForWindow(1200.0f, 1100.0f), 660.0f);

  const Box2d transformed = internal::TransformDocumentBox(
      Box2d::FromXYWH(0.0, 0.0, 10.0, 20.0), Transform2d::Translate(Vector2d(5.0, -3.0)));
  EXPECT_EQ(transformed.topLeft, Vector2d(5.0, -3.0));
  EXPECT_EQ(transformed.bottomRight, Vector2d(15.0, 17.0));
}

TEST(EditorShellInternalTest, PendingClickBusyActionPrefersFastRedragThenCancelsBusyRender) {
  EXPECT_EQ(internal::PendingClickBusyActionForState(/*tookFastRedrag=*/true,
                                                     /*rendererBusy=*/true),
            internal::PendingClickBusyAction::CompleteFastRedrag);
  EXPECT_EQ(internal::PendingClickBusyActionForState(/*tookFastRedrag=*/true,
                                                     /*rendererBusy=*/false),
            internal::PendingClickBusyAction::CompleteFastRedrag);

  EXPECT_EQ(internal::PendingClickBusyActionForState(/*tookFastRedrag=*/false,
                                                     /*rendererBusy=*/true),
            internal::PendingClickBusyAction::CancelBusyRender);
  EXPECT_EQ(internal::PendingClickBusyActionForState(/*tookFastRedrag=*/false,
                                                     /*rendererBusy=*/false),
            internal::PendingClickBusyAction::RunIdleClickPath);
}

TEST(EditorShellInternalTest, PendingClickIdleActionWaitsForMarqueeIntentBeforeSlowPath) {
  EXPECT_EQ(internal::PendingClickIdleActionForState(
                /*leftMouseDown=*/true, /*pendingClickCanStartMarquee=*/true,
                /*selectHoldElapsed=*/true, /*selectDragIntent=*/false),
            internal::PendingClickIdleAction::BeginMarquee);
  EXPECT_EQ(internal::PendingClickIdleActionForState(
                /*leftMouseDown=*/true, /*pendingClickCanStartMarquee=*/true,
                /*selectHoldElapsed=*/false, /*selectDragIntent=*/true),
            internal::PendingClickIdleAction::BeginMarquee);

  EXPECT_EQ(internal::PendingClickIdleActionForState(
                /*leftMouseDown=*/true, /*pendingClickCanStartMarquee=*/true,
                /*selectHoldElapsed=*/false, /*selectDragIntent=*/false),
            internal::PendingClickIdleAction::WaitForMarqueeIntent);

  EXPECT_EQ(internal::PendingClickIdleActionForState(
                /*leftMouseDown=*/false, /*pendingClickCanStartMarquee=*/true,
                /*selectHoldElapsed=*/true, /*selectDragIntent=*/true),
            internal::PendingClickIdleAction::DispatchSlowPath);
  EXPECT_EQ(internal::PendingClickIdleActionForState(
                /*leftMouseDown=*/true, /*pendingClickCanStartMarquee=*/false,
                /*selectHoldElapsed=*/true, /*selectDragIntent=*/true),
            internal::PendingClickIdleAction::DispatchSlowPath);
}

TEST(EditorShellInternalTest, ActivePaintStateUsesFillAndStrokeAttributes) {
  ActivePaintStyle style;
  style.fill = "#ff0000";
  style.stroke = "url(#paint)";

  const internal::ToolbarPaintState state = internal::ToolbarPaintStateForActivePaint(style);

  EXPECT_FALSE(state.fill.isNone);
  EXPECT_FALSE(state.fill.isCustom);
  EXPECT_EQ(state.fill.color, css::RGBA::RGB(0xff, 0x00, 0x00));
  EXPECT_FALSE(state.stroke.isNone);
  EXPECT_TRUE(state.stroke.isCustom);
  EXPECT_EQ(state.stroke.customLabel, "url(#paint)");
}

TEST(EditorShellInternalTest, SourceHelpersPreferInitialSourceAndCanonicalizeTrailingNewline) {
  const std::filesystem::path path = TempPathForTest("initial.svg");
  WriteTextFile(path, "<svg id=\"from-file\"/>\n");

  EditorShellOptions sourceOptions;
  sourceOptions.svgPath = path.string();
  sourceOptions.initialSource = "<svg id=\"from-memory\"/>";
  EXPECT_EQ(internal::InitialDocumentSyncSource(sourceOptions), "<svg id=\"from-memory\"/>");

  EditorShellOptions fileOptions;
  fileOptions.svgPath = path.string();
  EXPECT_EQ(internal::InitialDocumentSyncSource(fileOptions), "<svg id=\"from-file\"/>\n");

  EditorShellOptions missingOptions;
  missingOptions.svgPath = path.string() + ".missing";
  EXPECT_TRUE(internal::InitialDocumentSyncSource(missingOptions).empty());

  EXPECT_EQ(internal::CanonicalizeForTextEditor("abc\n"), "abc");
  EXPECT_EQ(internal::CanonicalizeForTextEditor("abc"), "abc");
  EXPECT_TRUE(internal::CanonicalizeForTextEditor("").empty());
}

TEST(EditorShellInternalTest, ReferenceHighlightChipLabelCombinesDirections) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kReferencedSvg));
  auto target = app.document().document().querySelector("#target");
  auto paint = app.document().document().querySelector("#paint");
  auto referrer = app.document().document().querySelector("#referrer");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(paint.has_value());
  ASSERT_TRUE(referrer.has_value());

  ReferenceHighlightSummary summary;
  EXPECT_TRUE(internal::ReferenceHighlightChipLabel(summary).empty());

  summary.referencedElements = {*target, *paint};
  EXPECT_EQ(internal::ReferenceHighlightChipLabel(summary), "-> 2");

  summary.referencingElements = {*target, *paint, *referrer};
  EXPECT_EQ(internal::ReferenceHighlightChipLabel(summary), "-> 2  <- 3");

  summary.referencedElements.clear();
  EXPECT_EQ(internal::ReferenceHighlightChipLabel(summary), "<- 3");
}

TEST(EditorShellInternalTest, ElementCollectionHelpersKeepUniqueElementsAndLabels) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kReferencedSvg));

  auto target = app.document().document().querySelector("#target");
  auto paint = app.document().document().querySelector("#paint");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(paint.has_value());

  std::vector<svg::SVGElement> elements;
  internal::AddUniqueElements(&elements, std::vector<svg::SVGElement>{*target, *paint, *target});

  ASSERT_EQ(elements.size(), 2u);
  EXPECT_TRUE(internal::ContainsElement(elements, *target));
  EXPECT_TRUE(internal::ContainsElement(elements, *paint));
  EXPECT_EQ(internal::ElementContextMenuLabel(*target), "<rect> #target");
  EXPECT_EQ(internal::ElementContextMenuLabel(*paint), "<linearGradient> #paint");
}

TEST(EditorShellInternalTest, ResolveDocumentViewBoxUsesViewBoxIntrinsicSizeAndDefault) {
  EditorApp viewBoxApp;
  ASSERT_TRUE(viewBoxApp.loadFromString(kInitialSvg));
  EXPECT_EQ(internal::ResolveDocumentViewBox(viewBoxApp.document().document()),
            Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  EditorApp intrinsicApp;
  ASSERT_TRUE(intrinsicApp.loadFromString(kIntrinsicSizeSvg));
  EXPECT_EQ(internal::ResolveDocumentViewBox(intrinsicApp.document().document()),
            Box2d::FromXYWH(0.0, 0.0, 160.0, 90.0));

  EditorApp defaultApp;
  ASSERT_TRUE(defaultApp.loadFromString("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"));
  EXPECT_EQ(internal::ResolveDocumentViewBox(defaultApp.document().document()),
            Box2d::FromXYWH(0.0, 0.0, 512.0, 512.0));
}

TEST(EditorShellInternalTest, PaintReferenceStateIncludesSameDocumentSourceRange) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kPaintToolbarSvg));
  svg::SVGDocument& document = app.document().document();

  const internal::ToolbarPaintReferenceState local = internal::ToolbarPaintReferenceStateFor(
      &document, document.source(), svg::Reference("#paint"));
  EXPECT_EQ(local.href, "#paint");
  EXPECT_FALSE(local.external);
  ASSERT_TRUE(local.sourceRange.has_value());
  EXPECT_NE(std::string_view(document.source())
                .substr(local.sourceRange->start, local.sourceRange->end - local.sourceRange->start)
                .find("<linearGradient id=\"paint\""),
            std::string_view::npos);

  const internal::ToolbarPaintReferenceState withoutSource =
      internal::ToolbarPaintReferenceStateFor(&document, std::nullopt, svg::Reference("#paint"));
  EXPECT_FALSE(withoutSource.sourceRange.has_value());

  const internal::ToolbarPaintReferenceState external = internal::ToolbarPaintReferenceStateFor(
      &document, document.source(), svg::Reference("other.svg#paint"));
  EXPECT_TRUE(external.external);
  EXPECT_FALSE(external.sourceRange.has_value());
}

class EditorShellTestAccess {
public:
  static EditorApp& App(EditorShell& shell) { return shell.app_; }
  static const EditorApp& App(const EditorShell& shell) { return shell.app_; }
  static TextEditor& Source(EditorShell& shell) { return shell.textEditor_; }
  static const TextEditor& Source(const EditorShell& shell) { return shell.textEditor_; }

  static bool TryOpenPath(EditorShell& shell, std::string_view path, std::string* error) {
    return shell.tryOpenPath(path, error);
  }

  static bool TrySavePath(EditorShell& shell, std::string_view path, std::string* error) {
    return shell.trySavePath(path, error);
  }

  static void RequestRevert(EditorShell& shell) { shell.requestRevert(); }

  static void RequestSave(EditorShell& shell) { shell.requestSave(); }

  static void RequestSaveAs(EditorShell& shell, std::string error = std::string()) {
    shell.requestSaveAs(std::move(error));
  }

  static void RequestExportViewportSvg(EditorShell& shell, bool includeOverlay,
                                       std::string error = std::string()) {
    shell.requestExportViewportSvg(includeOverlay, std::move(error));
  }

  static bool TryExportViewportSvgToPath(EditorShell& shell, std::string_view path,
                                         std::string* error) {
    return shell.tryExportViewportSvgToPath(path, error);
  }

  static bool PendingViewportExport(const EditorShell& shell) {
    return shell.pendingViewportExport_;
  }

  static bool PendingViewportExportOverlay(const EditorShell& shell) {
    return shell.pendingViewportExportOverlay_;
  }

  static bool OpenFileModalRequested(const EditorShell& shell) {
    return shell.dialogPresenter_.openFileModalRequested();
  }

  static bool SaveFileModalRequested(const EditorShell& shell) {
    return shell.dialogPresenter_.saveFileModalRequested();
  }

  static bool AboutPopupRequested(const EditorShell& shell) {
    return shell.dialogPresenter_.aboutPopupRequested();
  }

  static void MaybeLogResourceDiagnostics(EditorShell& shell, const FrameCostBreakdown& frameCost) {
    shell.maybeLogResourceDiagnostics(frameCost);
  }

  static void MaybeLogFrameMissTelemetry(EditorShell& shell, const FrameCostBreakdown& frameCost) {
    shell.maybeLogFrameMissTelemetry(frameCost);
  }

  static bool HighlightSelectionSourceIfNeeded(EditorShell& shell) {
    return shell.highlightSelectionSourceIfNeeded();
  }

  static void UpdateSourceFocusView(EditorShell& shell, bool scrollToSelection) {
    shell.updateSourceFocusView(scrollToSelection);
  }

  static void SetSourceFocusMode(EditorShell& shell, bool enabled) {
    shell.setSourceFocusMode(enabled);
  }

  static void SetSourcePaneVisible(EditorShell& shell, bool visible) {
    shell.setSourcePaneVisible(visible);
  }

  static void RevealSourceRange(EditorShell& shell, SourceByteRange byteRange) {
    shell.revealSourceRange(byteRange);
  }

  static void UpdateSourceStyleDecorations(EditorShell& shell) {
    shell.updateSourceStyleDecorations();
  }

  static void UpdateSourceHoverPreview(EditorShell& shell) { shell.updateSourceHoverPreview(); }

  static void RefreshReferenceHighlightSummaryIfNeeded(EditorShell& shell) {
    shell.refreshReferenceHighlightSummaryIfNeeded();
  }

  static void ApplyReferenceHighlightPreview(EditorShell& shell) {
    shell.applyReferenceHighlightPreview();
  }

  static void SetReferenceHighlightChipHovered(EditorShell& shell, bool hovered) {
    shell.setReferenceHighlightChipHovered(hovered);
  }

  static void SetReferenceHighlightActive(EditorShell& shell, bool active) {
    shell.referenceHighlightActive_ = active;
  }

  static void ConfigureViewport(EditorShell& shell, const Box2d& documentViewBox) {
    shell.interactionController_.updatePaneLayout(Vector2d(20.0, 30.0), Vector2d(400.0, 260.0),
                                                  documentViewBox);
    (void)shell.interactionController_.resetToActualSize();
  }

  static void RefreshSelectionBoundsCache(EditorShell& shell) {
    shell.renderCoordinator_.refreshSelectionBoundsCache(shell.app_);
  }

  static bool FlushQueuedMutationAndRefreshOverlay(EditorShell& shell) {
    return shell.flushQueuedMutationAndRefreshOverlay();
  }

  static std::size_t DisplayedSelectionBoundsCount(const EditorShell& shell) {
    return shell.renderCoordinator_.selectionBoundsCache().displayedBoundsDoc.size();
  }

  static std::vector<svg::SVGElement> ReferenceHighlightElements(const EditorShell& shell) {
    return shell.referenceHighlightElements();
  }

  static std::vector<svg::SVGElement> CombinedSourcePreviewElements(const EditorShell& shell) {
    return shell.combinedSourcePreviewElements();
  }

  static std::optional<StyleFocus> StyleFocusAtSourceOffset(EditorShell& shell,
                                                            std::size_t sourceOffset) {
    return shell.styleFocusAtSourceOffset(sourceOffset);
  }

  static std::optional<StyleFocus> StyleFocusAtSourceCursor(EditorShell& shell) {
    return shell.styleFocusAtSourceCursor();
  }

  static void ApplyPendingDocumentSpaceReplayInput(EditorShell& shell) {
    shell.applyPendingDocumentSpaceReplayInputForTesting();
  }

  static void ApplyReplayAction(EditorShell& shell, const repro::ReproAction& action) {
    shell.applyReplayActionForTesting(action);
  }

  static void ApplyStyleFocus(EditorShell& shell, StyleFocus styleFocus) {
    shell.applyStyleFocus(std::move(styleFocus));
  }

  static void ApplySourcePartition(EditorShell& shell, FocusPartition partition) {
    shell.applySourcePartition(std::move(partition));
  }

  static void OpenRenderPaneContextMenu(EditorShell& shell, const Vector2d& documentPoint) {
    shell.openRenderPaneContextMenu(documentPoint);
  }

  static void RenderRenderPaneContextMenu(EditorShell& shell) {
    shell.renderRenderPaneContextMenu();
  }

  static std::vector<SourceByteRange> SourceHoverRangesForElements(
      const EditorShell& shell, const std::vector<svg::SVGElement>& elements) {
    return shell.sourceHoverRangesForElements(elements);
  }

  static std::optional<Box2d> SelectionSizeChipScreenRect(EditorShell& shell,
                                                          std::string_view label,
                                                          const Vector2d& anchor) {
    return shell.selectionSizeChipScreenRect(label, anchor);
  }

  static std::optional<Box2d> ReferenceHighlightChipScreenRect(EditorShell& shell,
                                                               std::string_view label) {
    return shell.referenceHighlightChipScreenRect(label);
  }

  static std::optional<Vector2d> SelectionChipAnchorScreen(
      EditorShell& shell,
      std::optional<SelectTool::ActiveGesturePreview> activeGesturePreview = std::nullopt) {
    const std::optional<EditorShell::SelectionChipBounds> bounds =
        shell.selectionChipBounds(activeGesturePreview);
    if (!bounds.has_value()) {
      return std::nullopt;
    }

    return bounds->chipAnchorScreen;
  }

  static std::optional<Box2d> SelectionChipScreenBounds(
      EditorShell& shell,
      std::optional<SelectTool::ActiveGesturePreview> activeGesturePreview = std::nullopt) {
    const std::optional<EditorShell::SelectionChipBounds> bounds =
        shell.selectionChipBounds(activeGesturePreview);
    if (!bounds.has_value()) {
      return std::nullopt;
    }

    return bounds->screenBounds;
  }

  static Box2d ToolPaletteScreenRect(EditorShell& shell, const ImVec2& paneOrigin,
                                     const ImVec2& contentRegion) {
    return shell.toolPaletteScreenRect(paneOrigin, contentRegion);
  }

  static bool CanvasHasSelectableElements(EditorShell& shell) {
    return shell.canvasHasSelectableElements();
  }

  static void SelectAllCanvasElements(EditorShell& shell) { shell.selectAllCanvasElements(); }

  static bool SelectionIsAllText(const EditorShell& shell) { return shell.selectionIsAllText(); }

  static void ConvertSelectedTextToOutlines(EditorShell& shell) {
    shell.convertSelectedTextToOutlines();
  }

  static const std::string& LastConvertTextError(const EditorShell& shell) {
    return shell.lastConvertTextError_;
  }

  static void RenderSourcePane(EditorShell& shell, float paneOriginY, float paneHeight,
                               float paneWidth, ImFont* codeFont) {
    shell.renderSourcePane(paneOriginY, paneHeight, paneWidth, codeFont);
  }

  static void RenderRenderPane(EditorShell& shell, const Vector2d& renderPaneOrigin,
                               const Vector2d& renderPaneSize, ImGuiWindowFlags paneFlags) {
    shell.renderRenderPane(renderPaneOrigin, renderPaneSize, paneFlags);
  }

  static void RenderFillStrokeToolbarWidget(EditorShell& shell) {
    shell.renderFillStrokeToolbarWidget();
  }

  static void RenderToolPalette(EditorShell& shell, const ImVec2& paneOrigin,
                                const ImVec2& contentRegion) {
    shell.renderToolPalette(paneOrigin, contentRegion);
  }

  static void RenderSourcePaneSplitter(EditorShell& shell, float windowWidth, float paneOriginY,
                                       float paneHeight, float sourcePaneWidth) {
    shell.renderSourcePaneSplitter(windowWidth, paneOriginY, paneHeight, sourcePaneWidth);
  }

  static void RenderRightPaneSplitter(EditorShell& shell, float windowWidth, float paneOriginY,
                                      float paneHeight) {
    shell.renderRightPaneSplitter(windowWidth, paneOriginY, paneHeight);
  }

  static void RenderDockedLayerPanelDragHandle(EditorShell& shell) {
    shell.renderDockedLayerPanelDragHandle();
  }

  static void RenderFloatingLayerPanel(EditorShell& shell) { shell.renderFloatingLayerPanel(); }

  static void RenderLayerPanelContents(EditorShell& shell) { shell.renderLayerPanelContents(); }

  static void RenderReferenceHighlightChip(EditorShell& shell) {
    shell.renderReferenceHighlightChip();
  }

  static void RenderSelectionSizeChip(
      EditorShell& shell, const SelectionTransformHandleIntent& hoverTransformIntent,
      const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview) {
    shell.renderSelectionSizeChip(hoverTransformIntent, activeGesturePreview);
  }

  static void ApplyMenuActions(EditorShell& shell, const MenuBarActions& actions) {
    shell.applyMenuActions(actions);
  }

  static void UseInMemoryShapeClipboard(EditorShell& shell) {
    shell.shapeClipboard_ = std::make_unique<InMemoryClipboard>();
  }

  static void ClearShapeClipboard(EditorShell& shell) { shell.shapeClipboard_.reset(); }

  static void SetShapeClipboardText(EditorShell& shell, std::string_view text) {
    ASSERT_NE(shell.shapeClipboard_, nullptr);
    shell.shapeClipboard_->setText(text);
  }

  static std::string ShapeClipboardText(const EditorShell& shell) {
    return shell.shapeClipboard_ != nullptr ? shell.shapeClipboard_->getText() : std::string();
  }

  static bool ShapeClipboardHasText(const EditorShell& shell) {
    return shell.shapeClipboard_ != nullptr && shell.shapeClipboard_->hasText();
  }

  static void CopySelectedShapesToClipboard(EditorShell& shell) {
    shell.copySelectedShapesToClipboard();
  }

  static void CutSelectedShapesToClipboard(EditorShell& shell) {
    shell.cutSelectedShapesToClipboard();
  }

  static void PasteShapesFromClipboard(EditorShell& shell, bool inFront) {
    shell.pasteShapesFromClipboard(inFront);
  }

  static void HandleGlobalShortcuts(EditorShell& shell) { shell.handleGlobalShortcuts(); }

  static bool ActiveToolIsSelect(const EditorShell& shell) {
    return shell.activeTool_ == EditorShell::ActiveTool::Select;
  }

  static bool ActiveToolIsPen(const EditorShell& shell) {
    return shell.activeTool_ == EditorShell::ActiveTool::Pen;
  }

  static bool ActiveToolIsText(const EditorShell& shell) {
    return shell.activeTool_ == EditorShell::ActiveTool::Text;
  }

  static bool PenToolIsDrafting(const EditorShell& shell) { return shell.penTool_.isDrafting(); }

  static bool PenToolIsDraggingAnchor(const EditorShell& shell) {
    return shell.penTool_.isDraggingAnchor();
  }

  static const std::string& PenToolActivePathData(const EditorShell& shell) {
    return shell.penTool_.activePathData();
  }

  static bool PenDragFlushedThisFrame(const EditorShell& shell) {
    return shell.penDragFlushedThisFrame_;
  }

  static bool SelectToolIsMarqueeing(const EditorShell& shell) {
    return shell.selectTool_.isMarqueeing();
  }

  static void BufferPendingClick(EditorShell& shell, const Vector2d& documentPoint,
                                 MouseModifiers modifiers = MouseModifiers{}) {
    shell.interactionController_.bufferPendingClick(documentPoint, modifiers);
  }

  static bool HasPendingClick(const EditorShell& shell) {
    return shell.interactionController_.pendingClick().has_value();
  }

  static void SetPendingSelectClickStartSeconds(EditorShell& shell, double seconds) {
    shell.pendingSelectClickStartSeconds_ = seconds;
  }

  static bool RequestRenderAtEndOfFrame(const EditorShell& shell) {
    return shell.requestRenderAtEndOfFrame_;
  }

  static void ClearRequestRenderAtEndOfFrame(EditorShell& shell) {
    shell.requestRenderAtEndOfFrame_ = false;
  }

  static void SetShowCompositorDebugPanel(EditorShell& shell, bool value) {
    shell.showCompositorDebugPanel_ = value;
  }

  static bool ShowCompositorDebugPanel(const EditorShell& shell) {
    return shell.showCompositorDebugPanel_;
  }

  static bool ShowPerfOverlay(const EditorShell& shell) { return shell.showPerfOverlay_; }

  static void SetLayerPanelDetached(EditorShell& shell, bool value) {
    shell.layerPanelDetached_ = value;
  }

  static void SetLayerPanelDetachDragActive(EditorShell& shell, bool value) {
    shell.layerPanelDetachDragActive_ = value;
  }

  static void SetLayerPanelFloatingNeedsPlacement(EditorShell& shell, bool value) {
    shell.layerPanelFloatingNeedsPlacement_ = value;
  }

  static void SetLayerPanelFloatingGeometry(EditorShell& shell, const ImVec2& pos,
                                            const ImVec2& size) {
    shell.layerPanelFloatingPos_ = pos;
    shell.layerPanelFloatingSize_ = size;
  }

  static bool LayerPanelDetached(const EditorShell& shell) { return shell.layerPanelDetached_; }

  static bool LayerPanelDetachDragActive(const EditorShell& shell) {
    return shell.layerPanelDetachDragActive_;
  }

  static bool LayerPanelFloatingNeedsPlacement(const EditorShell& shell) {
    return shell.layerPanelFloatingNeedsPlacement_;
  }

  static bool SourcePaneVisible(const EditorShell& shell) { return shell.sourcePaneVisible_; }
  static float SourcePaneWidth(const EditorShell& shell) { return shell.sourcePaneWidth_; }
  static void SetSourcePaneWidth(EditorShell& shell, float value) {
    shell.sourcePaneWidth_ = value;
  }
  static bool SourceFocusMode(const EditorShell& shell) { return shell.sourceFocusMode_; }
  static bool SourceFocusOriginatedInStyle(const EditorShell& shell) {
    return shell.sourceFocusOriginatedInStyle_;
  }
  static bool SourceSelectionOriginatedInText(const EditorShell& shell) {
    return shell.sourceSelectionOriginatedInText_;
  }
  static void SetSourceSelectionOriginatedInText(EditorShell& shell, bool value) {
    shell.sourceSelectionOriginatedInText_ = value;
  }
  static bool PreserveSourceEditFocusCursor(const EditorShell& shell) {
    return shell.preserveSourceEditFocusCursor_;
  }
  static void SetPreserveSourceEditFocusCursor(EditorShell& shell, bool value) {
    shell.preserveSourceEditFocusCursor_ = value;
  }
  static const ReferenceHighlightSummary& ReferenceSummary(const EditorShell& shell) {
    return shell.referenceHighlightSummary_;
  }
  static bool ReferenceHighlightActive(const EditorShell& shell) {
    return shell.referenceHighlightActive_;
  }
  static bool ReferenceHighlightChipHovered(const EditorShell& shell) {
    return shell.referenceHighlightChipHovered_;
  }
  static bool StyleSourceDecorationsValid(const EditorShell& shell) {
    return shell.styleSourceDecorationsValid_;
  }
  static std::size_t StyleSourceContributionCount(const EditorShell& shell) {
    return shell.styleSourceContributions_.size();
  }
  static std::optional<Vector2d> RenderContextMenuDocumentPoint(const EditorShell& shell) {
    return shell.renderContextMenuDocumentPoint_;
  }
  static bool RenderContextMenuOpenRequested(const EditorShell& shell) {
    return shell.renderContextMenuOpenRequested_;
  }
  static bool RenderContextMenuHasHitElement(const EditorShell& shell) {
    return shell.renderContextMenuHitElement_.has_value();
  }
  static std::size_t LastReferenceHighlightSelectionSize(const EditorShell& shell) {
    return shell.lastReferenceHighlightSelection_.size();
  }
  static void NoteFrameDelta(EditorShell& shell, float deltaMs) {
    shell.interactionController_.noteFrameDelta(deltaMs);
  }
  static std::uint64_t ResourceDiagnosticsFrame(const EditorShell& shell) {
    return shell.resourceDiagnosticsFrame_;
  }
  static bool FrameMissTelemetryWriteErrorLogged(const EditorShell& shell) {
    return shell.frameMissTelemetryWriteErrorLogged_;
  }
};

void RunFramesUntilDisplayedSelectionBounds(gui::EditorWindow& window, EditorShell& shell) {
  for (int attempt = 0; attempt < 40; ++attempt) {
    window.beginFrame();
    shell.runFrame();
    window.endFrame();
    if (EditorShellTestAccess::DisplayedSelectionBoundsCount(shell) > 0u) {
      return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void DriveGlobalShortcut(EditorShell& shell, const std::vector<ImGuiKey>& keys, bool ctrl = false,
                         bool shift = false, bool super = false) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.ConfigMacOSXBehaviors = false;
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  if (ctrl) {
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
  }
  if (shift) {
    io.AddKeyEvent(ImGuiMod_Shift, true);
  }
  if (super) {
    io.AddKeyEvent(ImGuiMod_Super, true);
  }
  for (ImGuiKey key : keys) {
    io.AddKeyEvent(key, true);
  }

  ImGui::NewFrame();
  EditorShellTestAccess::HandleGlobalShortcuts(shell);
  ImGui::Render();

  for (ImGuiKey key : keys) {
    io.AddKeyEvent(key, false);
  }
  if (super) {
    io.AddKeyEvent(ImGuiMod_Super, false);
  }
  if (shift) {
    io.AddKeyEvent(ImGuiMod_Shift, false);
  }
  if (ctrl) {
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
  }
  ImGui::NewFrame();
  ImGui::Render();
}

void RenderToolbarFrame(gui::EditorWindow& window, EditorShell& shell, const ImVec2& cursor,
                        const ImVec2& mouse, bool mouseDown) {
  (void)window;
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);

  ImGui::NewFrame();
  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(220.0f, 100.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellToolbarMouseHost", nullptr, kHostFlags);
  ImGui::SetCursorScreenPos(cursor);
  EditorShellTestAccess::RenderFillStrokeToolbarWidget(shell);
  ImGui::End();
  ImGui::Render();
}

void ClickToolbar(gui::EditorWindow& window, EditorShell& shell, const ImVec2& cursor,
                  const ImVec2& mouse) {
  RenderToolbarFrame(window, shell, cursor, mouse, /*mouseDown=*/false);
  RenderToolbarFrame(window, shell, cursor, mouse, /*mouseDown=*/true);
  RenderToolbarFrame(window, shell, cursor, mouse, /*mouseDown=*/false);
}

void RenderToolPaletteFrame(gui::EditorWindow& window, EditorShell& shell, const ImVec2& paneOrigin,
                            const ImVec2& contentRegion, const ImVec2& mouse, bool mouseDown) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);

  window.beginFrame();
  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellToolPaletteMouseHost", nullptr, kHostFlags);
  EditorShellTestAccess::RenderToolPalette(shell, paneOrigin, contentRegion);
  ImGui::End();
  window.endFrame();
}

void RenderPaneMouseFrame(gui::EditorWindow& window, EditorShell& shell,
                          const Vector2d& documentPoint, bool mouseDown,
                          const Vector2d& renderPaneOrigin = Vector2d(20.0, 30.0),
                          const Vector2d& renderPaneSize = Vector2d(400.0, 260.0),
                          bool shift = false, bool option = false, bool command = false,
                          bool doubleClick = false) {
  const Vector2d screenPoint = shell.viewportForReadback().documentToScreen(documentPoint);

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.AddKeyEvent(ImGuiMod_Shift, shift);
  io.AddKeyEvent(ImGuiMod_Alt, option);
  io.AddKeyEvent(ImGuiMod_Ctrl, command);
  io.AddMousePosEvent(static_cast<float>(screenPoint.x), static_cast<float>(screenPoint.y));
  io.AddMouseButtonEvent(0, mouseDown);

  window.beginFrame();
  if (doubleClick) {
    io.MouseClickedCount[0] = 2;
  }
  EditorShellTestAccess::RenderRenderPane(
      shell, renderPaneOrigin, renderPaneSize,
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
  window.endFrame();

  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  io.AddKeyEvent(ImGuiMod_Alt, false);
  io.AddKeyEvent(ImGuiMod_Shift, false);
}

void RenderContextMenuFrame(gui::EditorWindow& window, EditorShell& shell) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }

  window.beginFrame();
  ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(220.0f, 120.0f), ImGuiCond_Always);
  ImGui::Begin(
      "EditorShellContextMenuHost", nullptr,
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
  EditorShellTestAccess::RenderRenderPaneContextMenu(shell);
  ImGui::End();
  window.endFrame();
}

Box2d RenderDockedLayerPanelDragHandleFrame(gui::EditorWindow& window, EditorShell& shell,
                                            const ImVec2& mouse, bool mouseDown) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);

  window.beginFrame();
  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(80.0f, 60.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(260.0f, 100.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellDockedLayerPanelDragHost", nullptr, kHostFlags);
  EditorShellTestAccess::RenderDockedLayerPanelDragHandle(shell);
  const ImVec2 itemMin = ImGui::GetItemRectMin();
  const ImVec2 itemMax = ImGui::GetItemRectMax();
  ImGui::End();
  window.endFrame();
  return Box2d(Vector2d(itemMin.x, itemMin.y), Vector2d(itemMax.x, itemMax.y));
}

void RenderFloatingLayerPanelFrame(gui::EditorWindow& window, EditorShell& shell,
                                   const ImVec2& mouse, bool mouseDown) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);

  window.beginFrame();
  EditorShellTestAccess::RenderFloatingLayerPanel(shell);
  window.endFrame();
}

Box2d RenderSourcePaneSplitterFrame(gui::EditorWindow& window, EditorShell& shell,
                                    float sourcePaneWidth, const ImVec2& mouse, bool mouseDown,
                                    float windowWidth = 640.0f) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);

  window.beginFrame();
  EditorShellTestAccess::RenderSourcePaneSplitter(shell, windowWidth, /*paneOriginY=*/0.0f,
                                                  /*paneHeight=*/220.0f, sourcePaneWidth);
  const ImVec2 itemMin = ImGui::GetItemRectMin();
  const ImVec2 itemMax = ImGui::GetItemRectMax();
  window.endFrame();
  return Box2d(Vector2d(itemMin.x, itemMin.y), Vector2d(itemMax.x, itemMax.y));
}

void RenderReferenceHighlightChipFrame(gui::EditorWindow& window, EditorShell& shell,
                                       const ImVec2& mouse, bool mouseDown) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);

  window.beginFrame();
  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellReferenceChipHost", nullptr, kHostFlags);
  EditorShellTestAccess::RenderReferenceHighlightChip(shell);
  ImGui::End();
  window.endFrame();
}

void RenderSelectionSizeChipFrame(
    gui::EditorWindow& window, EditorShell& shell,
    const SelectionTransformHandleIntent& hoverTransformIntent,
    const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }

  window.beginFrame();
  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellSelectionChipHost", nullptr, kHostFlags);
  EditorShellTestAccess::RenderSelectionSizeChip(shell, hoverTransformIntent, activeGesturePreview);
  ImGui::End();
  window.endFrame();
}

TEST(EditorShellTest, HiddenWindowShellConstructsAndRunsFrames) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  {
    EditorShell emptyShell(window, EditorShellOptions{});
    EXPECT_FALSE(emptyShell.valid());
  }

  EditorShellOptions options;
  options.initialSource = std::string(kInitialSvg);
  options.initialPath = "memory.svg";
  options.editorNoticeText = "test notice";
  EditorShell shell(window, std::move(options));

  ASSERT_TRUE(shell.valid());
  EXPECT_FALSE(shell.selectedElementLabelForReadback().has_value());

  window.beginFrame();
  shell.runFrame();
  window.endFrame();

  const LayerInspectorStatusReadback status = shell.layerInspectorStatusForReadback();
  EXPECT_GE(status.viewportDesiredCanvas.x, 0);
  EXPECT_GE(status.viewportDesiredCanvas.y, 0);
  EXPECT_FALSE(shell.selectedElementLabelForReadback().has_value());

  shell.setContentOnlyCaptureForNextFrameForReplay(true);
  shell.overrideViewportForReplay(shell.viewportForReadback());

  window.beginFrame();
  shell.runFrame();
  window.endFrame();

  EXPECT_TRUE(shell.valid());
}

TEST(EditorShellTest, FullFrameSmokeCoversPanelSourceAndContextMenuStates) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  RunFramesUntilDisplayedSelectionBounds(window, shell);

  EditorShellTestAccess::SetShowCompositorDebugPanel(shell, true);
  EditorShellTestAccess::SetLayerPanelDetached(shell, false);
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_TRUE(shell.valid());

  EditorShellTestAccess::SetLayerPanelDetached(shell, true);
  EditorShellTestAccess::SetLayerPanelFloatingNeedsPlacement(shell, true);
  EditorShellTestAccess::SetLayerPanelFloatingGeometry(shell, ImVec2(80.0f, 60.0f),
                                                       ImVec2(260.0f, 220.0f));
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_TRUE(EditorShellTestAccess::LayerPanelDetached(shell));

  EditorShellTestAccess::SetSourcePaneVisible(shell, false);
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));

  EditorShellTestAccess::SetSourcePaneVisible(shell, true);
  EditorShellTestAccess::SetSourceFocusMode(shell, true);
  EditorShellTestAccess::UpdateSourceFocusView(shell, /*scrollToSelection=*/true);
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_TRUE(EditorShellTestAccess::SourceFocusMode(shell));

  EditorShellTestAccess::OpenRenderPaneContextMenu(shell, Vector2d(12.0, 14.0));
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuOpenRequested(shell));
}

TEST(EditorShellTest, IntrinsicSizeDocumentInitializesViewportWithoutViewBox) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kIntrinsicSizeSvg, "intrinsic.svg"));
  ASSERT_TRUE(shell.valid());

  window.beginFrame();
  EditorShellTestAccess::RenderRenderPane(
      shell, Vector2d(0.0, 0.0), Vector2d(320.0, 200.0),
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
  window.endFrame();

  const ViewportState viewport = shell.viewportForReadback();
  EXPECT_GT(viewport.documentViewBox.width(), 100.0);
  EXPECT_GT(viewport.documentViewBox.height(), 50.0);
}

TEST(EditorShellTest, SelectionReadbackAndIdleWakeReflectSourceState) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  EXPECT_FALSE(shell.nextIdleWakeSeconds().has_value());
  EXPECT_FALSE(shell.selectedElementLabelForReadback().has_value());

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  EXPECT_EQ(shell.selectedElementLabelForReadback(), "<rect> #target");
  EXPECT_TRUE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));
  ASSERT_FALSE(EditorShellTestAccess::Source(shell).getSelectedText().empty());

  const std::vector<SourceByteRange> ranges =
      EditorShellTestAccess::SourceHoverRangesForElements(shell, {*target});
  ASSERT_FALSE(ranges.empty());
  EditorShellTestAccess::RevealSourceRange(shell, ranges.front());

  const std::optional<float> idleWake = shell.nextIdleWakeSeconds();
  ASSERT_TRUE(idleWake.has_value());
  EXPECT_GE(*idleWake, 0.0f);
}

TEST(EditorShellTest, LayerInspectorReadbackIncludesSelectedStyleAndPathDiagnostics) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  constexpr std::string_view kDiagnosticSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <path id="target" d="M 10 12 L 50 12 L 50 36 Z" style="fill: #010203; stroke: #040506"/>
</svg>
)svg";
  EditorShell shell(window, OptionsWithSource(kDiagnosticSvg, "diagnostics.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  (void)target->getComputedStyle();
  RunFramesUntilDisplayedSelectionBounds(window, shell);

  const LayerInspectorStatusReadback status = shell.layerInspectorStatusForReadback();

  ASSERT_TRUE(status.selectedStyleAttribute.has_value());
  EXPECT_NE(status.selectedStyleAttribute->find("fill: #010203"), std::string::npos);
  ASSERT_TRUE(status.selectedLocalStyleFill.has_value());
  EXPECT_FALSE(status.selectedLocalStyleFill->empty());
  ASSERT_TRUE(status.selectedComputedFill.has_value());
  EXPECT_FALSE(status.selectedComputedFill->empty());
  ASSERT_TRUE(status.selectedRenderingInstanceFill.has_value());
  EXPECT_FALSE(status.selectedRenderingInstanceFill->empty());
  ASSERT_TRUE(status.selectedPathDataAttribute.has_value());
  EXPECT_EQ(*status.selectedPathDataAttribute, "M 10 12 L 50 12 L 50 36 Z");
}

TEST(EditorShellTest, ReplayActionsSwitchToolsAndIgnoreUnknownToolNames) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "pen",
                                           });
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "text",
                                           });
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "select",
                                           });
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "unsupported",
                                           });
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::CommitPenPath,
                                           });
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
}

TEST(EditorShellTest, ReplayActionSelectCommitsDraftPenPath) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "pen",
                                           });
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(25.0, 16.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(25.0, 16.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "select",
                                           });

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);
}

TEST(EditorShellTest, ReplayActionCommitPenPathCommitsDraftPathInPlace) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "pen",
                                           });
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(8.0, 9.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(8.0, 9.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(28.0, 19.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(28.0, 19.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::CommitPenPath,
                                           });

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);
}

TEST(EditorShellTest, ReplayStyleActionsUpdateSelectionAndActivePaintFallbacks) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetStyleProperty,
                                               .propertyName = "fill",
                                               .propertyValue = "#010203",
                                           });
  (void)EditorShellTestAccess::App(shell).flushFrame();
  target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(target->getAttribute("style").has_value());
  EXPECT_NE(target->getAttribute("style")->str().find("fill: #010203"), std::string::npos);

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetStyleProperty,
                                               .propertyName = "stroke",
                                               .propertyValue = "#040506",
                                           });
  (void)EditorShellTestAccess::App(shell).flushFrame();
  target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(target->getAttribute("style").has_value());
  EXPECT_NE(target->getAttribute("style")->str().find("stroke: #040506"), std::string::npos);

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetStyleProperty,
                                               .propertyName = "stroke-width",
                                               .propertyValue = "7.5",
                                           });
  (void)EditorShellTestAccess::App(shell).flushFrame();
  target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(target->getAttribute("style").has_value());
  EXPECT_NE(target->getAttribute("style")->str().find("stroke-width: 7.5"), std::string::npos);

  EditorShellTestAccess::App(shell).clearSelection();
  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetStyleProperty,
                                               .propertyName = "stroke-width",
                                               .propertyValue = "not-a-number",
                                           });
  EXPECT_TRUE(EditorShellTestAccess::App(shell).document().queue().empty());
}

TEST(EditorShellTest, ConstructsFromSvgPathAndKeepsInvalidSourceEditable) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  const std::filesystem::path validPath = TempPathForTest("valid.svg");
  WriteTextFile(validPath, kInitialSvg);

  EditorShellOptions pathOptions;
  pathOptions.svgPath = validPath.string();
  EditorShell pathShell(window, std::move(pathOptions));

  ASSERT_TRUE(pathShell.valid());
  EXPECT_TRUE(EditorShellTestAccess::App(pathShell).hasDocument());
  ASSERT_TRUE(EditorShellTestAccess::App(pathShell).currentFilePath().has_value());
  EXPECT_EQ(*EditorShellTestAccess::App(pathShell).currentFilePath(), validPath.string());

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));

  EXPECT_TRUE(invalidShell.valid());
  EXPECT_FALSE(EditorShellTestAccess::App(invalidShell).hasDocument());
  EXPECT_EQ(EditorShellTestAccess::Source(invalidShell).getText(), "<svg><rect></svg>");
}

TEST(EditorShellTest, OpenSaveAndRevertUpdateDocumentAndSourceState) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  std::string error;
  EXPECT_FALSE(
      EditorShellTestAccess::TryOpenPath(shell, TempPathForTest("missing.svg").string(), &error));
  EXPECT_EQ(error, "Could not open file.");

  const std::filesystem::path invalidPath = TempPathForTest("invalid.svg");
  WriteTextFile(invalidPath, "<svg><rect></svg>");
  error.clear();
  EXPECT_FALSE(EditorShellTestAccess::TryOpenPath(shell, invalidPath.string(), &error));
  EXPECT_EQ(error, "Failed to parse SVG.");

  const std::filesystem::path openPath = TempPathForTest("opened.svg");
  constexpr std::string_view kOpenedSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg"><rect id="opened"/></svg>)svg";
  WriteTextFile(openPath, kOpenedSvg);
  error.clear();
  EXPECT_TRUE(EditorShellTestAccess::TryOpenPath(shell, openPath.string(), &error)) << error;
  EXPECT_TRUE(EditorShellTestAccess::App(shell).hasDocument());
  EXPECT_NE(EditorShellTestAccess::Source(shell).getText().find("opened"), std::string::npos);

  error.clear();
  EXPECT_FALSE(EditorShellTestAccess::TrySavePath(shell, "", &error));
  EXPECT_EQ(error, "Choose a file path.");

  EditorShell invalidSaveShell(window, OptionsWithSource("<svg><rect></svg>", "broken-save.svg"));
  ASSERT_TRUE(invalidSaveShell.valid());
  error.clear();
  EXPECT_FALSE(EditorShellTestAccess::TrySavePath(invalidSaveShell, "ignored.svg", &error));
  EXPECT_EQ(error, "No SVG document is loaded.");

  const std::filesystem::path savePath = TempPathForTest("saved.svg");
  error.clear();
  EXPECT_TRUE(EditorShellTestAccess::TrySavePath(shell, savePath.string(), &error)) << error;
  EXPECT_NE(ReadTextFile(savePath).find("opened"), std::string::npos);
  EXPECT_FALSE(EditorShellTestAccess::App(shell).isDirty());

  error.clear();
  EXPECT_FALSE(EditorShellTestAccess::TrySavePath(
      shell, TempPathForTest("missing_directory/saved.svg").string(), &error));
  EXPECT_FALSE(error.empty());

  EditorShellTestAccess::Source(shell).setText("<svg>dirty</svg>");
  EditorShellTestAccess::App(shell).markDirty();
  EditorShellTestAccess::RequestRevert(shell);

  EXPECT_FALSE(EditorShellTestAccess::App(shell).isDirty());
  EXPECT_NE(EditorShellTestAccess::Source(shell).getText().find("opened"), std::string::npos);
}

TEST(EditorShellTest, RevertRequestIgnoresGuardStates) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  EditorShellTestAccess::RequestRevert(invalidShell);
  EXPECT_FALSE(EditorShellTestAccess::App(invalidShell).hasDocument());

  EditorShell cleanShell(window, OptionsWithSource(kInitialSvg, "clean.svg"));
  ASSERT_TRUE(cleanShell.valid());
  const std::string cleanSource = EditorShellTestAccess::Source(cleanShell).getText();
  EditorShellTestAccess::RequestRevert(cleanShell);
  EXPECT_EQ(EditorShellTestAccess::Source(cleanShell).getText(), cleanSource);
  EXPECT_FALSE(EditorShellTestAccess::App(cleanShell).isDirty());

  EditorShell dirtyWithoutCleanSource(window, OptionsWithSource(kInitialSvg, "dirty.svg"));
  ASSERT_TRUE(dirtyWithoutCleanSource.valid());
  EditorShellTestAccess::Source(dirtyWithoutCleanSource).setText("<svg>dirty</svg>");
  EditorShellTestAccess::App(dirtyWithoutCleanSource).setCleanSourceText("");
  EditorShellTestAccess::App(dirtyWithoutCleanSource).markDirty();

  EditorShellTestAccess::RequestRevert(dirtyWithoutCleanSource);

  EXPECT_EQ(EditorShellTestAccess::Source(dirtyWithoutCleanSource).getText(), "<svg>dirty</svg>");
  EXPECT_TRUE(EditorShellTestAccess::App(dirtyWithoutCleanSource).isDirty());
}

TEST(EditorShellTest, SaveRequestsUseCurrentPathAndFallbackToSaveAs) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  const std::filesystem::path savePath = TempPathForTest("request_save.svg");
  EditorShell shell(window, OptionsWithSource(kInitialSvg, savePath.string()));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::RequestSave(shell);
  EXPECT_TRUE(std::filesystem::exists(savePath));
  EXPECT_NE(ReadTextFile(savePath).find("target"), std::string::npos);

  EditorShell untitledShell(window, OptionsWithSource(kInitialSvg, ""));
  ASSERT_TRUE(untitledShell.valid());
  EditorShellTestAccess::App(untitledShell).setCurrentFilePath("");
  EditorShellTestAccess::RequestSaveAs(untitledShell, "choose a path");
  EditorShellTestAccess::RequestSave(untitledShell);
  EXPECT_TRUE(untitledShell.valid());
}

TEST(EditorShellTest, ViewportSvgExportRequestsAndWritesCroppedSvg) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  const std::filesystem::path sourcePath = TempPathForTest("source.svg");
  EditorShell shell(window, OptionsWithSource(kInitialSvg, sourcePath.string()));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  EditorShellTestAccess::RequestExportViewportSvg(shell, /*includeOverlay=*/false);
  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_FALSE(EditorShellTestAccess::PendingViewportExportOverlay(shell));

  EditorShellTestAccess::RequestExportViewportSvg(shell, /*includeOverlay=*/true, "try again");
  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExportOverlay(shell));

  const std::filesystem::path exportPath = TempPathForTest("viewport.svg");
  std::string error;
  EXPECT_TRUE(EditorShellTestAccess::TryExportViewportSvgToPath(shell, exportPath.string(), &error))
      << error;
  EXPECT_FALSE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_FALSE(EditorShellTestAccess::PendingViewportExportOverlay(shell));
  const std::string exported = ReadTextFile(exportPath);
  EXPECT_NE(exported.find("<svg"), std::string::npos);
  EXPECT_NE(exported.find("target"), std::string::npos);

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  error.clear();
  EXPECT_FALSE(
      EditorShellTestAccess::TryExportViewportSvgToPath(invalidShell, exportPath.string(), &error));
  EXPECT_EQ(error, "No document is open to export.");

  error.clear();
  EXPECT_FALSE(EditorShellTestAccess::TryExportViewportSvgToPath(
      shell, TempPathForTest("missing_directory/viewport.svg").string(), &error));
  EXPECT_FALSE(error.empty());
}

TEST(EditorShellTest, ViewportSvgExportRequestUsesUntitledDocument) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, ""));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::App(shell).setCurrentFilePath("");

  EditorShellTestAccess::RequestExportViewportSvg(shell, /*includeOverlay=*/false);

  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_FALSE(EditorShellTestAccess::PendingViewportExportOverlay(shell));
}

TEST(EditorShellTest, DiagnosticsLoggingHonorsEnvironmentTargets) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  FrameCostBreakdown frameCost;
  frameCost.overlay.captureMs = 2.0;
  frameCost.compositedUpload.uploadMs = 3.0;
  frameCost.compositedRender.cachedMs = 4.0;

  unsetenv("DONNER_EDITOR_RESOURCE_LOG");
  unsetenv("DONNER_EDITOR_FRAME_MISS_LOG");
  EditorShellTestAccess::MaybeLogResourceDiagnostics(shell, frameCost);
  EXPECT_EQ(EditorShellTestAccess::ResourceDiagnosticsFrame(shell), 0u);

  setenv("DONNER_EDITOR_RESOURCE_LOG", "1", /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogResourceDiagnostics(shell, frameCost);
  EXPECT_EQ(EditorShellTestAccess::ResourceDiagnosticsFrame(shell), 1u);
  EditorShellTestAccess::MaybeLogResourceDiagnostics(shell, frameCost);
  EXPECT_EQ(EditorShellTestAccess::ResourceDiagnosticsFrame(shell), 2u);
  for (int frame = 3; frame <= 60; ++frame) {
    EditorShellTestAccess::MaybeLogResourceDiagnostics(shell, frameCost);
  }
  EXPECT_EQ(EditorShellTestAccess::ResourceDiagnosticsFrame(shell), 60u);
  unsetenv("DONNER_EDITOR_RESOURCE_LOG");

  const std::filesystem::path telemetryPath = TempPathForTest("frame_miss.jsonl");
  std::filesystem::remove(telemetryPath);
  setenv("DONNER_EDITOR_FRAME_MISS_LOG", telemetryPath.string().c_str(), /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_FALSE(std::filesystem::exists(telemetryPath));

  EditorShellTestAccess::NoteFrameDelta(shell, 25.0f);
  setenv("DONNER_EDITOR_FRAME_MISS_LOG", "false", /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_FALSE(std::filesystem::exists(telemetryPath));

  setenv("DONNER_EDITOR_FRAME_MISS_LOG", telemetryPath.string().c_str(), /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_NE(ReadTextFile(telemetryPath).find("frame_budget_miss"), std::string::npos);

  for (std::string_view stderrTarget : {"", "1", "true", "stderr"}) {
    setenv("DONNER_EDITOR_FRAME_MISS_LOG", std::string(stderrTarget).c_str(), /*overwrite=*/1);
    EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
    EXPECT_FALSE(EditorShellTestAccess::FrameMissTelemetryWriteErrorLogged(shell));
  }

  setenv("DONNER_EDITOR_FRAME_MISS_LOG", "0", /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_FALSE(EditorShellTestAccess::FrameMissTelemetryWriteErrorLogged(shell));

  unsetenv("DONNER_EDITOR_FRAME_MISS_LOG");
  setenv("DONNER_EDITOR_RESOURCE_LOG", "1", /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_FALSE(EditorShellTestAccess::FrameMissTelemetryWriteErrorLogged(shell));
  unsetenv("DONNER_EDITOR_RESOURCE_LOG");

  const std::filesystem::path badTelemetryPath =
      TempPathForTest("missing_directory/frame_miss.jsonl");
  setenv("DONNER_EDITOR_FRAME_MISS_LOG", badTelemetryPath.string().c_str(), /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_TRUE(EditorShellTestAccess::FrameMissTelemetryWriteErrorLogged(shell));
  unsetenv("DONNER_EDITOR_FRAME_MISS_LOG");
}

TEST(EditorShellTest, SourceFocusAndStyleDecorationsTrackSelectionAndDirtySource) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  EXPECT_TRUE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));
  EditorShellTestAccess::SetSourceFocusMode(shell, true);
  EXPECT_TRUE(EditorShellTestAccess::SourceFocusMode(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).hasFocusPartition());

  EditorShellTestAccess::UpdateSourceStyleDecorations(shell);
  EXPECT_TRUE(EditorShellTestAccess::StyleSourceDecorationsValid(shell));
  EXPECT_GT(EditorShellTestAccess::StyleSourceContributionCount(shell), 0u);
  EXPECT_GT(EditorShellTestAccess::Source(shell).sourceStyleDecorations().size(), 0u);

  const std::size_t contributionCount = EditorShellTestAccess::StyleSourceContributionCount(shell);
  EditorShellTestAccess::UpdateSourceStyleDecorations(shell);
  EXPECT_EQ(EditorShellTestAccess::StyleSourceContributionCount(shell), contributionCount);

  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.setSelection(Coordinates(0, 0), Coordinates(0, 0));
  source.insertText(" ");
  EditorShellTestAccess::UpdateSourceStyleDecorations(shell);
  EXPECT_FALSE(EditorShellTestAccess::StyleSourceDecorationsValid(shell));
  EXPECT_TRUE(source.sourceStyleDecorations().empty());
}

TEST(EditorShellTest, StyleFocusCursorAndPartitionGuards) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());

  const std::string sourceText = EditorShellTestAccess::Source(shell).getText();
  const std::size_t fillOffset = sourceText.find("fill");
  ASSERT_NE(fillOffset, std::string::npos);
  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.setCursorPosition(source.getCoordinatesAtByteOffset(fillOffset));

  std::optional<StyleFocus> cursorFocus = EditorShellTestAccess::StyleFocusAtSourceCursor(shell);
  ASSERT_TRUE(cursorFocus.has_value());
  EXPECT_FALSE(cursorFocus->impactedElements.empty());

  EXPECT_FALSE(EditorShellTestAccess::StyleFocusAtSourceOffset(shell, 1).has_value());

  EditorShellTestAccess::SetSourceFocusMode(shell, false);
  EditorShellTestAccess::ApplySourcePartition(
      shell, FocusPartition{
                 .fullColor = {{.startLine = 1, .endLine = 2}},
                 .referenceColor = {{.startLine = 2, .endLine = 3}},
                 .dimmed = {{.startLine = 0, .endLine = 1}},
                 .hidden = {{.startLine = 3, .endLine = 4}},
             });
  EXPECT_TRUE(source.hasFocusPartition());

  source.insertText(" ");
  EXPECT_FALSE(EditorShellTestAccess::StyleFocusAtSourceCursor(shell).has_value());
}

TEST(EditorShellTest, RenderContextMenuOpenStoresPointAndHitElement) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::OpenRenderPaneContextMenu(shell, Vector2d(12.0, 14.0));

  EXPECT_TRUE(EditorShellTestAccess::RenderContextMenuOpenRequested(shell));
  ASSERT_TRUE(EditorShellTestAccess::RenderContextMenuDocumentPoint(shell).has_value());
  EXPECT_EQ(*EditorShellTestAccess::RenderContextMenuDocumentPoint(shell), Vector2d(12.0, 14.0));
  EXPECT_TRUE(EditorShellTestAccess::RenderContextMenuHasHitElement(shell));

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  EditorShellTestAccess::OpenRenderPaneContextMenu(invalidShell, Vector2d(1.0, 1.0));
  EXPECT_TRUE(EditorShellTestAccess::RenderContextMenuOpenRequested(invalidShell));
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuHasHitElement(invalidShell));
}

TEST(EditorShellTest, RenderContextMenuRendersHitEmptyAndNoDocumentStates) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::OpenRenderPaneContextMenu(shell, Vector2d(12.0, 14.0));
  RenderContextMenuFrame(window, shell);
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuOpenRequested(shell));
  EXPECT_TRUE(EditorShellTestAccess::RenderContextMenuHasHitElement(shell));

  EditorShellTestAccess::OpenRenderPaneContextMenu(shell, Vector2d(400.0, 300.0));
  RenderContextMenuFrame(window, shell);
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuOpenRequested(shell));
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuHasHitElement(shell));

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  EditorShellTestAccess::OpenRenderPaneContextMenu(invalidShell, Vector2d(1.0, 1.0));
  RenderContextMenuFrame(window, invalidShell);
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuOpenRequested(invalidShell));
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuHasHitElement(invalidShell));
}

TEST(EditorShellTest, DocumentSpaceReplayInputSelectsHitElementAndHandlesGuards) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  EXPECT_TRUE(EditorShellTestAccess::App(shell).selectedElements().empty());

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  invalidShell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 14.0),
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(invalidShell);
  EXPECT_TRUE(EditorShellTestAccess::App(invalidShell).selectedElements().empty());

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 14.0),
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 14.0),
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(20.0, 30.0),
      .hitElementId = std::string(),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");
}

TEST(EditorShellTest, DocumentSpaceReplayInputRoutesSelectPressAndRelease) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 14.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(24.0, 22.0),
      .leftMouseDown = true,
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 14.0),
      .leftMouseReleased = true,
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));
}

TEST(EditorShellTest, DocumentSpaceReplayInputRoutesTextToolClick) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  DriveGlobalShortcut(shell, {ImGuiKey_T});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(30.0, 40.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  const svg::SVGElement selected = *EditorShellTestAccess::App(shell).selectedElement();
  EXPECT_EQ(selected.type(), svg::ElementType::Text);
  ASSERT_TRUE(selected.getAttribute("x").has_value());
  EXPECT_EQ(*selected.getAttribute("x"), "30");
  ASSERT_TRUE(selected.getAttribute("y").has_value());
  EXPECT_EQ(*selected.getAttribute("y"), "40");
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find(">Text<"),
            std::string_view::npos);
}

TEST(EditorShellTest, DocumentSpaceReplayInputRoutesPenPressReleaseAndHover) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  DriveGlobalShortcut(shell, {ImGuiKey_P});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolActivePathData(shell).empty());

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(10.0, 12.0),
      .leftMouseDown = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  EXPECT_TRUE(EditorShellTestAccess::PenDragFlushedThisFrame(shell));
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolActivePathData(shell).empty());
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(18.0, 24.0),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
}

TEST(EditorShellTest, SourcePaneVisibilityRevealAndHoverRangesUseCurrentDocumentSource) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  const std::vector<SourceByteRange> ranges =
      EditorShellTestAccess::SourceHoverRangesForElements(shell, {*target});
  ASSERT_FALSE(ranges.empty());
  EXPECT_LT(ranges.front().start, ranges.front().end);

  ASSERT_TRUE(EditorShellTestAccess::Source(shell).setHoverSourceRanges(ranges));
  EditorShellTestAccess::SetSourcePaneVisible(shell, false);
  EXPECT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).hoverSourceRanges().empty());

  EditorShellTestAccess::RevealSourceRange(shell, ranges.front());
  EXPECT_TRUE(EditorShellTestAccess::SourcePaneVisible(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).nextFlashWakeSeconds().has_value());

  EditorShellTestAccess::Source(shell).setText("out of sync");
  EXPECT_TRUE(EditorShellTestAccess::SourceHoverRangesForElements(shell, {*target}).empty());
}

TEST(EditorShellTest, StyleFocusSelectionPathUsesSourceCursorContext) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());

  const std::string sourceText = EditorShellTestAccess::Source(shell).getText();
  const std::size_t styleOffset = sourceText.find("fill");
  ASSERT_NE(styleOffset, std::string::npos);

  std::optional<StyleFocus> styleFocus =
      EditorShellTestAccess::StyleFocusAtSourceOffset(shell, styleOffset);
  ASSERT_TRUE(styleFocus.has_value());
  EXPECT_FALSE(styleFocus->impactedElements.empty());

  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.setSelection(source.getCoordinatesAtByteOffset(styleOffset),
                      source.getCoordinatesAtByteOffset(styleOffset));
  EditorShellTestAccess::ApplyStyleFocus(shell, *styleFocus);

  EXPECT_TRUE(EditorShellTestAccess::SourceFocusOriginatedInStyle(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).selectedElements().empty());

  EditorShellTestAccess::SetSourceFocusMode(shell, true);
  EXPECT_TRUE(EditorShellTestAccess::SourceFocusMode(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).hasFocusPartition());
}

TEST(EditorShellTest, ReferenceHighlightPreviewCombinesElementsAndSourceRanges) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  EditorShellTestAccess::RefreshReferenceHighlightSummaryIfNeeded(shell);
  const ReferenceHighlightSummary& summary = EditorShellTestAccess::ReferenceSummary(shell);
  EXPECT_EQ(summary.referencedElements.size(), 2u);
  EXPECT_TRUE(summary.referencingElements.empty());
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightElements(shell).empty());

  EditorShellTestAccess::SetReferenceHighlightChipHovered(shell, true);
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightChipHovered(shell));
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightActive(shell));
  EXPECT_EQ(EditorShellTestAccess::ReferenceHighlightElements(shell).size(), 2u);
  EXPECT_EQ(EditorShellTestAccess::CombinedSourcePreviewElements(shell).size(), 2u);
  EXPECT_EQ(EditorShellTestAccess::Source(shell).hoverSourceRanges().size(), 2u);

  EditorShellTestAccess::SetReferenceHighlightChipHovered(shell, false);
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightChipHovered(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).hoverSourceRanges().empty());

  EditorShellTestAccess::SetReferenceHighlightChipHovered(shell, false);
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightChipHovered(shell));
}

TEST(EditorShellTest, ReferenceHighlightSummaryClearsForEmptySelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::RefreshReferenceHighlightSummaryIfNeeded(shell);

  EXPECT_EQ(EditorShellTestAccess::ReferenceSummary(shell).totalCount(), 0u);
  EXPECT_EQ(EditorShellTestAccess::LastReferenceHighlightSelectionSize(shell), 0u);
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightElements(shell).empty());
  EXPECT_TRUE(EditorShellTestAccess::CombinedSourcePreviewElements(shell).empty());
}

TEST(EditorShellTest, ShellGeometryHelpersClampToViewportAndSelectionCache) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  EditorShellTestAccess::RefreshReferenceHighlightSummaryIfNeeded(shell);

  const std::optional<Box2d> sizeChip =
      EditorShellTestAccess::SelectionSizeChipScreenRect(shell, "40 x 24", Vector2d(30.0, 42.0));
  ASSERT_TRUE(sizeChip.has_value());
  EXPECT_GE(sizeChip->topLeft.x, 20.0);
  EXPECT_GE(sizeChip->topLeft.y, 30.0);

  const std::optional<Box2d> noSizeChip =
      EditorShellTestAccess::SelectionSizeChipScreenRect(shell, "", Vector2d(30.0, 42.0));
  EXPECT_FALSE(noSizeChip.has_value());

  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightChipScreenRect(shell, "").has_value());
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightChipScreenRect(shell, "-> 2").has_value());

  const Box2d palette = EditorShellTestAccess::ToolPaletteScreenRect(shell, ImVec2(10.0f, 20.0f),
                                                                     ImVec2(500.0f, 300.0f));
  EXPECT_GT(palette.width(), 0.0);
  EXPECT_GT(palette.height(), 0.0);
}

TEST(EditorShellTest, PrivateUiRenderHelpersCoverPaneToolbarAndPanelStates) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  if (!ImGui::GetIO().Fonts->IsBuilt()) {
    ImGui::GetIO().Fonts->Build();
  }
  window.beginFrame();
  ImGuiIO& io = ImGui::GetIO();
  ASSERT_FALSE(io.Fonts->Fonts.empty());
  EditorShellTestAccess::RenderSourcePane(shell, /*paneOriginY=*/0.0f, /*paneHeight=*/180.0f,
                                          /*paneWidth=*/260.0f, io.Fonts->Fonts[0]);

  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(280.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(340.0f, 220.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellPrivateUiHost", nullptr, kHostFlags);
  EditorShellTestAccess::RenderFillStrokeToolbarWidget(shell);
  EditorShellTestAccess::RenderLayerPanelContents(shell);
  ImGui::End();

  EditorShellTestAccess::RenderSourcePaneSplitter(shell, /*windowWidth=*/640.0f,
                                                  /*paneOriginY=*/0.0f, /*paneHeight=*/220.0f,
                                                  /*sourcePaneWidth=*/260.0f);
  EditorShellTestAccess::RenderRightPaneSplitter(shell, /*windowWidth=*/640.0f,
                                                 /*paneOriginY=*/0.0f, /*paneHeight=*/220.0f);
  window.endFrame();

  EditorShellTestAccess::SetSourcePaneVisible(shell, false);
  EditorShellTestAccess::SetSourceFocusMode(shell, true);
  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>"));
  ASSERT_TRUE(invalidShell.valid());

  if (!ImGui::GetIO().Fonts->IsBuilt()) {
    ImGui::GetIO().Fonts->Build();
  }
  window.beginFrame();
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(240.0f, 80.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellInvalidToolbarHost", nullptr, kHostFlags);
  EditorShellTestAccess::RenderFillStrokeToolbarWidget(invalidShell);
  EditorShellTestAccess::RenderLayerPanelContents(invalidShell);
  ImGui::End();
  EditorShellTestAccess::RenderSourcePaneSplitter(shell, /*windowWidth=*/640.0f,
                                                  /*paneOriginY=*/0.0f, /*paneHeight=*/220.0f,
                                                  /*sourcePaneWidth=*/0.0f);
  EditorShellTestAccess::RenderRightPaneSplitter(shell, /*windowWidth=*/640.0f,
                                                 /*paneOriginY=*/0.0f, /*paneHeight=*/220.0f);
  window.endFrame();

  EXPECT_TRUE(shell.valid());
  EXPECT_TRUE(invalidShell.valid());
}

TEST(EditorShellTest, CompositorDebugPanelDragHandleDetachesAndMovesFloatingPanel) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::SetShowCompositorDebugPanel(shell, true);

  const Box2d handleRect =
      RenderDockedLayerPanelDragHandleFrame(window, shell, ImVec2(-100.0f, -100.0f),
                                            /*mouseDown=*/false);
  const ImVec2 handleCenter(
      static_cast<float>((handleRect.topLeft.x + handleRect.bottomRight.x) * 0.5),
      static_cast<float>((handleRect.topLeft.y + handleRect.bottomRight.y) * 0.5));
  RenderDockedLayerPanelDragHandleFrame(window, shell, handleCenter, /*mouseDown=*/true);
  RenderDockedLayerPanelDragHandleFrame(window, shell,
                                        ImVec2(handleCenter.x + 30.0f, handleCenter.y + 25.0f),
                                        /*mouseDown=*/true);

  EXPECT_TRUE(EditorShellTestAccess::LayerPanelDetached(shell));
  EXPECT_TRUE(EditorShellTestAccess::LayerPanelDetachDragActive(shell));
  EXPECT_TRUE(EditorShellTestAccess::LayerPanelFloatingNeedsPlacement(shell));

  RenderFloatingLayerPanelFrame(window, shell, ImVec2(150.0f, 125.0f), /*mouseDown=*/true);

  EXPECT_TRUE(EditorShellTestAccess::LayerPanelDetached(shell));
  EXPECT_FALSE(EditorShellTestAccess::LayerPanelFloatingNeedsPlacement(shell));

  RenderFloatingLayerPanelFrame(window, shell, ImVec2(150.0f, 125.0f), /*mouseDown=*/false);

  EXPECT_TRUE(EditorShellTestAccess::LayerPanelDetached(shell));
  EXPECT_FALSE(EditorShellTestAccess::LayerPanelDetachDragActive(shell));
}

TEST(EditorShellTest, SourcePaneSplitterDragCollapsesPane) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  const Box2d sourceRect = RenderSourcePaneSplitterFrame(
      window, shell, /*sourcePaneWidth=*/260.0f, ImVec2(-100.0f, -100.0f), /*mouseDown=*/false);
  const ImVec2 sourceCenter(
      static_cast<float>((sourceRect.topLeft.x + sourceRect.bottomRight.x) * 0.5),
      static_cast<float>((sourceRect.topLeft.y + sourceRect.bottomRight.y) * 0.5));
  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/260.0f, sourceCenter,
                                /*mouseDown=*/true);
  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/260.0f,
                                ImVec2(sourceCenter.x - 120.0f, sourceCenter.y),
                                /*mouseDown=*/true);

  EXPECT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));
}

TEST(EditorShellTest, SourcePaneSplitterDragExpandsVisiblePane) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::SetSourcePaneWidth(shell, 260.0f);
  const Box2d sourceRect = RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/260.0f,
                                                         ImVec2(-100.0f, -100.0f),
                                                         /*mouseDown=*/false,
                                                         /*windowWidth=*/1000.0f);
  const ImVec2 sourceCenter(
      static_cast<float>((sourceRect.topLeft.x + sourceRect.bottomRight.x) * 0.5),
      static_cast<float>((sourceRect.topLeft.y + sourceRect.bottomRight.y) * 0.5));
  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/260.0f, sourceCenter,
                                /*mouseDown=*/true, /*windowWidth=*/1000.0f);
  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/260.0f,
                                ImVec2(sourceCenter.x + 80.0f, sourceCenter.y),
                                /*mouseDown=*/true, /*windowWidth=*/1000.0f);

  EXPECT_TRUE(EditorShellTestAccess::SourcePaneVisible(shell));
  EXPECT_GT(EditorShellTestAccess::SourcePaneWidth(shell), 260.0f);
}

TEST(EditorShellTest, FillStrokeToolbarMouseHitTestingCoversChipsSwatchesAndTooltips) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kPaintToolbarSvg, "paint_toolbar.svg"));
  ASSERT_TRUE(shell.valid());
  svg::SVGDocument& document = EditorShellTestAccess::App(shell).document().document();

  auto selectById = [&](std::string_view id) {
    std::optional<svg::SVGElement> element = document.querySelector("#" + std::string(id));
    ASSERT_TRUE(element.has_value()) << id;
    EditorShellTestAccess::App(shell).setSelection(*element);
  };

  constexpr ImVec2 kCursor(20.0f, 40.0f);
  constexpr ImVec2 kStrokeChip(70.0f, 47.0f);
  constexpr ImVec2 kFillChip(70.0f, 63.0f);
  constexpr ImVec2 kStrokeSwatchOnly(50.0f, 47.0f);
  constexpr ImVec2 kFillSwatchOnly(30.0f, 65.0f);
  constexpr ImVec2 kWidgetBackground(58.0f, 70.0f);

  selectById("local");
  RunFramesUntilDisplayedSelectionBounds(window, shell);
  EditorShellTestAccess::SetSourcePaneVisible(shell, false);
  ClickToolbar(window, shell, kCursor, kFillChip);
  ClickToolbar(window, shell, kCursor, kStrokeChip);
  RenderToolbarFrame(window, shell, kCursor, kStrokeChip, /*mouseDown=*/false);
  ClickToolbar(window, shell, kCursor, kFillSwatchOnly);
  ClickToolbar(window, shell, kCursor, kStrokeSwatchOnly);
  ClickToolbar(window, shell, kCursor, kWidgetBackground);

  for (std::string_view id : {"missing", "contextual", "styled-none-attribute", "external"}) {
    selectById(id);
    RenderToolbarFrame(window, shell, kCursor, kFillChip, /*mouseDown=*/false);
  }

  EXPECT_TRUE(shell.valid());
}

TEST(EditorShellTest, ToolPaletteSelectCommitsOpenPenPath) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  DriveGlobalShortcut(shell, {ImGuiKey_P});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(25.0, 16.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(25.0, 16.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  constexpr ImVec2 kPaneOrigin(20.0f, 30.0f);
  constexpr ImVec2 kContentRegion(400.0f, 260.0f);
  const Box2d palette =
      EditorShellTestAccess::ToolPaletteScreenRect(shell, kPaneOrigin, kContentRegion);
  const ImVec2 selectCenter(static_cast<float>(palette.topLeft.x + 20.0),
                            static_cast<float>(palette.topLeft.y + 20.0));

  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion,
                         ImVec2(selectCenter.x, selectCenter.y), /*mouseDown=*/false);
  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, selectCenter,
                         /*mouseDown=*/false);
  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, selectCenter,
                         /*mouseDown=*/true);
  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, selectCenter,
                         /*mouseDown=*/false);

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);
}

TEST(EditorShellTest, HighlightSelectionSourceHandlesTextOriginAndClearedSelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  EditorShellTestAccess::SetSourceSelectionOriginatedInText(shell, true);
  EditorShellTestAccess::App(shell).setSelection(*target);

  EXPECT_TRUE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));
  EXPECT_FALSE(EditorShellTestAccess::SourceSelectionOriginatedInText(shell));
  EXPECT_FALSE(EditorShellTestAccess::PreserveSourceEditFocusCursor(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).hasFocusPartition());

  EXPECT_FALSE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));

  EditorShellTestAccess::App(shell).clearSelection();
  EXPECT_TRUE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));
  EXPECT_FALSE(EditorShellTestAccess::Source(shell).hasFocusPartition());
}

TEST(EditorShellTest, HighlightSelectionSourceClearsStalePreserveFlag) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  ASSERT_TRUE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));

  EditorShellTestAccess::SetSourceFocusMode(shell, false);
  EditorShellTestAccess::SetPreserveSourceEditFocusCursor(shell, true);

  EXPECT_FALSE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));
  EXPECT_FALSE(EditorShellTestAccess::PreserveSourceEditFocusCursor(shell));
}

TEST(EditorShellTest, ReferenceHighlightActiveAndChipRectUseSelectionBoundsCache) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  RunFramesUntilDisplayedSelectionBounds(window, shell);
  ASSERT_GT(EditorShellTestAccess::DisplayedSelectionBoundsCount(shell), 0u);
  EditorShellTestAccess::RefreshReferenceHighlightSummaryIfNeeded(shell);

  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightElements(shell).empty());
  EditorShellTestAccess::SetReferenceHighlightActive(shell, true);
  EXPECT_EQ(EditorShellTestAccess::ReferenceHighlightElements(shell).size(), 2u);
  EXPECT_EQ(EditorShellTestAccess::CombinedSourcePreviewElements(shell).size(), 2u);

  const std::optional<Box2d> chipRect =
      EditorShellTestAccess::ReferenceHighlightChipScreenRect(shell, "refs 2");
  ASSERT_TRUE(chipRect.has_value());
  EXPECT_GT(chipRect->width(), 0.0);
  EXPECT_GT(chipRect->height(), 0.0);
  EXPECT_GE(chipRect->topLeft.x, shell.viewportForReadback().imageScreenRect().topLeft.x);
  EXPECT_GE(chipRect->topLeft.y, shell.viewportForReadback().imageScreenRect().topLeft.y);
}

TEST(EditorShellTest, ReferenceHighlightChipRendersHoverAndClickStates) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  RunFramesUntilDisplayedSelectionBounds(window, shell);
  ASSERT_GT(EditorShellTestAccess::DisplayedSelectionBoundsCount(shell), 0u);
  EditorShellTestAccess::RefreshReferenceHighlightSummaryIfNeeded(shell);

  const std::optional<Box2d> chipRect =
      EditorShellTestAccess::ReferenceHighlightChipScreenRect(shell, "-> 2");
  ASSERT_TRUE(chipRect.has_value());
  const ImVec2 chipCenter(
      static_cast<float>((chipRect->topLeft.x + chipRect->bottomRight.x) * 0.5),
      static_cast<float>((chipRect->topLeft.y + chipRect->bottomRight.y) * 0.5));

  RenderReferenceHighlightChipFrame(window, shell, chipCenter, /*mouseDown=*/false);
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightChipHovered(shell));
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightActive(shell));

  RenderReferenceHighlightChipFrame(window, shell, chipCenter, /*mouseDown=*/true);
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightActive(shell));
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightElements(shell).empty());

  RenderReferenceHighlightChipFrame(window, shell, ImVec2(-100.0f, -100.0f),
                                    /*mouseDown=*/false);
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightChipHovered(shell));
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightActive(shell));
}

TEST(EditorShellTest, SelectionChipBoundsUseActiveGesturePreviews) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  RunFramesUntilDisplayedSelectionBounds(window, shell);
  ASSERT_GT(EditorShellTestAccess::DisplayedSelectionBoundsCount(shell), 0u);

  const std::optional<Vector2d> baseAnchor =
      EditorShellTestAccess::SelectionChipAnchorScreen(shell);
  ASSERT_TRUE(baseAnchor.has_value());

  SelectTool::ActiveGesturePreview movePreview{
      .kind = SelectTool::ActiveGestureKind::Move,
      .startBoundsDoc = Box2d::FromXYWH(10.0, 12.0, 40.0, 24.0),
      .documentFromStartDocument = Transform2d::Translate(Vector2d(5.0, 7.0)),
  };
  const std::optional<Vector2d> moveAnchor =
      EditorShellTestAccess::SelectionChipAnchorScreen(shell, movePreview);
  const std::optional<Box2d> moveScreenBounds =
      EditorShellTestAccess::SelectionChipScreenBounds(shell, movePreview);
  ASSERT_TRUE(moveAnchor.has_value());
  ASSERT_TRUE(moveScreenBounds.has_value());
  EXPECT_NE(*moveAnchor, *baseAnchor);
  EXPECT_GT(moveScreenBounds->width(), 0.0);
  EXPECT_GT(moveScreenBounds->height(), 0.0);

  SelectTool::ActiveGesturePreview rotatePreview = movePreview;
  rotatePreview.kind = SelectTool::ActiveGestureKind::Rotate;
  rotatePreview.documentFromStartDocument = Transform2d::Rotate(0.25);
  const std::optional<Vector2d> rotateAnchor =
      EditorShellTestAccess::SelectionChipAnchorScreen(shell, rotatePreview);
  ASSERT_TRUE(rotateAnchor.has_value());
  EXPECT_NE(*rotateAnchor, *moveAnchor);

  const SelectionTransformHandleIntent resizeIntent{
      .kind = SelectionTransformHandleKind::Resize,
      .corner = SelectionTransformCorner::TopLeft,
  };
  RenderSelectionSizeChipFrame(window, shell, resizeIntent, std::nullopt);
  RenderSelectionSizeChipFrame(window, shell, SelectionTransformHandleIntent{}, movePreview);
  RenderSelectionSizeChipFrame(window, shell, SelectionTransformHandleIntent{}, rotatePreview);
  EXPECT_TRUE(shell.valid());
}

TEST(EditorShellTest, SourceHoverRangeGuardsRejectEmptyNoDocumentAndDirtySource) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  EXPECT_TRUE(EditorShellTestAccess::SourceHoverRangesForElements(shell, {}).empty());

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  EXPECT_TRUE(EditorShellTestAccess::SourceHoverRangesForElements(invalidShell, {*target}).empty());

  EditorShellTestAccess::Source(shell).insertText(" ");
  EXPECT_TRUE(EditorShellTestAccess::SourceHoverRangesForElements(shell, {*target}).empty());
}

TEST(EditorShellTest, SourcePaneVisibilityNoOpsWhenAlreadyMatching) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  ASSERT_TRUE(EditorShellTestAccess::SourcePaneVisible(shell));
  EditorShellTestAccess::SetSourcePaneVisible(shell, true);
  EXPECT_TRUE(EditorShellTestAccess::SourcePaneVisible(shell));

  EditorShellTestAccess::SetSourcePaneVisible(shell, false);
  EXPECT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));
  EditorShellTestAccess::SetSourcePaneVisible(shell, false);
  EXPECT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));
}

TEST(EditorShellTest, SelectAllCanvasAndTextSelectionPreconditions) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  EXPECT_TRUE(EditorShellTestAccess::CanvasHasSelectableElements(shell));
  EditorShellTestAccess::SelectAllCanvasElements(shell);
  EXPECT_GT(EditorShellTestAccess::App(shell).selectedElements().size(), 1u);
  EXPECT_FALSE(EditorShellTestAccess::SelectionIsAllText(shell));

  auto label = EditorShellTestAccess::App(shell).document().document().querySelector("#label");
  ASSERT_TRUE(label.has_value());
  EditorShellTestAccess::App(shell).setSelection(*label);
  EXPECT_TRUE(EditorShellTestAccess::SelectionIsAllText(shell));

  EditorShellTestAccess::App(shell).clearSelection();
  EXPECT_FALSE(EditorShellTestAccess::SelectionIsAllText(shell));

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  EXPECT_FALSE(EditorShellTestAccess::CanvasHasSelectableElements(invalidShell));
  EditorShellTestAccess::SelectAllCanvasElements(invalidShell);
  EXPECT_TRUE(EditorShellTestAccess::App(invalidShell).selectedElements().empty());
}

TEST(EditorShellTest, ConvertSelectedTextToOutlinesGuardsNonTextSelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  const std::string sourceBefore(EditorShellTestAccess::App(shell).document().document().source());

  EditorShellTestAccess::ConvertSelectedTextToOutlines(shell);
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);

  EXPECT_EQ(EditorShellTestAccess::App(shell).document().document().source(), sourceBefore);
  EXPECT_TRUE(EditorShellTestAccess::LastConvertTextError(shell).empty());
  EXPECT_TRUE(EditorShellTestAccess::App(shell).document().document().querySelector("#target"));
}

TEST(EditorShellTest, ConvertSelectedTextToOutlinesReplacesTextWithPathGroup) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  auto label = EditorShellTestAccess::App(shell).document().document().querySelector("#label");
  ASSERT_TRUE(label.has_value());
  EditorShellTestAccess::App(shell).setSelection(*label);

  EditorShellTestAccess::ConvertSelectedTextToOutlines(shell);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));

  svg::SVGDocument& document = EditorShellTestAccess::App(shell).document().document();
  EXPECT_FALSE(document.querySelector("#label").has_value());
  ASSERT_TRUE(document.querySelector("#label_outlines").has_value());
  EXPECT_TRUE(document.querySelector("#label_outlines_0").has_value());
  EXPECT_NE(document.source().find(R"(id="label_outlines")"), std::string_view::npos);
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElements().size(), 1u);
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElements().front().id(), "label_outlines");
  EXPECT_TRUE(EditorShellTestAccess::LastConvertTextError(shell).empty());
}

TEST(EditorShellTest, RenderPaneDeferredEmptyClickStartsMarqueeAfterHold) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(400.0, 300.0));
  EditorShellTestAccess::SetPendingSelectClickStartSeconds(shell, -1.0);

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.AddMousePosEvent(-50.0f, -50.0f);
  io.AddMouseButtonEvent(0, true);

  window.beginFrame();
  EditorShellTestAccess::RenderRenderPane(
      shell, Vector2d(0.0, 0.0), Vector2d(320.0, 220.0),
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
  window.endFrame();

  EXPECT_FALSE(EditorShellTestAccess::HasPendingClick(shell));
  EXPECT_TRUE(EditorShellTestAccess::SelectToolIsMarqueeing(shell));
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  io.AddMouseButtonEvent(0, false);
  window.beginFrame();
  window.endFrame();
}

TEST(EditorShellTest, RenderPaneTextToolClickCreatesTextAndReturnsToSelect) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  DriveGlobalShortcut(shell, {ImGuiKey_T});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));

  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(24.0, 34.0));
  RenderPaneMouseFrame(window, shell, Vector2d(24.0, 34.0), /*mouseDown=*/false);
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);

  const std::string_view source = EditorShellTestAccess::App(shell).document().document().source();
  EXPECT_NE(source.find(R"(x="24")"), std::string_view::npos);
  EXPECT_NE(source.find(R"(y="34")"), std::string_view::npos);
  EXPECT_NE(source.find(">Text</text>"), std::string_view::npos);
}

TEST(EditorShellTest, RenderPanePenToolClickDragAndCommitOpenPath) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  DriveGlobalShortcut(shell, {ImGuiKey_P});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(12.0, 18.0));
  RenderPaneMouseFrame(window, shell, Vector2d(18.0, 24.0), /*mouseDown=*/false);
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolActivePathData(shell).empty());

  MouseModifiers constrainedClick;
  constrainedClick.shift = true;
  constrainedClick.pixelsPerDocUnit = shell.viewportForReadback().pixelsPerDocUnit();
  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(42.0, 24.0), constrainedClick);
  RenderPaneMouseFrame(window, shell, Vector2d(42.0, 24.0), /*mouseDown=*/false);
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  DriveGlobalShortcut(shell, {ImGuiKey_Enter});
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);
}

TEST(EditorShellTest, RenderPanePenToolDragsAnchorAndCommitsOnDoubleClick) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  DriveGlobalShortcut(shell, {ImGuiKey_P});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(12.0, 18.0));
  RenderPaneMouseFrame(window, shell, Vector2d(12.0, 18.0), /*mouseDown=*/true);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));

  RenderPaneMouseFrame(window, shell, Vector2d(22.0, 28.0), /*mouseDown=*/true);
  EXPECT_TRUE(EditorShellTestAccess::PenDragFlushedThisFrame(shell));
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));

  RenderPaneMouseFrame(window, shell, Vector2d(22.0, 28.0), /*mouseDown=*/false);
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));

  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(42.0, 24.0));
  RenderPaneMouseFrame(window, shell, Vector2d(42.0, 24.0), /*mouseDown=*/false);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  RenderPaneMouseFrame(window, shell, Vector2d(42.0, 24.0), /*mouseDown=*/true,
                       Vector2d(20.0, 30.0), Vector2d(400.0, 260.0),
                       /*shift=*/false, /*option=*/false, /*command=*/false,
                       /*doubleClick=*/true);
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);
}

TEST(EditorShellTest, GlobalShortcutDeleteWhilePenDraftingRemovesAnchorBeforeCanvasDelete) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  DriveGlobalShortcut(shell, {ImGuiKey_P});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 18.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 18.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  DriveGlobalShortcut(shell, {ImGuiKey_Delete});

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_TRUE(EditorShellTestAccess::App(shell).hasDocument());
}

TEST(EditorShellTest, GlobalShortcutsSwitchToolsZoomAndSourceFocus) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_P});
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_T});
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_Escape});
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_P});
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_V});
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));

  const bool sourceFocusBefore = EditorShellTestAccess::SourceFocusMode(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_Enter}, /*ctrl=*/true);
  EXPECT_NE(EditorShellTestAccess::SourceFocusMode(shell), sourceFocusBefore);

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_Equal}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_Minus}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  DriveGlobalShortcut(shell, {ImGuiKey_Equal}, /*ctrl=*/true);
  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_0}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_KeypadAdd}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_KeypadSubtract}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  const bool sourceFocusBeforeKeypadEnter = EditorShellTestAccess::SourceFocusMode(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_KeypadEnter}, /*ctrl=*/true);
  EXPECT_NE(EditorShellTestAccess::SourceFocusMode(shell), sourceFocusBeforeKeypadEnter);
}

TEST(EditorShellTest, GlobalShortcutsRouteFileDialogsSaveAndQuit) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  const std::filesystem::path savePath = TempPathForTest("shortcut_save.svg");
  std::filesystem::remove(savePath);
  EditorShell shell(window, OptionsWithSource(kInitialSvg, savePath.string()));
  ASSERT_TRUE(shell.valid());

  DriveGlobalShortcut(shell, {ImGuiKey_O}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::OpenFileModalRequested(shell));

  DriveGlobalShortcut(shell, {ImGuiKey_S}, /*ctrl=*/true);
  ASSERT_TRUE(std::filesystem::exists(savePath));
  EXPECT_NE(ReadTextFile(savePath).find("target"), std::string::npos);

  DriveGlobalShortcut(shell, {ImGuiKey_S}, /*ctrl=*/true, /*shift=*/true);
  EXPECT_TRUE(EditorShellTestAccess::SaveFileModalRequested(shell));

  glfwSetWindowShouldClose(window.rawHandle(), GLFW_FALSE);
  DriveGlobalShortcut(shell, {ImGuiKey_Q}, /*ctrl=*/true);
  EXPECT_NE(glfwWindowShouldClose(window.rawHandle()), 0);
}

TEST(EditorShellTest, GlobalShortcutsRouteCanvasSelectionAndShapeClipboard) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::UseInMemoryShapeClipboard(shell);

  DriveGlobalShortcut(shell, {ImGuiKey_A}, /*ctrl=*/true);
  EXPECT_GT(EditorShellTestAccess::App(shell).selectedElements().size(), 1u);

  DriveGlobalShortcut(shell, {ImGuiKey_A}, /*ctrl=*/true, /*shift=*/true);
  EXPECT_FALSE(EditorShellTestAccess::App(shell).hasSelection());

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  DriveGlobalShortcut(shell, {ImGuiKey_C}, /*ctrl=*/true);
  ASSERT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EXPECT_NE(EditorShellTestAccess::ShapeClipboardText(shell).find("target"), std::string::npos);

  DriveGlobalShortcut(shell, {ImGuiKey_V}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("target_pasted"),
            std::string_view::npos);

  DriveGlobalShortcut(shell, {ImGuiKey_F}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("target_pasted2"),
            std::string_view::npos);

  auto pasted =
      EditorShellTestAccess::App(shell).document().document().querySelector("#target_pasted");
  ASSERT_TRUE(pasted.has_value());
  EditorShellTestAccess::App(shell).setSelection(*pasted);
  DriveGlobalShortcut(shell, {ImGuiKey_X}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(
      EditorShellTestAccess::App(shell).document().document().querySelector("#target_pasted"));
}

TEST(EditorShellTest, GlobalShortcutsDeleteUndoRedoAndReorderSelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  constexpr std::string_view kStackedSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <rect id="back" x="0" y="0" width="10" height="10"/>
  <rect id="middle" x="20" y="0" width="10" height="10"/>
  <rect id="front" x="40" y="0" width="10" height="10"/>
</svg>
)svg";
  EditorShell shell(window, OptionsWithSource(kStackedSvg, "stacked.svg"));
  ASSERT_TRUE(shell.valid());

  auto middle = EditorShellTestAccess::App(shell).document().document().querySelector("#middle");
  ASSERT_TRUE(middle.has_value());
  EditorShellTestAccess::App(shell).setSelection(*middle);

  const std::string beforeForward(EditorShellTestAccess::App(shell).document().document().source());
  DriveGlobalShortcut(shell, {ImGuiKey_RightBracket}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  const std::string afterForward(EditorShellTestAccess::App(shell).document().document().source());
  EXPECT_NE(afterForward, beforeForward);

  DriveGlobalShortcut(shell, {ImGuiKey_LeftBracket}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  const std::string afterBackward(EditorShellTestAccess::App(shell).document().document().source());
  EXPECT_NE(afterBackward, afterForward);

  DriveGlobalShortcut(shell, {ImGuiKey_RightBracket}, /*ctrl=*/true, /*shift=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_LeftBracket}, /*ctrl=*/true, /*shift=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));

  middle = EditorShellTestAccess::App(shell).document().document().querySelector("#middle");
  ASSERT_TRUE(middle.has_value());
  EditorShellTestAccess::App(shell).setSelection(*middle);
  DriveGlobalShortcut(shell, {ImGuiKey_Delete});
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).document().document().querySelector("#middle"));

  DriveGlobalShortcut(shell, {ImGuiKey_Z}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_TRUE(EditorShellTestAccess::App(shell).document().document().querySelector("#middle"));

  DriveGlobalShortcut(shell, {ImGuiKey_Z}, /*ctrl=*/true, /*shift=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).document().document().querySelector("#middle"));

  middle = EditorShellTestAccess::App(shell).document().document().querySelector("#front");
  ASSERT_TRUE(middle.has_value());
  EditorShellTestAccess::App(shell).setSelection(*middle);
  DriveGlobalShortcut(shell, {ImGuiKey_Backspace});
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).document().document().querySelector("#front"));
}

TEST(EditorShellTest, MenuActionsRouteCanvasClipboardHistorySelectionAndViewState) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  EditorShellTestAccess::UseInMemoryShapeClipboard(shell);

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  MenuBarActions actions;
  actions.copy = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  ASSERT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EXPECT_NE(EditorShellTestAccess::ShapeClipboardText(shell).find("target"), std::string::npos);

  actions = MenuBarActions{};
  actions.cut = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).document().document().querySelector("#target"));

  actions = MenuBarActions{};
  actions.undo = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_TRUE(EditorShellTestAccess::App(shell).document().document().querySelector("#target"));

  actions = MenuBarActions{};
  actions.redo = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).document().document().querySelector("#target"));

  actions = MenuBarActions{};
  actions.revertFile = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::App(shell).document().document().querySelector("#target"));

  actions = MenuBarActions{};
  actions.paste = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_TRUE(
      EditorShellTestAccess::App(shell).document().document().querySelector("#target_pasted"));

  actions = MenuBarActions{};
  actions.pasteInFront = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_TRUE(
      EditorShellTestAccess::App(shell).document().document().querySelector("#target_pasted2"));

  actions = MenuBarActions{};
  actions.selectAllCanvas = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_GT(EditorShellTestAccess::App(shell).selectedElements().size(), 1u);

  actions = MenuBarActions{};
  actions.deselectAllCanvas = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_FALSE(EditorShellTestAccess::App(shell).hasSelection());

  actions = MenuBarActions{};
  actions.selectAll = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_EQ(EditorShellTestAccess::Source(shell).getSelectedText(),
            EditorShellTestAccess::Source(shell).getText());

  actions = MenuBarActions{};
  actions.deselectAll = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).getSelectedText().empty());

  auto label = EditorShellTestAccess::App(shell).document().document().querySelector("#label");
  ASSERT_TRUE(label.has_value());
  EditorShellTestAccess::App(shell).setSelection(*label);
  actions = MenuBarActions{};
  actions.convertTextToOutlines = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_TRUE(
      EditorShellTestAccess::App(shell).document().document().querySelector("#label_outlines"));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  const bool sourceFocusBefore = EditorShellTestAccess::SourceFocusMode(shell);
  const bool compositorDebugBefore = EditorShellTestAccess::ShowCompositorDebugPanel(shell);
  const bool perfOverlayBefore = EditorShellTestAccess::ShowPerfOverlay(shell);

  actions = MenuBarActions{};
  actions.zoomIn = true;
  actions.zoomOut = true;
  actions.actualSize = true;
  actions.toggleSourceFocusMode = true;
  actions.toggleCompositorDebugPanel = true;
  actions.togglePerfOverlay = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);

  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));
  EXPECT_NE(EditorShellTestAccess::SourceFocusMode(shell), sourceFocusBefore);
  EXPECT_NE(EditorShellTestAccess::ShowCompositorDebugPanel(shell), compositorDebugBefore);
  EXPECT_NE(EditorShellTestAccess::ShowPerfOverlay(shell), perfOverlayBefore);
}

TEST(EditorShellTest, MenuActionsRouteDialogAndExportRequestsWithoutCurrentPath) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShellOptions options;
  options.initialSource = std::string(kInitialSvg);
  EditorShell shell(window, std::move(options));
  ASSERT_TRUE(shell.valid());

  MenuBarActions actions;
  actions.openAbout = true;
  actions.openFile = true;
  actions.saveFile = true;
  actions.saveFileAs = true;
  actions.exportViewportSvg = true;
  actions.exportViewportSvgWithOverlay = true;
  actions.quit = true;
  actions.undo = true;
  actions.redo = true;

  EditorShellTestAccess::ApplyMenuActions(shell, actions);

  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExportOverlay(shell));
  EXPECT_TRUE(shell.valid());
}

TEST(EditorShellTest, ShapeClipboardCopyCutAndPasteUseCanvasSelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::UseInMemoryShapeClipboard(shell);

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  EditorShellTestAccess::CopySelectedShapesToClipboard(shell);
  EXPECT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EXPECT_NE(EditorShellTestAccess::ShapeClipboardText(shell).find("target"), std::string::npos);

  EditorShellTestAccess::PasteShapesFromClipboard(shell, /*inFront=*/false);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("target_pasted"),
            std::string_view::npos);

  auto pasted =
      EditorShellTestAccess::App(shell).document().document().querySelector("#target_pasted");
  ASSERT_TRUE(pasted.has_value());
  EditorShellTestAccess::App(shell).setSelection(*pasted);
  EditorShellTestAccess::CutSelectedShapesToClipboard(shell);
  EXPECT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_EQ(EditorShellTestAccess::App(shell).document().document().source().find("target_pasted"),
            std::string_view::npos);

  EditorShellTestAccess::ClearShapeClipboard(shell);
  const std::string_view sourceBeforeNullClipboardPaste =
      EditorShellTestAccess::App(shell).document().document().source();
  EditorShellTestAccess::CopySelectedShapesToClipboard(shell);
  EXPECT_FALSE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EditorShellTestAccess::PasteShapesFromClipboard(shell, /*inFront=*/true);
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);
  EXPECT_EQ(EditorShellTestAccess::App(shell).document().document().source(),
            sourceBeforeNullClipboardPaste);
}

TEST(EditorShellTest, ShapeClipboardRejectsMalformedAndPastesIntoSelectedGroup) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  constexpr std::string_view kGroupedSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <g id="group"><circle id="inside" cx="12" cy="12" r="4"/></g>
  <rect id="target" x="10" y="12" width="40" height="24" fill="#3366cc"/>
</svg>
)svg";
  EditorShell shell(window, OptionsWithSource(kGroupedSvg, "grouped.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::UseInMemoryShapeClipboard(shell);

  EditorShellTestAccess::SetShapeClipboardText(shell, "<rect");
  const std::string_view sourceBeforeMalformedPaste =
      EditorShellTestAccess::App(shell).document().document().source();
  EditorShellTestAccess::PasteShapesFromClipboard(shell, /*inFront=*/false);
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);
  EXPECT_EQ(EditorShellTestAccess::App(shell).document().document().source(),
            sourceBeforeMalformedPaste);

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  auto group = EditorShellTestAccess::App(shell).document().document().querySelector("#group");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(group.has_value());

  EditorShellTestAccess::App(shell).setSelection(*target);
  EditorShellTestAccess::CopySelectedShapesToClipboard(shell);
  ASSERT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));

  EditorShellTestAccess::App(shell).setSelection(*group);
  EditorShellTestAccess::PasteShapesFromClipboard(shell, /*inFront=*/false);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  const std::string_view source = EditorShellTestAccess::App(shell).document().document().source();
  const std::size_t groupOffset = source.find(R"(id="group")");
  const std::size_t pastedOffset = source.find(R"(id="target_pasted")");
  const std::size_t groupCloseOffset = source.find("</g>");
  ASSERT_NE(groupOffset, std::string_view::npos);
  ASSERT_NE(pastedOffset, std::string_view::npos);
  ASSERT_NE(groupCloseOffset, std::string_view::npos);
  EXPECT_GT(pastedOffset, groupOffset);
  EXPECT_LT(pastedOffset, groupCloseOffset);
}

}  // namespace donner::editor
