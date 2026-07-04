#include "donner/editor/EditorShell.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "donner/editor/gui/EditorWindow.h"

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
    shell.interactionController_.resetToActualSize();
  }

  static void RefreshSelectionBoundsCache(EditorShell& shell) {
    shell.renderCoordinator_.refreshSelectionBoundsCache(shell.app_);
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

  static void ApplyStyleFocus(EditorShell& shell, StyleFocus styleFocus) {
    shell.applyStyleFocus(std::move(styleFocus));
  }

  static void ApplySourcePartition(EditorShell& shell, FocusPartition partition) {
    shell.applySourcePartition(std::move(partition));
  }

  static void OpenRenderPaneContextMenu(EditorShell& shell, const Vector2d& documentPoint) {
    shell.openRenderPaneContextMenu(documentPoint);
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

  static bool SourcePaneVisible(const EditorShell& shell) { return shell.sourcePaneVisible_; }
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

  const std::filesystem::path savePath = TempPathForTest("saved.svg");
  error.clear();
  EXPECT_TRUE(EditorShellTestAccess::TrySavePath(shell, savePath.string(), &error)) << error;
  EXPECT_NE(ReadTextFile(savePath).find("opened"), std::string::npos);
  EXPECT_FALSE(EditorShellTestAccess::App(shell).isDirty());

  EditorShellTestAccess::Source(shell).setText("<svg>dirty</svg>");
  EditorShellTestAccess::App(shell).markDirty();
  EditorShellTestAccess::RequestRevert(shell);

  EXPECT_FALSE(EditorShellTestAccess::App(shell).isDirty());
  EXPECT_NE(EditorShellTestAccess::Source(shell).getText().find("opened"), std::string::npos);
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
  unsetenv("DONNER_EDITOR_RESOURCE_LOG");

  EditorShellTestAccess::NoteFrameDelta(shell, 25.0f);
  const std::filesystem::path telemetryPath = TempPathForTest("frame_miss.jsonl");
  std::filesystem::remove(telemetryPath);
  setenv("DONNER_EDITOR_FRAME_MISS_LOG", "false", /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_FALSE(std::filesystem::exists(telemetryPath));

  setenv("DONNER_EDITOR_FRAME_MISS_LOG", telemetryPath.string().c_str(), /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_NE(ReadTextFile(telemetryPath).find("frame_budget_miss"), std::string::npos);

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

}  // namespace donner::editor
