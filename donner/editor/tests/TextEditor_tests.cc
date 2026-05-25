#include "donner/editor/TextEditor.h"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/ImGuiInternalIncludes.h"

namespace donner::editor {

/// Fixture for `TextEditor` tests. Creates a per-test ImGui context so
/// calls that route through ImGui's clipboard (`copy()`, `cut()`,
/// `paste()`, `GetClipboardText()`) don't crash. The context is
/// destroyed in TearDown so each test starts fresh.
class TextEditorTests : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    imguiContext_ = ImGui::CreateContext();
    // ImGui needs a valid font atlas even for non-rendering operations
    // (clipboard, input processing). Build a default atlas so functions
    // that query glyph metrics don't crash.
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.Fonts->Build();
  }

  void TearDown() override {
    if (imguiContext_ != nullptr) {
      ImGui::DestroyContext(imguiContext_);
      imguiContext_ = nullptr;
    }
  }

  void RenderEditorFrame(const ImVec2& editorSize = ImVec2(240.0f, 180.0f)) {
    editor.setHandleKeyboardInputs(false);
    editor.setHandleMouseInputs(false);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(editorSize.x + 40.0f, editorSize.y + 40.0f), ImGuiCond_Always);
    ImGui::Begin(
        "TextEditorTestWindow", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    editor.render("##editor", editorSize);
    ImGui::End();
    ImGui::Render();
  }

  void RenderEditorFrameWithMouse(const ImVec2& mousePos, bool mouseDown,
                                  const ImVec2& editorSize = ImVec2(240.0f, 180.0f)) {
    editor.setHandleKeyboardInputs(false);
    editor.setHandleMouseInputs(true);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.AddMousePosEvent(mousePos.x, mousePos.y);
    io.AddMouseButtonEvent(0, mouseDown);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(editorSize.x + 40.0f, editorSize.y + 40.0f), ImGuiCond_Always);
    ImGui::Begin(
        "TextEditorTestWindow", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    editor.render("##editor", editorSize);
    ImGui::End();
    ImGui::Render();
  }

  [[nodiscard]] int VisualLineCount() const { return static_cast<int>(editor.visualLines_.size()); }

  [[nodiscard]] int VisualLineStartColumn(int visualIndex) const {
    return editor.visualLines_[visualIndex].startColumn;
  }

  [[nodiscard]] int VisualLineEndColumn(int visualIndex) const {
    return editor.visualLines_[visualIndex].endColumn;
  }

  [[nodiscard]] int VisualLineIndentColumns(int visualIndex) const {
    return editor.visualLines_[visualIndex].indentColumns;
  }

  [[nodiscard]] bool VisualLineIsContinuation(int visualIndex) const {
    return editor.visualLines_[visualIndex].continuation;
  }

  [[nodiscard]] bool VisualLineIsFocusHiddenPlaceholder(int visualIndex) const {
    return editor.visualLines_[visualIndex].focusHiddenPlaceholder;
  }

  [[nodiscard]] LineRange VisualLineHiddenRange(int visualIndex) const {
    return editor.visualLines_[visualIndex].hiddenRange;
  }

  [[nodiscard]] bool HorizontalScrollEnabled() const { return editor.horizontalScroll_; }

  void RequestSourceFocusModeContextMenuToggle() {
    editor.sourceFocusModeContextMenuToggleRequested_ = true;
  }

  [[nodiscard]] bool SourceFocusModeContextMenuVisible() const {
    return editor.sourceFocusModeContextMenuVisible_;
  }

  [[nodiscard]] bool SourceFocusModeContextMenuChecked() const {
    return editor.sourceFocusModeContextMenuChecked_;
  }

  [[nodiscard]] std::vector<int> VisualLineLogicalLines() const {
    std::vector<int> lines;
    lines.reserve(editor.visualLines_.size());
    for (const auto& visualLine : editor.visualLines_) {
      lines.push_back(visualLine.lineNo);
    }
    return lines;
  }

  [[nodiscard]] int FirstContinuationVisualLineForLine(int lineNo) const {
    for (int i = 0; i < static_cast<int>(editor.visualLines_.size()); ++i) {
      const auto& visualLine = editor.visualLines_[i];
      if (visualLine.lineNo == lineNo && visualLine.continuation) {
        return i;
      }
    }
    return -1;
  }

  [[nodiscard]] int VisualLineIndexForCoordinates(const Coordinates& position) const {
    return editor.visualLineIndexForCoordinates(position);
  }

  [[nodiscard]] float LastScrollY() const { return editor.lastScroll_; }
  [[nodiscard]] float LastScrollViewportHeight() const { return editor.scrollViewportHeight_; }
  [[nodiscard]] float CharacterAdvanceY() const { return editor.charAdvance_.y; }
  void EnterCharacter(ImWchar character) { editor.enterCharacter(character, /*shift=*/false); }

  void OpenAutocompleteAtCursor(std::string_view displayText) {
    editor.autocompleteOpened_ = true;
    editor.autocompleteSuggestions_.clear();
    editor.autocompleteSuggestions_.emplace_back(RcString(displayText), RcString(displayText));
    editor.autocompleteIndex_ = 0;
    editor.autocompletePosition_ = editor.getCursorPosition();
  }

  void ReplaceAutocompleteSuggestion(std::string_view displayText) {
    ASSERT_FALSE(editor.autocompleteSuggestions_.empty());
    editor.autocompleteSuggestions_[0] = {RcString(displayText), RcString(displayText)};
  }

  [[nodiscard]] int AutocompleteChildWindowCount() const {
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
      return 0;
    }

    int childWindowCount = 0;
    for (ImGuiWindow* window : context->Windows) {
      const std::string_view name(window->Name);
      if ((name.find("Autocomplete") != std::string_view::npos ||
           name.find("autocompl") != std::string_view::npos) &&
          (window->Flags & ImGuiWindowFlags_ChildWindow) != 0) {
        ++childWindowCount;
      }
    }
    return childWindowCount;
  }

  [[nodiscard]] int AutocompleteTopLevelWindowCount() const {
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
      return 0;
    }

    int topLevelWindowCount = 0;
    for (ImGuiWindow* window : context->Windows) {
      const std::string_view name(window->Name);
      if ((name.find("Autocomplete") != std::string_view::npos ||
           name.find("autocompl") != std::string_view::npos) &&
          (window->Flags & ImGuiWindowFlags_ChildWindow) == 0) {
        ++topLevelWindowCount;
      }
    }
    return topLevelWindowCount;
  }

  [[nodiscard]] Coordinates CoordinatesAtVisualTextOffset(int visualIndex,
                                                          int visualColumnOffset) const {
    const auto& visualLine = editor.visualLines_[visualIndex];
    const ImVec2 screenPos{
        editor.uiCursorPos_.x + editor.textStart_ +
            static_cast<float>(visualLine.indentColumns + visualColumnOffset) *
                editor.charAdvance_.x,
        editor.uiCursorPos_.y + static_cast<float>(visualIndex) * editor.charAdvance_.y +
            editor.charAdvance_.y * 0.5f,
    };
    return editor.screenPosToCoordinates(screenPos);
  }

  [[nodiscard]] ImVec2 ScreenPointAtVisualTextOffset(int visualIndex,
                                                     int visualColumnOffset) const {
    const auto& visualLine = editor.visualLines_[visualIndex];
    return ImVec2{
        editor.uiCursorPos_.x + editor.textStart_ +
            static_cast<float>(visualLine.indentColumns + visualColumnOffset) *
                editor.charAdvance_.x,
        editor.uiCursorPos_.y + static_cast<float>(visualIndex) * editor.charAdvance_.y +
            editor.charAdvance_.y * 0.5f,
    };
  }

  [[nodiscard]] bool HasActiveSourceFlash() const {
    return editor.nextFlashWakeSeconds().has_value() &&
           !editor.flashDecorations_.activeBackgrounds(FlashDecorations::Clock::now()).empty();
  }

  [[nodiscard]] std::optional<TextEditor::FocusReferenceConnectorLayout> FocusReferenceLayout(
      const FocusReferenceLink& link, int linkIndex) const {
    return editor.focusReferenceConnectorLayout(link, linkIndex);
  }

  [[nodiscard]] ImVec2 ScreenPointAtCoordinates(const Coordinates& position) const {
    return editor.coordinatesToScreenPos(position);
  }

  [[nodiscard]] float TextBaselineOffsetY() const {
    const ImFont* font = ImGui::GetFont();
    if (font == nullptr || font->FontSize <= 0.0f) {
      return ImGui::GetTextLineHeight();
    }

    return font->Ascent * (ImGui::GetFontSize() / font->FontSize);
  }

  [[nodiscard]] std::vector<ActiveFlash> ActiveSourceFlashes() const {
    return editor.flashDecorations_.activeBackgrounds(FlashDecorations::Clock::now());
  }

  [[nodiscard]] bool IsByteOffsetInIneffectiveStyleDecoration(std::size_t byteOffset) const {
    return editor.isByteOffsetInIneffectiveStyleDecoration(byteOffset);
  }

  [[nodiscard]] std::size_t SourceStyleChipHitRectCount() const {
    return editor.sourceStyleChipHitRects_.size();
  }

  [[nodiscard]] ImVec2 SourceStyleChipHitRectCenter(std::size_t index) const {
    const auto& hitRect = editor.sourceStyleChipHitRects_[index];
    return ImVec2((hitRect.min.x + hitRect.max.x) * 0.5f, (hitRect.min.y + hitRect.max.y) * 0.5f);
  }

  [[nodiscard]] ImVec2 SourceStyleChipHitRectMin(std::size_t index) const {
    return editor.sourceStyleChipHitRects_[index].min;
  }

  [[nodiscard]] ImVec2 SourceStyleChipHitRectMax(std::size_t index) const {
    return editor.sourceStyleChipHitRects_[index].max;
  }

  [[nodiscard]] std::string SourceStyleChipHitRectTooltip(std::size_t index) const {
    return editor.sourceStyleChipHitRects_[index].tooltip;
  }

  ImGuiContext* imguiContext_ = nullptr;
  TextEditor editor;
};

// ============================================================================
// CURSOR MOVEMENT TESTS
// ============================================================================

TEST_F(TextEditorTests, CursorStartsAtOrigin) {
  editor.setText("Hello");
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0)) << "Cursor should start at (0, 0)";
}

TEST_F(TextEditorTests, MoveRightAdvancesCursor) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 0));
  editor.moveRight(1, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 1))
      << "moveRight should advance cursor by 1 column";
}

TEST_F(TextEditorTests, KeyboardNavigationMarksCursorPositionChanged) {
  editor.setText("Hello\nWorld");
  RenderEditorFrame();

  editor.moveDown(1, false);
  EXPECT_TRUE(editor.isCursorPositionChanged());
}

TEST_F(TextEditorTests, MouseClickMarksCursorPositionChangedByMouse) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 0));
  RenderEditorFrame();

  const ImVec2 clickPos =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/6);

  RenderEditorFrameWithMouse(clickPos, false);
  RenderEditorFrameWithMouse(clickPos, true);

  EXPECT_TRUE(editor.isCursorPositionChanged());
  EXPECT_TRUE(editor.didMouseChangeCursorPosition());
  EXPECT_NE(editor.getCursorPosition(), Coordinates(0, 0));
}

TEST_F(TextEditorTests, HoveredTextPositionTracksMouseWithoutMovingCursor) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 0));
  RenderEditorFrame();

  const ImVec2 hoverPos =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/6);
  RenderEditorFrameWithMouse(hoverPos, false);

  ASSERT_TRUE(editor.hoveredTextPosition().has_value());
  EXPECT_EQ(*editor.hoveredTextPosition(), Coordinates(0, 6));
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0));
  EXPECT_FALSE(editor.isCursorPositionChanged());
  EXPECT_FALSE(editor.didMouseChangeCursorPosition());
}

TEST_F(TextEditorTests, HoverSourceRangesAreClampedAndDeduplicated) {
  editor.setText("abcdef");

  EXPECT_TRUE(editor.setHoverSourceRanges({
      SourceByteRange{.start = 2, .end = 5},
      SourceByteRange{.start = 2, .end = 5},
      SourceByteRange{.start = 5, .end = 100},
      SourceByteRange{.start = 4, .end = 4},
  }));

  ASSERT_EQ(editor.hoverSourceRanges().size(), 2u);
  EXPECT_EQ(editor.hoverSourceRanges()[0], (SourceByteRange{.start = 2, .end = 5}));
  EXPECT_EQ(editor.hoverSourceRanges()[1], (SourceByteRange{.start = 5, .end = 6}));

  EXPECT_FALSE(editor.setHoverSourceRanges(editor.hoverSourceRanges()));
  EXPECT_TRUE(editor.clearHoverSourceRanges());
  EXPECT_TRUE(editor.hoverSourceRanges().empty());
}

TEST_F(TextEditorTests, SourceStyleDecorationsAreClampedAndCleared) {
  editor.setText("abcdef");

  EXPECT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 7,
          .range = SourceByteRange{.start = 4, .end = 4},
          .ineffective = true,
      },
      TextEditor::SourceStyleDecoration{
          .id = 2,
          .range = SourceByteRange{.start = 5, .end = 100},
          .showChip = true,
          .chipCount = -4,
          .tooltip = "unused selector",
      },
      TextEditor::SourceStyleDecoration{
          .id = 1,
          .range = SourceByteRange{.start = 2, .end = 5},
          .ineffective = true,
          .tooltip = "fill is overridden",
      },
  }));

  ASSERT_EQ(editor.sourceStyleDecorations().size(), 2u);
  EXPECT_EQ(editor.sourceStyleDecorations()[0].id, 1u);
  EXPECT_EQ(editor.sourceStyleDecorations()[0].range, (SourceByteRange{.start = 2, .end = 5}));
  EXPECT_TRUE(editor.sourceStyleDecorations()[0].ineffective);
  EXPECT_EQ(editor.sourceStyleDecorations()[1].id, 2u);
  EXPECT_EQ(editor.sourceStyleDecorations()[1].range, (SourceByteRange{.start = 5, .end = 6}));
  EXPECT_EQ(editor.sourceStyleDecorations()[1].chipCount, 0);

  EXPECT_FALSE(editor.setSourceStyleDecorations(editor.sourceStyleDecorations()));
  EXPECT_TRUE(editor.clearSourceStyleDecorations());
  EXPECT_TRUE(editor.sourceStyleDecorations().empty());
}

TEST_F(TextEditorTests, SourceStyleDecorationsStrikeRangesWithoutRenderingHiddenChips) {
  editor.setText("fill: red;");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 11,
          .range = SourceByteRange{.start = 0, .end = 4},
          .ineffective = true,
          .showChip = false,
          .chipCount = 5,
          .tooltip = "fill is overridden",
      },
  }));

  EXPECT_TRUE(IsByteOffsetInIneffectiveStyleDecoration(0));
  EXPECT_TRUE(IsByteOffsetInIneffectiveStyleDecoration(3));
  EXPECT_FALSE(IsByteOffsetInIneffectiveStyleDecoration(4));

  RenderEditorFrame(ImVec2(360.0f, 120.0f));
  EXPECT_EQ(SourceStyleChipHitRectCount(), 0u);
}

TEST_F(TextEditorTests, SourceStyleDecorationChipClickIsConsumable) {
  editor.setText(".cls { fill: red; }\n");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 42,
          .range = SourceByteRange{.start = 7, .end = 16},
          .showChip = true,
          .chipCount = 3,
          .tooltip = "3 matched elements",
      },
  }));

  RenderEditorFrame(ImVec2(420.0f, 120.0f));
  ASSERT_EQ(SourceStyleChipHitRectCount(), 1u);
  const ImVec2 chipCenter = SourceStyleChipHitRectCenter(0);

  RenderEditorFrameWithMouse(chipCenter, true, ImVec2(420.0f, 120.0f));

  EXPECT_EQ(editor.takeClickedSourceStyleChipId(), std::optional<std::size_t>(42));
  EXPECT_EQ(editor.takeClickedSourceStyleChipId(), std::nullopt);
}

TEST_F(TextEditorTests, SourceStyleDecorationChipUsesChipRangeAnchor) {
  editor.setText(".hit {\n  fill: red;\n}\n");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 84,
          .range = SourceByteRange{.start = 9, .end = 19},
          .chipRange = SourceByteRange{.start = 0, .end = 4},
          .showChip = true,
          .chipCount = 2,
          .tooltip = "fill is overridden",
          .chipTooltip = "Selector matches 2 elements",
      },
  }));

  RenderEditorFrame(ImVec2(420.0f, 120.0f));
  ASSERT_EQ(SourceStyleChipHitRectCount(), 1u);

  const float selectorLineCenterY =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/4).y;
  const float propertyLineCenterY =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/1, /*visualColumnOffset=*/12).y;
  const float chipCenterY = SourceStyleChipHitRectCenter(0).y;
  EXPECT_LT(std::abs(chipCenterY - selectorLineCenterY),
            std::abs(chipCenterY - propertyLineCenterY));
  EXPECT_EQ(SourceStyleChipHitRectTooltip(0), "Selector matches 2 elements");
}

TEST_F(TextEditorTests, SourceStyleDecorationOverflowMarkerHasTooltipAndIsNotClickable) {
  editor.setText("<linearGradient id=\"paint\">\n");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 85,
          .range = SourceByteRange{.start = 0, .end = 27},
          .chipRange = SourceByteRange{.start = 0, .end = 27},
          .showChip = true,
          .chipCount = 6,
          .showOverflowMarker = true,
          .chipTooltip = "Referenced 6 times",
          .overflowTooltip = "Too many reverse refs to draw lines",
      },
  }));

  RenderEditorFrame(ImVec2(520.0f, 120.0f));
  ASSERT_EQ(SourceStyleChipHitRectCount(), 2u);
  EXPECT_LT(SourceStyleChipHitRectCenter(0).x, SourceStyleChipHitRectCenter(1).x);
  EXPECT_EQ(SourceStyleChipHitRectTooltip(0), "Referenced 6 times");
  EXPECT_EQ(SourceStyleChipHitRectTooltip(1), "Too many reverse refs to draw lines");

  RenderEditorFrameWithMouse(SourceStyleChipHitRectCenter(1), true, ImVec2(520.0f, 120.0f));
  EXPECT_EQ(editor.takeClickedSourceStyleChipId(), std::nullopt);
}

TEST_F(TextEditorTests, MoveLeftRetractsCursor) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.moveLeft(1, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 1))
      << "moveLeft should move cursor back by 1 column";
}

TEST_F(TextEditorTests, MoveRightAtLineEndWrapsToNextLine) {
  editor.setText("Hello\nWorld");
  editor.setCursorPosition(Coordinates(0, 5));  // End of "Hello"
  editor.moveRight(1, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 0))
      << "moveRight at line end should wrap to start of next line";
}

TEST_F(TextEditorTests, MoveLeftAtLineStartWrapsToEndOfPreviousLine) {
  editor.setText("Hello\nWorld");
  editor.setCursorPosition(Coordinates(1, 0));  // Start of "World"
  editor.moveLeft(1, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 5))
      << "moveLeft at line start should wrap to end of previous line";
}

TEST_F(TextEditorTests, MoveUpAdvancesLineUp) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(2, 2));
  editor.moveUp(1, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 2))
      << "moveUp should move cursor up by 1 line";
}

TEST_F(TextEditorTests, MoveDownAdvancesLineDown) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.moveDown(1, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 2))
      << "moveDown should move cursor down by 1 line";
}

TEST_F(TextEditorTests, MoveUpOnFirstLineStaysAtFirstLine) {
  editor.setText("Line1\nLine2");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.moveUp(1, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 2))
      << "moveUp on first line should not change position";
}

TEST_F(TextEditorTests, MoveDownOnLastLineStaysAtLastLine) {
  editor.setText("Line1\nLine2");
  editor.setCursorPosition(Coordinates(1, 2));
  editor.moveDown(1, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 2))
      << "moveDown on last line should not change position";
}

TEST_F(TextEditorTests, MoveHomeSetsColumnToZero) {
  editor.setText("Line1");
  editor.setCursorPosition(Coordinates(0, 3));
  editor.moveHome(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0))
      << "moveHome should move cursor to column 0 of current line";
}

TEST_F(TextEditorTests, MoveEndSetsColumnToLineEnd) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.moveEnd(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 5))
      << "moveEnd should move cursor to end of current line";
}

TEST_F(TextEditorTests, MoveTopJumpsToDocumentStart) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(2, 3));
  editor.moveTop(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0))
      << "moveTop should move cursor to (0, 0)";
}

TEST_F(TextEditorTests, MoveBottomJumpsToDocumentEnd) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(0, 0));
  editor.moveBottom(false);
  // moveBottom moves to (lastLine, 0), not end of last line
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(2, 0))
      << "moveBottom should move cursor to (lastLine, 0)";
}

// ============================================================================
// SELECTION TESTS
// ============================================================================

TEST_F(TextEditorTests, SelectAllSelectsEntireBuffer) {
  editor.setText("Hello world");
  editor.selectAll();
  EXPECT_EQ(editor.getSelectedText(), "Hello world")
      << "selectAll should select entire buffer content";
}

TEST_F(TextEditorTests, ShiftRightExpandsSelection) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 0));
  editor.moveRight(1, true, false);  // select=true
  EXPECT_TRUE(editor.hasSelection()) << "Should have selection after Shift+Right";
  EXPECT_EQ(editor.getSelectedText(), "H") << "Selection should contain single character 'H'";
}

TEST_F(TextEditorTests, ShiftLeftContractsSelection) {
  editor.setText("Hello");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 3));
  // `setSelection` doesn't move the cursor, so put it explicitly at
  // the selection's end before contracting — `moveLeft(_, select)`
  // grows/shrinks relative to the cursor position, not the
  // selection bounds.
  editor.setCursorPosition(Coordinates(0, 3));
  editor.moveLeft(1, true, false);
  // After contracting left, selection should be from (0,0) to (0,2)
  EXPECT_EQ(editor.getSelectedText(), "He") << "Selection should be contracted to 'He'";
}

TEST_F(TextEditorTests, SelectionStartAndEnd) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  EXPECT_EQ(editor.getSelectedText(), "Hello") << "setSelection should select specified range";
}

TEST_F(TextEditorTests, HasSelectionReturnsFalseWhenNoSelection) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 0));
  EXPECT_FALSE(editor.hasSelection())
      << "hasSelection should return false after setCursorPosition clears selection";
}

TEST_F(TextEditorTests, SetSelectionStartPreservesEnd) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.setSelectionStart(Coordinates(0, 2));
  // Selection should now span from (0, 2) to (0, 5)
  EXPECT_EQ(editor.getSelectedText(), "llo")
      << "setSelectionStart should adjust start while preserving end";
}

TEST_F(TextEditorTests, SetSelectionEndPreservesStart) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.setSelectionEnd(Coordinates(0, 8));
  // Selection should now span from (0, 0) to (0, 8)
  EXPECT_EQ(editor.getSelectedText(), "Hello wo")
      << "setSelectionEnd should adjust end while preserving start";
}

// ============================================================================
// INSERTION & DELETION TESTS
// ============================================================================

TEST_F(TextEditorTests, InsertTextAddsCharacters) {
  editor.setText("");
  editor.insertText("Hello");
  EXPECT_EQ(editor.getText(), "Hello") << "insertText should add text to buffer";
}

TEST_F(TextEditorTests, DeleteDeletesCharacterAtCursor) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.delete_();
  EXPECT_EQ(editor.getText(), "Hllo") << "delete should remove character at cursor";
}

TEST_F(TextEditorTests, DeleteAtLineEndMergesWithNextLine) {
  editor.setText("Hello\nWorld");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.delete_();
  EXPECT_EQ(editor.getText(), "HelloWorld") << "delete at line end should merge with next line";
}

TEST_F(TextEditorTests, DeleteOnLastCharacterOfLastLineDoesNothing) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.delete_();
  EXPECT_EQ(editor.getText(), "Hello") << "delete on last character should not delete";
}

TEST_F(TextEditorTests, InsertTextWithSelection) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.insertText("Hi");
  EXPECT_EQ(editor.getText(), "Hi world") << "insertText with selection should replace selection";
}

// ============================================================================
// UNDO/REDO TESTS
//
// IMPORTANT: `editor.insertText()` is a raw-primitive that does NOT record
// undo entries — only the user-facing path (`enterCharacter`, `backspace`,
// `delete_`, `cut`, `paste`) records undo. These tests exercise the real
// user-facing path. If a future refactor makes `insertText` participate in
// the undo system, additional tests for that direct-API case should be
// added separately.
// ============================================================================

TEST_F(TextEditorTests, UndoReversesInsertion) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.enterCharacter('!', /*shift=*/false);
  EXPECT_EQ(editor.getText(), "Hello!");
  editor.undo();
  EXPECT_EQ(editor.getText(), "Hello") << "undo should revert insertion";
}

TEST_F(TextEditorTests, UndoReversesDeletion) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  // `delete_()` removes the character at the cursor (column 2 = first
  // 'l'), so "Hello" → "Helo". Previously this test expected "Hllo",
  // which is what *backspace* would produce.
  editor.delete_();
  EXPECT_EQ(editor.getText(), "Helo");
  editor.undo();
  EXPECT_EQ(editor.getText(), "Hello") << "undo should revert deletion";
}

TEST_F(TextEditorTests, RedoRestoresAfterUndo) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.enterCharacter('!', /*shift=*/false);
  editor.undo();
  editor.redo();
  EXPECT_EQ(editor.getText(), "Hello!") << "redo should restore undone change";
}

TEST_F(TextEditorTests, MultipleUndoStepsBackthrough) {
  editor.setText("H");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.enterCharacter('e', /*shift=*/false);
  editor.enterCharacter('l', /*shift=*/false);
  editor.enterCharacter('l', /*shift=*/false);
  editor.enterCharacter('o', /*shift=*/false);
  EXPECT_EQ(editor.getText(), "Hello");
  editor.undo(3);
  EXPECT_EQ(editor.getText(), "He") << "undo(3) should step back 3 operations";
}

TEST_F(TextEditorTests, RedoIsClearedAfterNewEdit) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.enterCharacter('!', /*shift=*/false);
  editor.undo();
  EXPECT_TRUE(editor.canRedo()) << "Should be able to redo after undo";
  editor.enterCharacter('?', /*shift=*/false);
  EXPECT_FALSE(editor.canRedo()) << "redo should be cleared after new edit";
}

TEST_F(TextEditorTests, CanUndoReturnsFalseAtStart) {
  editor.setText("Hello");
  EXPECT_FALSE(editor.canUndo()) << "canUndo should return false with no undo history";
}

TEST_F(TextEditorTests, CanUndoReturnsTrueAfterEdit) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.enterCharacter('!', /*shift=*/false);
  EXPECT_TRUE(editor.canUndo()) << "canUndo should return true after edit";
}

// ============================================================================
// DOUBLE-CLICK WORD SELECTION TESTS
// ============================================================================

TEST_F(TextEditorTests, SelectWordUnderCursorSelectsWord) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 2));  // Inside "Hello"
  editor.selectWordUnderCursor();
  EXPECT_EQ(editor.getSelectedText(), "Hello") << "selectWordUnderCursor should select entire word";
}

TEST_F(TextEditorTests, SelectWordAtPunctuation) {
  editor.setText("Hello, world");
  editor.setCursorPosition(Coordinates(0, 5));  // On the comma
  editor.selectWordUnderCursor();
  // Comma is a punctuation token, so it should be selected as a single unit
  EXPECT_EQ(editor.getSelectedText(), ",")
      << "selectWordUnderCursor on punctuation should select punctuation run";
}

TEST_F(TextEditorTests, SelectWordOnWhitespace) {
  editor.setText("Hello  world");
  editor.setCursorPosition(Coordinates(0, 6));  // On the space
  editor.selectWordUnderCursor();
  // Should select the whitespace run
  EXPECT_EQ(editor.getSelectedText(), "  ")
      << "selectWordUnderCursor on whitespace should select whitespace run";
}

// ============================================================================
// MULTI-LINE EDITING TESTS
// ============================================================================

TEST_F(TextEditorTests, PasteMultiLineText) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.insertText("!\nWorld");
  EXPECT_EQ(editor.getText(), "Hello!\nWorld") << "insertText should handle multi-line text";
}

TEST_F(TextEditorTests, SelectionSpanningMultipleLines) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setSelection(Coordinates(0, 2), Coordinates(2, 2));
  EXPECT_EQ(editor.getSelectedText(), "ne1\nLine2\nLi")
      << "setSelection should select across multiple lines";
}

TEST_F(TextEditorTests, InsertNewlineAtLineStart) {
  editor.setText("Hello\nWorld");
  editor.setCursorPosition(Coordinates(1, 0));
  editor.insertText("\n");
  EXPECT_EQ(editor.getText(), "Hello\n\nWorld") << "insertText newline should split line";
}

// ============================================================================
// CLIPBOARD TESTS
// ============================================================================

TEST_F(TextEditorTests, CopyWithSelection) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.copy();
  const char* clipboard = ImGui::GetClipboardText();
  EXPECT_STREQ(clipboard, "Hello") << "copy with selection should copy selected text";
}

TEST_F(TextEditorTests, CopyWithoutSelectionCopiesLine) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.copy();
  const char* clipboard = ImGui::GetClipboardText();
  EXPECT_STREQ(clipboard, "Hello") << "copy without selection should copy entire line";
}

TEST_F(TextEditorTests, CutWithSelection) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.cut();
  EXPECT_EQ(editor.getText(), " world") << "cut should remove selected text and copy to clipboard";
  const char* clipboard = ImGui::GetClipboardText();
  EXPECT_STREQ(clipboard, "Hello");
}

TEST_F(TextEditorTests, CutCapturesDeleteIntentAndUndoRestoresText) {
  editor.setText("Hello world");
  editor.resetTextChanged();
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));

  editor.cut();

  std::vector<SourceEditIntent> intents = editor.takePendingSourceEditIntents();
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].offset, 0u);
  EXPECT_EQ(intents[0].removedLength, 5u);
  EXPECT_EQ(intents[0].replacement, "");
  EXPECT_EQ(intents[0].kind, SourceEditIntentKind::Delete);

  editor.undo();
  EXPECT_EQ(editor.getText(), "Hello world");
}

TEST_F(TextEditorTests, CutWithoutSelectionDoesNothing) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.cut();
  EXPECT_EQ(editor.getText(), "Hello") << "cut without selection should not delete";
}

TEST_F(TextEditorTests, PasteInsertsClipboardText) {
  editor.setText("Hello");
  editor.resetTextChanged();
  ImGui::SetClipboardText(" world");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.paste();
  EXPECT_EQ(editor.getText(), "Hello world") << "paste should insert clipboard text at cursor";

  std::vector<SourceEditIntent> intents = editor.takePendingSourceEditIntents();
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].offset, 5u);
  EXPECT_EQ(intents[0].removedLength, 0u);
  EXPECT_EQ(intents[0].replacement, " world");
  EXPECT_EQ(intents[0].kind, SourceEditIntentKind::Insert);
}

TEST_F(TextEditorTests, PasteWithSelectionReplacesSelection) {
  editor.setText("Hello world");
  editor.resetTextChanged();
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  ImGui::SetClipboardText("Hi");
  editor.paste();
  EXPECT_EQ(editor.getText(), "Hi world") << "paste with selection should replace selection";

  std::vector<SourceEditIntent> intents = editor.takePendingSourceEditIntents();
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].offset, 0u);
  EXPECT_EQ(intents[0].removedLength, 5u);
  EXPECT_EQ(intents[0].replacement, "Hi");
  EXPECT_EQ(intents[0].kind, SourceEditIntentKind::Replace);

  editor.undo();
  EXPECT_EQ(editor.getText(), "Hello world");
}

TEST_F(TextEditorTests, ProcessReplaceCapturesReplaceIntentAndUndoRestoresText) {
  editor.setText("red blue red");
  editor.resetTextChanged();

  editor.processReplace("red", "green");

  EXPECT_EQ(editor.getText(), "green blue red");
  std::vector<SourceEditIntent> intents = editor.takePendingSourceEditIntents();
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].offset, 0u);
  EXPECT_EQ(intents[0].removedLength, 3u);
  EXPECT_EQ(intents[0].replacement, "green");
  EXPECT_EQ(intents[0].kind, SourceEditIntentKind::Replace);

  editor.undo();
  EXPECT_EQ(editor.getText(), "red blue red");
}

// ============================================================================
// RENDERED SOURCE VIEW TESTS
// ============================================================================

TEST_F(TextEditorTests, RenderBuildsWrappedVisualRowsForLongXmlLine) {
  editor.setText(R"(  <rect id="target" x="10" y="20" width="30" height="40" fill="red"/>)");

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  ASSERT_GT(VisualLineCount(), 1);
  const int continuationIndex = FirstContinuationVisualLineForLine(0);
  ASSERT_NE(continuationIndex, -1);
  EXPECT_TRUE(VisualLineIsContinuation(continuationIndex));
  EXPECT_EQ(VisualLineIndentColumns(continuationIndex), 8);
  EXPECT_FALSE(editor.isImGuiChildIgnored());
  EXPECT_TRUE(editor.wordWrapEnabled());
  EXPECT_FALSE(HorizontalScrollEnabled());
}

TEST_F(TextEditorTests, WrappedHitTestingMapsContinuationRowToLogicalColumn) {
  editor.setText(R"(  <rect id="target" x="10" y="20" width="30" height="40" fill="red"/>)");

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  const int continuationIndex = FirstContinuationVisualLineForLine(0);
  ASSERT_NE(continuationIndex, -1);
  ASSERT_GT(VisualLineEndColumn(continuationIndex), VisualLineStartColumn(continuationIndex) + 2);

  const Coordinates hit = CoordinatesAtVisualTextOffset(continuationIndex, 2);

  EXPECT_EQ(hit, Coordinates(0, VisualLineStartColumn(continuationIndex) + 2));
}

TEST_F(TextEditorTests, FocusPartitionHidesLinesFromRenderedVisualLayout) {
  editor.setText("root\nhidden-a\nhidden-b\ntarget\nclose");
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 3, .endLine = 4}},
      .dimmed = {LineRange{.startLine = 0, .endLine = 1}, LineRange{.startLine = 4, .endLine = 5}},
      .hidden = {LineRange{.startLine = 1, .endLine = 3}},
  });

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  EXPECT_EQ(VisualLineLogicalLines(), (std::vector<int>{0, 1, 3, 4}));
  ASSERT_GE(VisualLineCount(), 3);
  EXPECT_TRUE(VisualLineIsFocusHiddenPlaceholder(1));
  EXPECT_EQ(VisualLineHiddenRange(1), (LineRange{.startLine = 1, .endLine = 3}));
  EXPECT_EQ(CoordinatesAtVisualTextOffset(2, 2), Coordinates(3, 2));
}

TEST_F(TextEditorTests, ClickingFocusHiddenPlaceholderExpandsRangeWithoutMovingCursor) {
  editor.setText("root\nhidden-a\nhidden-b\ntarget\nclose");
  const FocusPartition partition{
      .fullColor = {LineRange{.startLine = 3, .endLine = 4}},
      .dimmed = {LineRange{.startLine = 0, .endLine = 1}, LineRange{.startLine = 4, .endLine = 5}},
      .hidden = {LineRange{.startLine = 1, .endLine = 3}},
  };
  editor.setFocusPartition(partition);
  editor.setCursorPosition(Coordinates(3, 0));

  RenderEditorFrame(ImVec2(300.0f, 180.0f));
  ASSERT_TRUE(VisualLineIsFocusHiddenPlaceholder(1));

  const ImVec2 clickPos =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/1, /*visualColumnOffset=*/1);
  RenderEditorFrameWithMouse(clickPos, false, ImVec2(300.0f, 180.0f));
  RenderEditorFrameWithMouse(clickPos, true, ImVec2(300.0f, 180.0f));

  EXPECT_EQ(VisualLineLogicalLines(), (std::vector<int>{0, 1, 2, 3, 4}));
  EXPECT_FALSE(VisualLineIsFocusHiddenPlaceholder(1));
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(3, 0));
  EXPECT_FALSE(editor.isCursorPositionChanged());

  editor.setFocusPartition(partition);
  RenderEditorFrame(ImVec2(300.0f, 180.0f));
  EXPECT_EQ(VisualLineLogicalLines(), (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST_F(TextEditorTests, CursorInsideFocusRangeTracksVisibleFocusBrightnessLines) {
  editor.setText("root\nreference\ntarget\nclosing\nhidden");
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 2, .endLine = 3}},
      .referenceColor = {LineRange{.startLine = 1, .endLine = 2}},
      .dimmed = {LineRange{.startLine = 0, .endLine = 1}, LineRange{.startLine = 3, .endLine = 4}},
      .hidden = {LineRange{.startLine = 4, .endLine = 5}},
  });

  editor.setCursorPosition(Coordinates(2, 1));
  EXPECT_TRUE(editor.isCursorInsideFocusRange());

  editor.setCursorPosition(Coordinates(0, 0));
  EXPECT_TRUE(editor.isCursorInsideFocusRange());

  editor.setCursorPosition(Coordinates(1, 0));
  EXPECT_TRUE(editor.isCursorInsideFocusRange());

  editor.setCursorPosition(Coordinates(4, 0));
  EXPECT_FALSE(editor.isCursorInsideFocusRange());

  editor.clearFocusPartition();
  editor.setCursorPosition(Coordinates(2, 1));
  EXPECT_FALSE(editor.isCursorInsideFocusRange());
}

TEST_F(TextEditorTests, ExternalSourceEditQueuesRenderedFlashDecoration) {
  editor.setText("<svg></svg>");
  editor.resetTextChanged();

  editor.applyExternalSourceEdit(5, 0, "<g/>");
  RenderEditorFrame(ImVec2(300.0f, 120.0f));

  ASSERT_TRUE(HasActiveSourceFlash());
  const std::vector<ActiveFlash> flashes = ActiveSourceFlashes();
  ASSERT_EQ(flashes.size(), 1u);
  EXPECT_EQ(flashes[0].byteRange, (SourceByteRange{.start = 5, .end = 9}));
  EXPECT_GT(flashes[0].intensity, 0.0f);
}

TEST_F(TextEditorTests, ExternalSourceEditInsideSelectionExtendsSelection) {
  constexpr std::string_view kSource =
      R"(<svg><rect id="r1" x="0" y="0" width="10" height="10"/></svg>)";
  constexpr std::string_view kInserted = " transform=\"translate(5)\"";
  editor.setText(kSource);
  editor.resetTextChanged();

  const std::size_t selectionStart = kSource.find("<rect");
  ASSERT_NE(selectionStart, std::string_view::npos);
  const std::size_t selectionEnd = kSource.find("/>", selectionStart);
  ASSERT_NE(selectionEnd, std::string_view::npos);
  const std::size_t selectedSourceEnd = selectionEnd + 2;
  editor.setSelection(editor.getCoordinatesAtByteOffset(selectionStart),
                      editor.getCoordinatesAtByteOffset(selectedSourceEnd));
  editor.setCursorPosition(editor.getCoordinatesAtByteOffset(selectionStart));

  editor.applyExternalSourceEdit(selectionEnd, 0, kInserted);

  EXPECT_EQ(
      editor.getSelectedText(),
      R"expected(<rect id="r1" x="0" y="0" width="10" height="10" transform="translate(5)"/>)expected");
  EXPECT_EQ(editor.getCursorPosition(), editor.getCoordinatesAtByteOffset(selectionStart));
}

TEST_F(TextEditorTests, ExternalSourceEditInsideReverseSelectionExtendsSelection) {
  constexpr std::string_view kSource =
      R"(<svg><rect id="r1" x="0" y="0" width="10" height="10"/></svg>)";
  constexpr std::string_view kInserted = " transform=\"translate(5)\"";
  editor.setText(kSource);
  editor.resetTextChanged();

  const std::size_t selectionStart = kSource.find("<rect");
  ASSERT_NE(selectionStart, std::string_view::npos);
  const std::size_t selectionEnd = kSource.find("/>", selectionStart);
  ASSERT_NE(selectionEnd, std::string_view::npos);
  const std::size_t selectedSourceEnd = selectionEnd + 2;
  editor.setSelection(editor.getCoordinatesAtByteOffset(selectedSourceEnd),
                      editor.getCoordinatesAtByteOffset(selectionStart));

  editor.applyExternalSourceEdit(selectionEnd, 0, kInserted);

  EXPECT_EQ(
      editor.getSelectedText(),
      R"expected(<rect id="r1" x="0" y="0" width="10" height="10" transform="translate(5)"/>)expected");
}

TEST_F(TextEditorTests, SourceFocusModeContextMenuStateAndToggleRequestAreConsumable) {
  editor.setSourceFocusModeContextMenu(true);

  EXPECT_TRUE(SourceFocusModeContextMenuVisible());
  EXPECT_TRUE(SourceFocusModeContextMenuChecked());
  EXPECT_FALSE(editor.takeSourceFocusModeContextMenuToggleRequest());

  RequestSourceFocusModeContextMenuToggle();
  EXPECT_TRUE(editor.takeSourceFocusModeContextMenuToggleRequest());
  EXPECT_FALSE(editor.takeSourceFocusModeContextMenuToggleRequest());

  editor.clearSourceFocusModeContextMenu();
  EXPECT_FALSE(SourceFocusModeContextMenuVisible());
}

TEST_F(TextEditorTests, FocusReferenceConnectorsRouteThroughDistinctRightSideLanes) {
  editor.setText(
      "  <defs>\n"
      "    <linearGradient id=\"grad\"/>\n"
      "  </defs>\n"
      "  <rect fill=\"url(#grad)\"/>\n"
      "  <circle stroke=\"url(#grad)\"/>\n");
  const FocusReferenceLink rectLink{
      .from = SourcePoint{.line = 3, .column = 19},
      .to = SourcePoint{.line = 1, .column = 4},
  };
  const FocusReferenceLink circleLink{
      .from = SourcePoint{.line = 4, .column = 23},
      .to = SourcePoint{.line = 1, .column = 4},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 1, .endLine = 5}},
      .referenceLinks = {rectLink, circleLink},
  });

  RenderEditorFrame(ImVec2(520.0f, 180.0f));

  auto rectLayout = FocusReferenceLayout(rectLink, 0);
  auto circleLayout = FocusReferenceLayout(circleLink, 1);
  ASSERT_TRUE(rectLayout.has_value());
  ASSERT_TRUE(circleLayout.has_value());

  EXPECT_GT(rectLayout->laneStart.x, rectLayout->start.x);
  EXPECT_FLOAT_EQ(rectLayout->laneStart.x, rectLayout->laneEnd.x);
  EXPECT_GT(rectLayout->laneEnd.x, rectLayout->tip.x);
  EXPECT_NE(rectLayout->laneStart.x, circleLayout->laneStart.x);

  const float baselineY = TextBaselineOffsetY();
  const ImVec2 rectSourceTop = ScreenPointAtCoordinates(Coordinates(3, 19));
  const ImVec2 gradientTargetTop = ScreenPointAtCoordinates(Coordinates(1, 4));
  EXPECT_FLOAT_EQ(rectLayout->start.y, rectSourceTop.y + baselineY);
  EXPECT_FLOAT_EQ(rectLayout->tip.y, gradientTargetTop.y + baselineY);

  EXPECT_NE(rectLayout->color, circleLayout->color);
  const float alpha = ImGui::ColorConvertU32ToFloat4(rectLayout->color).w;
  EXPECT_GE(alpha, 0.45f);
  EXPECT_LE(alpha, 0.55f);
}

TEST_F(TextEditorTests, FocusReferenceConnectorTerminatesOnRightSideOfSourceStyleChip) {
  editor.setText(
      "<linearGradient id=\"paint\">\n"
      "<rect fill=\"url(#paint)\"/>\n");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 94,
          .range = SourceByteRange{.start = 0, .end = 27},
          .chipRange = SourceByteRange{.start = 0, .end = 27},
          .showChip = true,
          .chipCount = 1,
          .chipTooltip = "Referenced 1 time",
      },
  }));

  const FocusReferenceLink link{
      .from = SourcePoint{.line = 1, .column = 17},
      .to = SourcePoint{.line = 0, .column = 27},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 0, .endLine = 2}},
      .referenceLinks = {link},
  });

  RenderEditorFrame(ImVec2(520.0f, 140.0f));
  ASSERT_EQ(SourceStyleChipHitRectCount(), 1u);

  const auto layout = FocusReferenceLayout(link, 0);
  ASSERT_TRUE(layout.has_value());

  const ImVec2 chipMin = SourceStyleChipHitRectMin(0);
  const ImVec2 chipMax = SourceStyleChipHitRectMax(0);
  EXPECT_FLOAT_EQ(layout->tip.x, chipMax.x);
  EXPECT_FLOAT_EQ(layout->tip.y, chipMin.y + (chipMax.y - chipMin.y) * 0.5f);
}

TEST_F(TextEditorTests, ReferenceOnlyFocusPartitionLeavesAllLinesVisible) {
  editor.setText(
      "  <defs>\n"
      "    <linearGradient id=\"grad\"/>\n"
      "  </defs>\n"
      "  <rect fill=\"url(#grad)\"/>\n");
  const FocusReferenceLink link{
      .from = SourcePoint{.line = 3, .column = 19},
      .to = SourcePoint{.line = 1, .column = 4},
  };
  editor.setFocusPartition(FocusPartition{
      .referenceLinks = {link},
  });

  RenderEditorFrame(ImVec2(520.0f, 180.0f));

  EXPECT_EQ(VisualLineLogicalLines(), (std::vector<int>{0, 1, 2, 3}));
  EXPECT_TRUE(FocusReferenceLayout(link, 0).has_value());
}

TEST_F(TextEditorTests, SelectAndFocusScrollsToWrappedVisualCursorLine) {
  editor.setText(
      "  <rect id=\"target\" x=\"10\" y=\"20\" width=\"30\" height=\"40\" "
      "fill=\"red\" stroke=\"blue\" opacity=\"0.5\" transform=\"translate(1 2)\"/>\n");

  RenderEditorFrame(ImVec2(240.0f, 80.0f));

  const Coordinates targetStart(0, 96);
  const int targetVisualLine = VisualLineIndexForCoordinates(targetStart);
  ASSERT_GT(targetVisualLine, 0);

  editor.selectAndFocus(targetStart, Coordinates(0, 108));
  RenderEditorFrame(ImVec2(240.0f, 80.0f));
  RenderEditorFrame(ImVec2(240.0f, 80.0f));

  EXPECT_GT(LastScrollY(), 0.0f);
}

TEST_F(TextEditorTests, SelectAndFocusScrollsEntireSelectionIntoView) {
  std::ostringstream source;
  for (int i = 0; i < 80; ++i) {
    source << "line" << i << "\n";
  }
  editor.setText(source.str());

  constexpr ImVec2 kEditorSize(240.0f, 180.0f);
  RenderEditorFrame(kEditorSize);

  editor.selectAndFocus(Coordinates(40, 0), Coordinates(45, 6));
  RenderEditorFrame(kEditorSize);
  RenderEditorFrame(kEditorSize);

  const int firstVisible = static_cast<int>(std::floor(LastScrollY() / CharacterAdvanceY()));
  const int lastVisible =
      firstVisible +
      std::max(1, static_cast<int>(std::floor(LastScrollViewportHeight() / CharacterAdvanceY()))) -
      1;
  EXPECT_LE(firstVisible, 40) << "scrollY=" << LastScrollY() << " charY=" << CharacterAdvanceY()
                              << " viewportY=" << LastScrollViewportHeight();
  EXPECT_GE(lastVisible, 45) << "firstVisible=" << firstVisible << " scrollY=" << LastScrollY()
                             << " charY=" << CharacterAdvanceY()
                             << " viewportY=" << LastScrollViewportHeight();
}

TEST_F(TextEditorTests, TypingOnTopVisibleLineDoesNotNudgeScrollUp) {
  std::ostringstream source;
  for (int i = 0; i < 80; ++i) {
    source << "line" << i << "\n";
  }
  editor.setText(source.str());

  constexpr ImVec2 kEditorSize(240.0f, 80.0f);
  RenderEditorFrame(kEditorSize);
  editor.selectAndFocus(Coordinates(40, 0), Coordinates(40, 0));
  RenderEditorFrame(kEditorSize);
  RenderEditorFrame(kEditorSize);

  const int topVisibleLine = static_cast<int>(std::floor(LastScrollY() / CharacterAdvanceY()));
  editor.setCursorPosition(Coordinates(topVisibleLine, 0));
  RenderEditorFrame(kEditorSize);
  RenderEditorFrame(kEditorSize);
  const float beforeTypingScrollY = LastScrollY();

  EnterCharacter('x');
  RenderEditorFrame(kEditorSize);
  RenderEditorFrame(kEditorSize);

  EXPECT_GE(LastScrollY() + 0.5f, beforeTypingScrollY);
}

TEST_F(TextEditorTests, AutocompletePopupDoesNotPerturbEditorScroll) {
  std::ostringstream source;
  for (int i = 0; i < 80; ++i) {
    source << "<path id=\"line" << i << "\" class=\"cls-" << i << "\"/>\n";
  }
  editor.setText(source.str());

  constexpr ImVec2 kEditorSize(240.0f, 80.0f);
  RenderEditorFrame(kEditorSize);
  editor.selectAndFocus(Coordinates(40, 8), Coordinates(40, 8));
  RenderEditorFrame(kEditorSize);
  RenderEditorFrame(kEditorSize);

  const float beforePopupScrollY = LastScrollY();
  OpenAutocompleteAtCursor("style");
  RenderEditorFrame(kEditorSize);
  const float afterOpenScrollY = LastScrollY();

  ReplaceAutocompleteSuggestion("stroke");
  RenderEditorFrame(kEditorSize);
  const float afterSuggestionChangeScrollY = LastScrollY();

  EXPECT_NEAR(afterOpenScrollY, beforePopupScrollY, 0.5f);
  EXPECT_NEAR(afterSuggestionChangeScrollY, beforePopupScrollY, 0.5f);
  EXPECT_EQ(AutocompleteChildWindowCount(), 0);
  EXPECT_EQ(AutocompleteTopLevelWindowCount(), 1);
}

// ============================================================================
// COORDINATE SYSTEM & EDGE CASE TESTS
// ============================================================================

TEST_F(TextEditorTests, SetTextClearsBuffer) {
  editor.setText("Hello");
  editor.setText("World");
  EXPECT_EQ(editor.getText(), "World") << "setText should replace entire buffer";
}

TEST_F(TextEditorTests, EmptyBufferHasSingleLine) {
  TextEditor emptyEditor;
  EXPECT_EQ(emptyEditor.getText(), "") << "Empty editor should have no text";
}

TEST_F(TextEditorTests, GetTextReturnsExactBuffer) {
  std::string content = "Hello\nWorld\nTest";
  editor.setText(content);
  EXPECT_EQ(editor.getText(), content) << "getText should return exact buffer content";
}

TEST_F(TextEditorTests, SelectionNormalizedIfStartAfterEnd) {
  editor.setText("Hello world");
  // Pass the bounds in reverse order — `setSelection` should
  // normalize them so `start <= end` and the resulting selection
  // covers the same character range as if they'd been in order.
  editor.setSelection(Coordinates(0, 8), Coordinates(0, 2));
  // Half-open range [2, 8) over "Hello world" = "llo wo".
  EXPECT_EQ(editor.getSelectedText(), "llo wo") << "setSelection should normalize reversed ranges";
}

TEST_F(TextEditorTests, MultipleDeletesRemoveChars) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.delete_();
  editor.delete_();
  editor.delete_();
  EXPECT_EQ(editor.getText(), "He") << "Multiple deletes should remove multiple characters";
}

TEST_F(TextEditorTests, InsertAfterUndoDoesNotClearRedo) {
  editor.setText("A");
  // Drop the cursor at the end of "A" before inserting so the
  // following inserts append rather than prepending. `setText`
  // leaves the cursor at (0, 0).
  editor.setCursorPosition(Coordinates(0, 1));
  editor.insertText("B");
  editor.insertText("C");
  EXPECT_EQ(editor.getText(), "ABC");
  editor.undo();
  editor.undo();
  EXPECT_EQ(editor.getText(), "A");
  EXPECT_TRUE(editor.canRedo()) << "Should be able to redo after undo";
  // Now insert new text - this should clear redo
  editor.insertText("X");
  EXPECT_FALSE(editor.canRedo()) << "New edit should clear redo history";
}

// ============================================================================
// HOME/END TESTS
// ============================================================================

TEST_F(TextEditorTests, HomeOnSecondLineMovesToStartOfLine) {
  editor.setText("Line1\nLine2");
  editor.setCursorPosition(Coordinates(1, 3));
  editor.moveHome(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 0))
      << "moveHome should move to column 0 of current line";
}

TEST_F(TextEditorTests, EndOnFirstLineMovesToEndOfLine) {
  editor.setText("Line1\nLine2");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.moveEnd(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 5))
      << "moveEnd should move to end of current line";
}

// ============================================================================
// SELECTION WITH MOVEMENT TESTS
// ============================================================================

TEST_F(TextEditorTests, ShiftHomeSelectsToLineStart) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 3));
  editor.moveHome(true);  // select=true
  EXPECT_EQ(editor.getSelectedText(), "Hel")
      << "Shift+Home should select from cursor to line start";
}

TEST_F(TextEditorTests, ShiftEndSelectsToLineEnd) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.moveEnd(true);  // select=true
  EXPECT_EQ(editor.getSelectedText(), "llo") << "Shift+End should select from cursor to line end";
}

TEST_F(TextEditorTests, ShiftUpSelectsMultipleLinesUp) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(2, 1));
  editor.moveUp(1, true);  // select=true
  EXPECT_TRUE(editor.hasSelection()) << "Shift+Up should create selection";
}

TEST_F(TextEditorTests, ShiftDownSelectsMultipleLinesDown) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.moveDown(1, true);  // select=true
  EXPECT_TRUE(editor.hasSelection()) << "Shift+Down should create selection";
}

TEST_F(TextEditorTests, ShiftCtrlEndSelectsToDocumentEnd) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.moveBottom(true);  // select=true to document end
  EXPECT_TRUE(editor.hasSelection()) << "Shift+Ctrl+End should create selection to document end";
}

TEST_F(TextEditorTests, ShiftCtrlHomeSelectsToDocumentStart) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(2, 2));
  editor.moveTop(true);  // select=true to document start
  EXPECT_TRUE(editor.hasSelection()) << "Shift+Ctrl+Home should create selection to document start";
}

TEST_F(TextEditorTests, MultiLineDeleteAcrossLines) {
  editor.setText("Line1\nLine2\nLine3");
  // Select cols 2..4 across lines 0..2 (half-open). That covers
  // "ne1\nLine2\nLine" — the trailing 4 chars on line 0, all of
  // line 1, and the leading 4 chars on line 2. Replacing it with
  // empty text should leave "Li" + "3" = "Li3".
  editor.setSelection(Coordinates(0, 2), Coordinates(2, 4));
  editor.insertText("");
  EXPECT_EQ(editor.getText(), "Li3") << "Replacing multi-line selection should join remaining text";
}

TEST_F(TextEditorTests, CopyMultipleLines) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setSelection(Coordinates(0, 0), Coordinates(2, 5));
  editor.copy();
  const char* clipboard = ImGui::GetClipboardText();
  EXPECT_STREQ(clipboard, "Line1\nLine2\nLine3") << "copy should handle multi-line selections";
}

TEST_F(TextEditorTests, InsertMultipleCharactersInSequence) {
  editor.setText("");
  editor.insertText("H");
  editor.insertText("e");
  editor.insertText("l");
  editor.insertText("l");
  editor.insertText("o");
  EXPECT_EQ(editor.getText(), "Hello") << "Sequential character insertions should build word";
}

TEST_F(TextEditorTests, GetSelectedTextWithoutSelection) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  std::string selected = editor.getSelectedText();
  EXPECT_EQ(selected, "") << "getSelectedText without selection should return empty string";
}

TEST_F(TextEditorTests, MoveRightMultipleTimes) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 0));
  editor.moveRight(5, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 5))
      << "moveRight(5) should move cursor 5 columns";
}

TEST_F(TextEditorTests, MoveLeftMultipleTimes) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 4));
  editor.moveLeft(3, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 1))
      << "moveLeft(3) should move cursor 3 columns";
}

TEST_F(TextEditorTests, SelectionPreservesAfterMove) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  EXPECT_EQ(editor.getSelectedText(), "Hello");
  // Move cursor - selection should remain
  editor.moveRight(2, false, false);
  EXPECT_EQ(editor.getSelectedText(), "Hello")
      << "Selection should be preserved after non-selection move";
}

TEST_F(TextEditorTests, DeleteMultipleSelectionsSuccessively) {
  editor.setText("ABCDEFGH");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 2));
  editor.insertText("");  // Delete "AB"
  EXPECT_EQ(editor.getText(), "CDEFGH");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 2));
  editor.insertText("");  // Delete "CD"
  EXPECT_EQ(editor.getText(), "EFGH") << "Successive deletions should work correctly";
}

TEST_F(TextEditorTests, UndoMultipleOperations) {
  editor.setText("A");
  // Position cursor at end of "A" so the following inserts append
  // rather than prepending. (`setText` leaves the cursor at the
  // start of the buffer.)
  editor.setCursorPosition(Coordinates(0, 1));
  editor.insertText("B");
  editor.insertText("C");
  editor.insertText("D");
  EXPECT_EQ(editor.getText(), "ABCD");
  // Three insertText calls → three undo entries; `undo(4)` walks
  // them all and stops at the start of the undo buffer.
  editor.undo(4);
  EXPECT_EQ(editor.getText(), "A") << "undo(4) should revert to 'A'";
}

TEST_F(TextEditorTests, RedoAfterMultipleUndos) {
  editor.setText("X");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.insertText("Y");
  editor.insertText("Z");
  editor.undo(2);
  EXPECT_EQ(editor.getText(), "X");
  editor.redo(1);
  EXPECT_EQ(editor.getText(), "XY") << "redo(1) should restore one operation";
}

TEST_F(TextEditorTests, SetSelectionOnMultipleLines) {
  editor.setText("AAA\nBBB\nCCC");
  editor.setSelection(Coordinates(0, 1), Coordinates(2, 2));
  std::string selected = editor.getSelectedText();
  EXPECT_EQ(selected, "AA\nBBB\nCC") << "setSelection should correctly span multiple lines";
}

TEST_F(TextEditorTests, CursorStaysInBoundsAfterDelete) {
  editor.setText("AB");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.delete_();
  // Cursor should remain at valid position (column 2 is at end of "AB")
  Coordinates pos = editor.getCursorPosition();
  EXPECT_LE(pos.column, 2) << "Cursor should stay in valid bounds after delete";
}

TEST_F(TextEditorTests, EmptyLineHandling) {
  editor.setText("A\n\nC");
  EXPECT_EQ(editor.getText(), "A\n\nC");
  editor.setCursorPosition(Coordinates(1, 0));
  editor.insertText("B");
  EXPECT_EQ(editor.getText(), "A\nB\nC") << "insertText on empty line should add character";
}

TEST_F(TextEditorTests, SelectWordAtLineEnd) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 4));
  editor.selectWordUnderCursor();
  // Should select "Hello" (the word containing position 4)
  EXPECT_EQ(editor.getSelectedText(), "Hello")
      << "selectWordUnderCursor at line end should select the word";
}

TEST_F(TextEditorTests, SelectAllOnMultilineDocument) {
  std::string content = "Line1\nLine2\nLine3";
  editor.setText(content);
  editor.selectAll();
  EXPECT_EQ(editor.getSelectedText(), content)
      << "selectAll should select entire multi-line document";
}

}  // namespace donner::editor
