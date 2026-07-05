#include "donner/editor/TextEditorCore.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace donner::editor {

void PrintTo(const Coordinates& coords, std::ostream* os) {
  *os << "Coordinates(line=" << coords.line << ", column=" << coords.column << ")";
}

void PrintTo(ColorIndex color, std::ostream* os) {
  switch (color) {
    case ColorIndex::Default: *os << "Default"; return;
    case ColorIndex::Keyword: *os << "Keyword"; return;
    case ColorIndex::Number: *os << "Number"; return;
    case ColorIndex::String: *os << "String"; return;
    case ColorIndex::CharLiteral: *os << "CharLiteral"; return;
    case ColorIndex::Punctuation: *os << "Punctuation"; return;
    case ColorIndex::Identifier: *os << "Identifier"; return;
    case ColorIndex::KnownIdentifier: *os << "KnownIdentifier"; return;
    case ColorIndex::Comment: *os << "Comment"; return;
    case ColorIndex::MultiLineComment: *os << "MultiLineComment"; return;
    case ColorIndex::Background: *os << "Background"; return;
    case ColorIndex::Cursor: *os << "Cursor"; return;
    case ColorIndex::Selection: *os << "Selection"; return;
    case ColorIndex::ErrorMarker: *os << "ErrorMarker"; return;
    case ColorIndex::Breakpoint: *os << "Breakpoint"; return;
    case ColorIndex::BreakpointOutline: *os << "BreakpointOutline"; return;
    case ColorIndex::CurrentLineIndicator: *os << "CurrentLineIndicator"; return;
    case ColorIndex::CurrentLineIndicatorOutline: *os << "CurrentLineIndicatorOutline"; return;
    case ColorIndex::LineNumber: *os << "LineNumber"; return;
    case ColorIndex::CurrentLineFill: *os << "CurrentLineFill"; return;
    case ColorIndex::CurrentLineFillInactive: *os << "CurrentLineFillInactive"; return;
    case ColorIndex::CurrentLineEdge: *os << "CurrentLineEdge"; return;
    case ColorIndex::ErrorMessage: *os << "ErrorMessage"; return;
    case ColorIndex::BreakpointDisabled: *os << "BreakpointDisabled"; return;
    case ColorIndex::UserFunction: *os << "UserFunction"; return;
    case ColorIndex::UserType: *os << "UserType"; return;
    case ColorIndex::UniformVariable: *os << "UniformVariable"; return;
    case ColorIndex::GlobalVariable: *os << "GlobalVariable"; return;
    case ColorIndex::LocalVariable: *os << "LocalVariable"; return;
    case ColorIndex::FunctionArgument: *os << "FunctionArgument"; return;
    case ColorIndex::Max: *os << "Max"; return;
  }

  *os << "ColorIndex(" << static_cast<int>(color) << ")";
}

void PrintTo(SourceEditIntentKind kind, std::ostream* os) {
  switch (kind) {
    case SourceEditIntentKind::Unknown: *os << "Unknown"; return;
    case SourceEditIntentKind::Insert: *os << "Insert"; return;
    case SourceEditIntentKind::Delete: *os << "Delete"; return;
    case SourceEditIntentKind::Replace: *os << "Replace"; return;
    case SourceEditIntentKind::Undo: *os << "Undo"; return;
    case SourceEditIntentKind::Redo: *os << "Redo"; return;
  }

  *os << "SourceEditIntentKind(" << static_cast<int>(kind) << ")";
}

using ::testing::AllOf;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Field;
using ::testing::Gt;
using ::testing::IsEmpty;
using ::testing::Optional;

auto SourceEditIntentIs(std::size_t offset, std::size_t removedLength, std::string_view replacement,
                        SourceEditIntentKind kind) {
  return AllOf(Field("offset", &SourceEditIntent::offset, offset),
               Field("removedLength", &SourceEditIntent::removedLength, removedLength),
               Field("replacement", &SourceEditIntent::replacement, std::string(replacement)),
               Field("kind", &SourceEditIntent::kind, kind),
               Field("bufferVersion", &SourceEditIntent::bufferVersion, Gt(0u)));
}

auto SourceEditIntentKindIs(SourceEditIntentKind kind) {
  return Field("kind", &SourceEditIntent::kind, kind);
}

std::vector<std::optional<ColorIndex>> ColorIndexesAt(const Line& line,
                                                      std::initializer_list<std::size_t> indexes) {
  std::vector<std::optional<ColorIndex>> colors;
  colors.reserve(indexes.size());
  for (std::size_t index : indexes) {
    if (index < line.size()) {
      colors.emplace_back(line[index].colorIndex);
    } else {
      colors.emplace_back(std::nullopt);
    }
  }

  return colors;
}

class TextEditorCoreTests : public ::testing::Test {
protected:
  void SetUp() override {
    editor_.setText("0123456789\n0123456789\n0123456789\n0123456789\n0123456789");
  }

  void BeginDrag(const Coordinates& anchor, SelectionMode mode = SelectionMode::Normal) {
    editor_.setCursorPosition(anchor);
    editor_.interactiveStart() = anchor;
    editor_.interactiveEnd() = anchor;
    editor_.setSelection(anchor, anchor, mode);
  }

  void DragTo(const Coordinates& position, SelectionMode mode = SelectionMode::Normal) {
    editor_.setCursorPosition(position);
    editor_.interactiveEnd() = position;
    editor_.setInteractiveSelection(editor_.interactiveStart(), editor_.interactiveEnd(), mode);
  }

  TextEditorCore editor_;
};

// ---------------------------------------------------------------------------
// TextBuffer direct edge cases
// ---------------------------------------------------------------------------

TEST(TextBufferTests, CountLeadingWhitespaceStopsAtFirstNonWhitespace) {
  Line line;
  line.emplace_back(' ', ColorIndex::Default);
  line.emplace_back('\t', ColorIndex::Default);
  line.emplace_back(' ', ColorIndex::Default);
  line.emplace_back('x', ColorIndex::Default);

  EXPECT_EQ(details::CountLeadingWhitespace(line, 4), 6);
  EXPECT_EQ(details::CountLeadingWhitespace(Line(), 4), 0);
}

TEST(TextBufferTests, SetTextKeepsAtLeastOneLineAndDropsTrailingNewline) {
  TextBuffer buffer;

  buffer.setText("");
  EXPECT_EQ(buffer.getTotalLines(), 1);
  EXPECT_THAT(buffer.getText(), IsEmpty());

  buffer.setText("abc\n");
  EXPECT_EQ(buffer.getTotalLines(), 1);
  EXPECT_THAT(buffer.getText(), Eq("abc"));
}

TEST(TextBufferTests, GetTextRangeHandlesInvalidEmptyReversedAndExclusiveEnd) {
  TextBuffer buffer;
  buffer.setText("abcdef\nsecond");

  Coordinates invalid;
  invalid.line = -1;
  EXPECT_THAT(buffer.getText(invalid, Coordinates(0, 1)), IsEmpty());
  EXPECT_THAT(buffer.getText(Coordinates(0, 1), invalid), IsEmpty());
  EXPECT_THAT(buffer.getText(Coordinates(0, 2), Coordinates(0, 2)), IsEmpty());
  EXPECT_THAT(buffer.getText(Coordinates(0, 4), Coordinates(0, 1)), Eq("bcd"));
  EXPECT_THAT(buffer.getText(Coordinates(0, 1), Coordinates(1, 3)), Eq("bcdef\nsec"));

  buffer.setText("abc");
  Coordinates invalidHighLine(99, 0);
  EXPECT_THAT(buffer.getText(Coordinates(0, 1), invalidHighLine), IsEmpty());
  EXPECT_THAT(buffer.getText(Coordinates(0, 1), Coordinates(0, 4)), Eq("bc"));
}

TEST(TextBufferTests, InsertTextAtHandlesCarriageReturnsAndIndentModes) {
  TextBuffer buffer;
  Coordinates where(0, 6);
  buffer.setText("  headtail");

  EXPECT_EQ(buffer.insertTextAt(where, "\r\n}", /*indent=*/true), 1);

  EXPECT_THAT(buffer.getText(), Eq("  head\n}tail"));
  EXPECT_EQ(where, Coordinates(1, 1));

  buffer.setText("    headtail");
  where = Coordinates(0, 8);

  EXPECT_EQ(buffer.insertTextAt(where, "\n  child", /*indent=*/true), 1);

  EXPECT_THAT(buffer.getText(), Eq("    head\n  childtail"));
  EXPECT_EQ(where, Coordinates(1, 7));

  buffer.setText("  headtail");
  where = Coordinates(0, 6);

  EXPECT_EQ(buffer.insertTextAt(where, "\n  ", /*indent=*/true), 1);

  EXPECT_THAT(buffer.getText(), Eq("  head\n  tail"));
  EXPECT_EQ(where, Coordinates(1, 2));

  buffer.setText("  headtail");
  where = Coordinates(0, 6);

  EXPECT_EQ(buffer.insertTextAt(where, "\n\rchild", /*indent=*/true), 1);

  EXPECT_THAT(buffer.getText(), Eq("  head\n  childtail"));
  EXPECT_EQ(where, Coordinates(1, 7));
}

TEST(TextBufferTests, InsertTextAtClampsCharacterIndexPastLineEnd) {
  TextBuffer buffer;
  buffer.setText("ab");
  Coordinates where(0, 99);

  buffer.insertTextAt(where, "X");

  EXPECT_THAT(buffer.getText(), Eq("abX"));
  EXPECT_EQ(where, Coordinates(0, 100));
}

TEST(TextBufferTests, DeleteRangeHandlesNoOpSingleLineAndExclusiveLineEnd) {
  TextBuffer buffer;
  buffer.setText("abc\ndef\nghi");

  buffer.deleteRange(Coordinates(0, 3), Coordinates(0, 3));
  EXPECT_THAT(buffer.getText(), Eq("abc\ndef\nghi"));

  buffer.deleteRange(Coordinates(0, 1), Coordinates(0, 3));
  EXPECT_THAT(buffer.getText(), Eq("a\ndef\nghi"));

  buffer.setText("abc\ndef\nghi");
  buffer.deleteRange(Coordinates(0, 1), Coordinates(1, 4));
  EXPECT_THAT(buffer.getText(), Eq("aghi"));

  buffer.setText("abc\n\nghi");
  buffer.deleteRange(Coordinates(0, 1), Coordinates(1, 0));
  EXPECT_THAT(buffer.getText(), Eq("a\nghi"));
}

TEST(TextBufferTests, InsertLineHandlesBoundsAndSplitPastLineEnd) {
  TextBuffer buffer;
  buffer.setText("abc");

  buffer.insertLine(-5);
  EXPECT_THAT(buffer.getText(), Eq("\nabc"));

  buffer.insertLine(99);
  EXPECT_THAT(buffer.getText(), Eq("\nabc\n"));

  buffer.setText("abc");
  buffer.insertLine(0, 99);
  EXPECT_THAT(buffer.getText(), Eq("abc\n"));

  buffer.setText("abc");
  buffer.insertLine(1, 3);
  EXPECT_THAT(buffer.getText(), Eq("abc\n"));
}

TEST(TextBufferTests, RemoveLineHandlesInvalidRangesAndKeepsOneLine) {
  TextBuffer buffer;
  buffer.setText("a\nb");

  buffer.removeLine(-1);
  buffer.removeLine(99);
  EXPECT_THAT(buffer.getText(), Eq("a\nb"));

  buffer.removeLine(0);
  EXPECT_THAT(buffer.getText(), Eq("b"));

  buffer.removeLine(0);
  EXPECT_THAT(buffer.getText(), IsEmpty());
  EXPECT_EQ(buffer.getTotalLines(), 1);

  buffer.setText("a\nb\nc");
  buffer.removeLine(2, 1);
  EXPECT_THAT(buffer.getText(), Eq("a\nb\nc"));

  buffer.removeLine(-5, 1);
  EXPECT_THAT(buffer.getText(), Eq("b\nc"));

  buffer.removeLine(1, 99);
  EXPECT_THAT(buffer.getText(), Eq("b"));

  buffer.setText("a\nb");
  buffer.removeLine(-5, 99);
  EXPECT_THAT(buffer.getText(), IsEmpty());
  EXPECT_EQ(buffer.getTotalLines(), 1);
}

TEST(TextBufferTests, MetricsHandleTabsUtf8OutOfRangeAndByteOffsets) {
  TextBuffer buffer;
  buffer.setText(std::string("\tab\n\xC3\xA9"));

  EXPECT_EQ(buffer.getLineMaxColumn(0), 4);
  EXPECT_EQ(buffer.getLineMaxColumn(99), 0);
  EXPECT_EQ(buffer.getLineCharacterCount(1), 1);
  EXPECT_EQ(buffer.getLineCharacterCount(99), 0);
  EXPECT_EQ(buffer.getCharacterIndex(Coordinates(0, 3)), 2);
  EXPECT_EQ(buffer.getCharacterColumn(0, 0), 0);
  EXPECT_EQ(buffer.getCharacterColumn(0, 2), 3);
  EXPECT_EQ(buffer.getCharacterColumn(99, 2), 0);
  EXPECT_EQ(buffer.getByteOffset(Coordinates(99, 0)), 6u);

  Coordinates negativeLine;
  negativeLine.line = -1;
  negativeLine.column = 7;
  EXPECT_EQ(buffer.getByteOffset(negativeLine), 3u);

  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(3), Coordinates(0, 4));
  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(4), Coordinates(1, 0));
  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(999), Coordinates(1, 1));

  TextBuffer oneLine;
  oneLine.setText("abc");
  EXPECT_EQ(oneLine.getCoordinatesAtByteOffset(999), Coordinates(0, 3));
}

TEST_F(TextEditorCoreTests, DragUpAcrossMultipleLinesPreservesOriginalAnchor) {
  BeginDrag(Coordinates(3, 5));

  DragTo(Coordinates(2, 7));
  DragTo(Coordinates(0, 2));

  EXPECT_EQ(editor_.interactiveStart(), Coordinates(3, 5));
  EXPECT_EQ(editor_.interactiveEnd(), Coordinates(0, 2));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 2));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(3, 5));
  EXPECT_EQ(editor_.getSelectedText(), "23456789\n0123456789\n0123456789\n01234");
}

TEST_F(TextEditorCoreTests, DragUpPastLineZeroUsesClampedEndpoint) {
  BeginDrag(Coordinates(2, 3));

  // The ImGui shell clamps mouse coordinates above the buffer to `(0, 0)`
  // before forwarding them into the core.
  DragTo(Coordinates(0, 0));

  EXPECT_EQ(editor_.interactiveStart(), Coordinates(2, 3));
  EXPECT_EQ(editor_.interactiveEnd(), Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 3));
  EXPECT_EQ(editor_.getSelectedText(), "0123456789\n0123456789\n012");
}

TEST_F(TextEditorCoreTests, DragForwardThenBackKeepsSelectionAnchoredAtClickPoint) {
  BeginDrag(Coordinates(2, 5));

  DragTo(Coordinates(3, 10));
  DragTo(Coordinates(1, 2));

  EXPECT_EQ(editor_.interactiveStart(), Coordinates(2, 5));
  EXPECT_EQ(editor_.interactiveEnd(), Coordinates(1, 2));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(1, 2));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 5));
  EXPECT_EQ(editor_.getSelectedText(), "23456789\n01234");
}

TEST_F(TextEditorCoreTests, DragDownThenUpSameDistanceInvertsVisibleSelectionCorrectly) {
  BeginDrag(Coordinates(2, 5));

  DragTo(Coordinates(4, 5));
  DragTo(Coordinates(0, 5));

  EXPECT_EQ(editor_.interactiveStart(), Coordinates(2, 5));
  EXPECT_EQ(editor_.interactiveEnd(), Coordinates(0, 5));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 5));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 5));
  EXPECT_EQ(editor_.getSelectedText(), "56789\n0123456789\n01234");
}

TEST_F(TextEditorCoreTests, InsertTextCapturesSourceEditIntent) {
  editor_.resetTextChanged();
  editor_.setCursorPosition(Coordinates(1, 3));

  editor_.insertText("abc");

  std::vector<SourceEditIntent> intents = editor_.takePendingSourceEditIntents();
  EXPECT_THAT(intents,
              ElementsAre(SourceEditIntentIs(14u, 0u, "abc", SourceEditIntentKind::Insert)));
}

TEST_F(TextEditorCoreTests, ReplaceSelectionCapturesSourceEditIntent) {
  editor_.resetTextChanged();
  editor_.setSelection(Coordinates(1, 2), Coordinates(2, 4));

  editor_.insertText("replacement");

  std::vector<SourceEditIntent> intents = editor_.takePendingSourceEditIntents();
  EXPECT_THAT(intents, ElementsAre(SourceEditIntentIs(13u, 13u, "replacement",
                                                      SourceEditIntentKind::Replace)));
}

// ---------------------------------------------------------------------------
// setText / getText / coordinate <-> byte-offset mapping
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, SetTextReplacesBufferAndRequestsScrollToTop) {
  editor_.setText("hello\nworld");

  EXPECT_THAT(editor_.getText(), Eq("hello\nworld"));
  EXPECT_EQ(editor_.buffer().getTotalLines(), 2);
  EXPECT_TRUE(editor_.isTextChanged());
  EXPECT_TRUE(editor_.scrollToTopRequested());
  // setText clears the undo history.
  EXPECT_FALSE(editor_.canUndo());
  EXPECT_FALSE(editor_.canRedo());
}

TEST_F(TextEditorCoreTests, SetTextPreserveScrollDoesNotRequestScrollToTop) {
  editor_.clearScrollToTop();
  editor_.setText("hello\nworld", /*preserveScroll=*/true);

  EXPECT_FALSE(editor_.scrollToTopRequested());
  EXPECT_THAT(editor_.getText(), Eq("hello\nworld"));
}

TEST_F(TextEditorCoreTests, ResetTextChangedClearsFlagAndChangedLines) {
  editor_.setText("a\nb");
  EXPECT_TRUE(editor_.isTextChanged());

  editor_.resetTextChanged();

  EXPECT_FALSE(editor_.isTextChanged());
  EXPECT_THAT(editor_.changedLines(), IsEmpty());
}

TEST_F(TextEditorCoreTests, GetTextRangeReturnsSubstring) {
  EXPECT_THAT(editor_.getText(Coordinates(0, 2), Coordinates(1, 3)), Eq("23456789\n012"));
}

TEST_F(TextEditorCoreTests, ByteOffsetRoundTripsThroughCoordinates) {
  // Line 2 starts after two 10-char lines + two '\n' bytes = byte 22.
  const Coordinates coords = editor_.getCoordinatesAtByteOffset(22);
  EXPECT_EQ(coords, Coordinates(2, 0));
  EXPECT_EQ(editor_.buffer().getByteOffset(coords), 22u);
}

// ---------------------------------------------------------------------------
// Coordinate sanitization / clamping
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, SanitizeClampsLinePastEndToLastLineEnd) {
  const Coordinates clamped = editor_.sanitizeCoordinates(Coordinates(999, 3));
  EXPECT_EQ(clamped, Coordinates(4, 10));
}

TEST_F(TextEditorCoreTests, SanitizeClampsColumnPastLineEnd) {
  EXPECT_EQ(editor_.sanitizeCoordinates(Coordinates(1, 999)), Coordinates(1, 10));
  // A negative column is clamped up to 0 (line is already valid).
  Coordinates negCol(0, 0);
  negCol.column = -5;
  EXPECT_EQ(editor_.sanitizeCoordinates(negCol), Coordinates(0, 0));
}

TEST_F(TextEditorCoreTests, GetCursorPositionIsSanitized) {
  editor_.setCursorPosition(Coordinates(2, 999));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(2, 10));
}

// ---------------------------------------------------------------------------
// Selection edge cases
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, EmptySelectionHasNoSelectedText) {
  editor_.setSelection(Coordinates(1, 3), Coordinates(1, 3));
  EXPECT_FALSE(editor_.hasSelection());
  EXPECT_THAT(editor_.getSelectedText(), IsEmpty());
}

TEST_F(TextEditorCoreTests, ReversedSelectionIsNormalizedToOrderedEndpoints) {
  editor_.setSelection(Coordinates(2, 5), Coordinates(1, 2));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(1, 2));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 5));
  EXPECT_TRUE(editor_.hasSelection());
}

TEST_F(TextEditorCoreTests, SelectAllCoversWholeBuffer) {
  editor_.selectAll();
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 0));
  EXPECT_THAT(editor_.getSelectedText(),
              Eq("0123456789\n0123456789\n0123456789\n0123456789\n0123456789"));
}

TEST_F(TextEditorCoreTests, SetSelectionStartSwapsWhenPastEnd) {
  editor_.setSelection(Coordinates(1, 0), Coordinates(1, 4));
  editor_.setSelectionStart(Coordinates(2, 0));
  // Start moved past end, so the endpoints swap.
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(1, 4));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 0));
}

TEST_F(TextEditorCoreTests, SetSelectionEndSwapsWhenBeforeStart) {
  editor_.setSelection(Coordinates(2, 0), Coordinates(2, 4));
  editor_.setSelectionEnd(Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 0));
}

TEST_F(TextEditorCoreTests, LineSelectionModeExpandsToFullLines) {
  editor_.setSelection(Coordinates(1, 3), Coordinates(2, 4), SelectionMode::Line);
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(1, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 10));
}

TEST_F(TextEditorCoreTests, WordSelectionModeSnapsToWordBoundaries) {
  editor_.setText("foo bar baz");
  editor_.setColorizerEnabled(false);
  editor_.setSelection(Coordinates(0, 5), Coordinates(0, 6), SelectionMode::Word);
  EXPECT_THAT(editor_.getSelectedText(), Eq("bar"));
}

TEST_F(TextEditorCoreTests, WordSelectionModeLeavesBoundaryEndpointInPlace) {
  editor_.setText("alpha beta");
  editor_.setColorizerEnabled(false);

  editor_.setSelection(Coordinates(0, 1), Coordinates(0, 5), SelectionMode::Word);

  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(0, 5));
  EXPECT_THAT(editor_.getSelectedText(), Eq("alpha"));
}

TEST_F(TextEditorCoreTests, IsOnWordBoundaryHandlesDisabledAndColorizedModes) {
  editor_.setText("aa bb");
  editor_.setColorizerEnabled(false);

  EXPECT_TRUE(editor_.isOnWordBoundary(Coordinates(0, 0)));
  EXPECT_FALSE(editor_.isOnWordBoundary(Coordinates(0, 1)));
  EXPECT_TRUE(editor_.isOnWordBoundary(Coordinates(0, 3)));
  EXPECT_TRUE(editor_.isOnWordBoundary(Coordinates(99, 0)));

  LanguageDefinition language;
  language.tokenRegexStrings.push_back({R"(a+)", ColorIndex::Keyword});
  language.tokenRegexStrings.push_back({R"(b+)", ColorIndex::Identifier});
  editor_.setColorizerEnabled(true);
  editor_.setLanguageDefinition(language);
  editor_.colorizeInternal();
  editor_.colorizeInternal();

  EXPECT_TRUE(editor_.isOnWordBoundary(Coordinates(0, 3)));
}

// ---------------------------------------------------------------------------
// Word utilities
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, FindWordStartAndEndBracketContiguousWord) {
  editor_.setText("alpha beta gamma");
  EXPECT_EQ(editor_.findWordStart(Coordinates(0, 8)), Coordinates(0, 6));
  EXPECT_EQ(editor_.findWordEnd(Coordinates(0, 8)), Coordinates(0, 10));
}

TEST_F(TextEditorCoreTests, GetWordAtReturnsContiguousRun) {
  editor_.setText("alpha beta gamma");
  EXPECT_THAT(editor_.getWordAt(Coordinates(0, 8)), Eq("beta"));
}

TEST_F(TextEditorCoreTests, GetWordUnderCursorUsesCharacterBeforeCursor) {
  editor_.setText("alpha beta");
  editor_.setCursorPosition(Coordinates(0, 5));  // cursor right after "alpha"
  EXPECT_THAT(editor_.getWordUnderCursor(), Eq("alpha"));
}

TEST_F(TextEditorCoreTests, FindWordStartOnLinePastEndReturnsInput) {
  const Coordinates past(99, 0);
  EXPECT_EQ(editor_.findWordStart(past), past);
  EXPECT_EQ(editor_.findWordEnd(past), past);
}

TEST_F(TextEditorCoreTests, FindNextWordSkipsToNextAlphanumericRun) {
  editor_.setText("foo  bar");
  EXPECT_EQ(editor_.findNextWord(Coordinates(0, 0)), Coordinates(0, 5));
}

TEST_F(TextEditorCoreTests, WordUtilitiesClassifyPunctuationWhitespaceAndEndSentinel) {
  editor_.setText("foo !!!\n   ");
  editor_.setColorizerEnabled(false);

  EXPECT_THAT(editor_.getWordAt(Coordinates(0, 5)), Eq("!!!"));
  EXPECT_THAT(editor_.getWordAt(Coordinates(0, 3)), Eq(" "));
  EXPECT_EQ(editor_.findNextWord(Coordinates(0, 3)), Coordinates(1, 3));
}

TEST_F(TextEditorCoreTests, FindWordStartTreatsUtf8ContinuationBytesAsWordCharacters) {
  editor_.setText(std::string("a\xA9z"));
  editor_.setColorizerEnabled(false);

  EXPECT_EQ(editor_.findWordStart(Coordinates(0, 1)), Coordinates(0, 0));
}

TEST_F(TextEditorCoreTests, SelectWordUnderCursorSelectsWholeWord) {
  editor_.setText("foo bar baz");
  editor_.setColorizerEnabled(false);
  editor_.setCursorPosition(Coordinates(0, 5));
  editor_.selectWordUnderCursor();
  EXPECT_THAT(editor_.getSelectedText(), Eq("bar"));
}

TEST_F(TextEditorCoreTests, FindFirstReturnsSentinelWhenAbsent) {
  editor_.setText("alpha\nbeta");
  editor_.setColorizerEnabled(false);
  EXPECT_EQ(editor_.findFirst("gamma", Coordinates(0, 0)), Coordinates(2, 0));
}

TEST_F(TextEditorCoreTests, FindFirstOutOfRangeStartReturnsEndSentinel) {
  EXPECT_EQ(editor_.findFirst("0", Coordinates(99, 0)), Coordinates(5, 0));
}

TEST_F(TextEditorCoreTests, FindFirstSkipsSubstringAndReportsWordBoundaryMatch) {
  editor_.setText("foo\nbar alpha\nbaz");
  editor_.setColorizerEnabled(false);

  EXPECT_EQ(editor_.findFirst("alpha", Coordinates(0, 1)), Coordinates(1, 4));
  EXPECT_EQ(editor_.findFirst("baz", Coordinates(1, 5)), Coordinates(2, 0));
}

TEST_F(TextEditorCoreTests, FindFirstSkipsPrefixSubstringBeforeWordMatch) {
  editor_.setText("alphabet alpha");
  editor_.setColorizerEnabled(false);

  EXPECT_EQ(editor_.findFirst("alpha", Coordinates(0, 0)), Coordinates(0, 9));
}

// ---------------------------------------------------------------------------
// Navigation: arrow keys, home/end, top/bottom
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, MoveRightWrapsToNextLine) {
  editor_.setCursorPosition(Coordinates(0, 10));  // end of line 0
  editor_.moveRight();
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(1, 0));
}

TEST_F(TextEditorCoreTests, MoveLeftWrapsToEndOfPreviousLine) {
  editor_.setCursorPosition(Coordinates(1, 0));
  editor_.moveLeft();
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 10));
}

TEST_F(TextEditorCoreTests, MoveLeftAtBufferStartIsClamped) {
  editor_.setCursorPosition(Coordinates(0, 0));
  editor_.moveLeft();
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 0));
}

TEST_F(TextEditorCoreTests, MoveRightAtBufferEndIsClamped) {
  editor_.setCursorPosition(Coordinates(4, 10));
  editor_.moveRight();
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(4, 10));
}

TEST_F(TextEditorCoreTests, MoveUpClampsAtTopLine) {
  editor_.setCursorPosition(Coordinates(0, 4));
  editor_.moveUp();
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 4));
}

TEST_F(TextEditorCoreTests, MoveDownClampsAtBottomLine) {
  editor_.setCursorPosition(Coordinates(4, 4));
  editor_.moveDown();
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(4, 4));
}

TEST_F(TextEditorCoreTests, MoveHomeAndEndJumpToLineExtents) {
  editor_.setCursorPosition(Coordinates(2, 5));
  editor_.moveHome();
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(2, 0));
  editor_.moveEnd();
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(2, 10));
}

TEST_F(TextEditorCoreTests, MoveTopAndBottomJumpToBufferExtents) {
  editor_.setCursorPosition(Coordinates(2, 5));
  editor_.moveBottom();
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(4, 0));
  editor_.moveTop();
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 0));
}

TEST_F(TextEditorCoreTests, ShiftMoveRightExtendsSelection) {
  editor_.setCursorPosition(Coordinates(1, 2));
  editor_.interactiveStart() = Coordinates(1, 2);
  editor_.interactiveEnd() = Coordinates(1, 2);
  editor_.moveRight(3, /*select=*/true);
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(1, 2));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(1, 5));
  EXPECT_THAT(editor_.getSelectedText(), Eq("234"));
}

TEST_F(TextEditorCoreTests, ShiftMoveRightUpdatesInteractiveStartWhenAnchorIsAtEnd) {
  editor_.setCursorPosition(Coordinates(1, 2));
  editor_.interactiveStart() = Coordinates(1, 0);
  editor_.interactiveEnd() = Coordinates(1, 2);

  editor_.moveRight(2, /*select=*/true);

  EXPECT_EQ(editor_.interactiveStart(), Coordinates(1, 0));
  EXPECT_EQ(editor_.interactiveEnd(), Coordinates(1, 4));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(1, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(1, 4));
}

TEST_F(TextEditorCoreTests, ShiftMoveDownThenUpContractsSelection) {
  editor_.setCursorPosition(Coordinates(1, 0));
  editor_.interactiveStart() = Coordinates(1, 0);
  editor_.interactiveEnd() = Coordinates(1, 0);
  editor_.moveDown(2, /*select=*/true);
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(3, 0));
  editor_.moveUp(1, /*select=*/true);
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 0));
}

TEST_F(TextEditorCoreTests, ShiftMoveUpUpdatesAnchorFromEitherSelectionSide) {
  editor_.setCursorPosition(Coordinates(2, 0));
  editor_.interactiveStart() = Coordinates(2, 0);
  editor_.interactiveEnd() = Coordinates(3, 0);

  editor_.moveUp(1, /*select=*/true);

  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(1, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(3, 0));

  editor_.setCursorPosition(Coordinates(2, 0));
  editor_.interactiveStart() = Coordinates(1, 0);
  editor_.interactiveEnd() = Coordinates(2, 0);

  editor_.moveUp(1, /*select=*/true);

  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(1, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(1, 0));
}

TEST_F(TextEditorCoreTests, ShiftMoveDownUpdatesAnchorFromEitherSelectionSide) {
  editor_.setCursorPosition(Coordinates(2, 0));
  editor_.interactiveStart() = Coordinates(1, 0);
  editor_.interactiveEnd() = Coordinates(2, 0);

  editor_.moveDown(1, /*select=*/true);

  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(1, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(3, 0));

  editor_.setCursorPosition(Coordinates(2, 0));
  editor_.interactiveStart() = Coordinates(2, 0);
  editor_.interactiveEnd() = Coordinates(3, 0);

  editor_.moveDown(1, /*select=*/true);

  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(3, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(3, 0));
}

TEST_F(TextEditorCoreTests, ShiftHorizontalMovesHandleWrappedAndUnanchoredSelections) {
  editor_.setCursorPosition(Coordinates(1, 0));
  editor_.interactiveStart() = Coordinates(0, 0);
  editor_.interactiveEnd() = Coordinates(4, 0);

  editor_.moveLeft(1, /*select=*/true);

  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 10));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(1, 0));

  editor_.setCursorPosition(Coordinates(0, 10));
  editor_.interactiveStart() = Coordinates(0, 0);
  editor_.interactiveEnd() = Coordinates(4, 0);

  editor_.moveRight(1, /*select=*/true);

  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 10));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(1, 0));
}

TEST_F(TextEditorCoreTests, ShiftMoveTopAndBottomCreateFullRangeSelection) {
  editor_.setCursorPosition(Coordinates(2, 4));

  editor_.moveTop(/*select=*/true);
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 4));

  editor_.setCursorPosition(Coordinates(1, 3));
  editor_.moveBottom(/*select=*/true);
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(1, 3));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(4, 0));
}

TEST_F(TextEditorCoreTests, ShiftHomeAndEndExtendFromInteractiveSide) {
  editor_.setCursorPosition(Coordinates(2, 5));
  editor_.interactiveStart() = Coordinates(2, 2);
  editor_.interactiveEnd() = Coordinates(2, 5);

  editor_.moveHome(/*select=*/true);
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(2, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 2));

  editor_.setCursorPosition(Coordinates(2, 5));
  editor_.interactiveStart() = Coordinates(2, 5);
  editor_.interactiveEnd() = Coordinates(2, 7);
  editor_.moveEnd(/*select=*/true);
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(2, 7));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 10));
}

TEST_F(TextEditorCoreTests, ShiftHomeAndEndResetUnanchoredSelectionAroundOldCursor) {
  editor_.setCursorPosition(Coordinates(2, 5));
  editor_.interactiveStart() = Coordinates(1, 0);
  editor_.interactiveEnd() = Coordinates(3, 0);

  editor_.moveHome(/*select=*/true);

  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(2, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 5));

  editor_.setCursorPosition(Coordinates(2, 5));
  editor_.interactiveStart() = Coordinates(1, 0);
  editor_.interactiveEnd() = Coordinates(3, 0);

  editor_.moveEnd(/*select=*/true);

  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(2, 5));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 10));
}

TEST_F(TextEditorCoreTests, ShiftMovesUpdateBothInteractiveSelectionAnchors) {
  editor_.setText("abcdef");

  editor_.setCursorPosition(Coordinates(0, 2));
  editor_.interactiveStart() = Coordinates(0, 2);
  editor_.interactiveEnd() = Coordinates(0, 4);
  editor_.moveLeft(1, /*select=*/true);
  EXPECT_EQ(editor_.interactiveStart(), Coordinates(0, 1));
  EXPECT_EQ(editor_.interactiveEnd(), Coordinates(0, 4));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 1));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(0, 4));

  editor_.setCursorPosition(Coordinates(0, 2));
  editor_.interactiveStart() = Coordinates(0, 2);
  editor_.interactiveEnd() = Coordinates(0, 4);
  editor_.moveRight(1, /*select=*/true);
  EXPECT_EQ(editor_.interactiveStart(), Coordinates(0, 3));
  EXPECT_EQ(editor_.interactiveEnd(), Coordinates(0, 4));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 3));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(0, 4));

  editor_.setCursorPosition(Coordinates(0, 3));
  editor_.interactiveStart() = Coordinates(0, 3);
  editor_.interactiveEnd() = Coordinates(0, 5);
  editor_.moveHome(/*select=*/true);
  EXPECT_EQ(editor_.interactiveStart(), Coordinates(0, 0));
  EXPECT_EQ(editor_.interactiveEnd(), Coordinates(0, 5));

  editor_.setCursorPosition(Coordinates(0, 3));
  editor_.interactiveStart() = Coordinates(0, 1);
  editor_.interactiveEnd() = Coordinates(0, 3);
  editor_.moveEnd(/*select=*/true);
  EXPECT_EQ(editor_.interactiveStart(), Coordinates(0, 1));
  EXPECT_EQ(editor_.interactiveEnd(), Coordinates(0, 6));
}

// ---------------------------------------------------------------------------
// Insertion (multi-line paste, indent)
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, InsertMultiLineTextSplitsBuffer) {
  editor_.setText("ab");
  editor_.setCursorPosition(Coordinates(0, 1));
  editor_.insertText("X\nY\nZ");
  EXPECT_THAT(editor_.getText(), Eq("aX\nY\nZb"));
  EXPECT_EQ(editor_.buffer().getTotalLines(), 3);
}

TEST_F(TextEditorCoreTests, InsertTextAddsScrollbarMarkerOncePerLine) {
  editor_.setText("abc");
  editor_.setScrollbarMarkers(true);
  editor_.changedLines() = {0};
  editor_.setCursorPosition(Coordinates(0, 1));

  editor_.insertText("X");

  EXPECT_THAT(editor_.getText(), Eq("aXbc"));
  EXPECT_THAT(editor_.changedLines(), ElementsAre(0));
}

TEST_F(TextEditorCoreTests, InsertEmptyTextWithNoSelectionIsNoOp) {
  editor_.setText("hello");
  editor_.resetTextChanged();
  editor_.insertText("");
  EXPECT_THAT(editor_.getText(), Eq("hello"));
  EXPECT_FALSE(editor_.canUndo());
}

TEST_F(TextEditorCoreTests, InsertEmptyTextWithSelectionDeletesSelection) {
  editor_.setText("hello world");
  editor_.setSelection(Coordinates(0, 5), Coordinates(0, 11));
  editor_.insertText("");
  EXPECT_THAT(editor_.getText(), Eq("hello"));
  EXPECT_TRUE(editor_.canUndo());
}

TEST_F(TextEditorCoreTests, EnterCharacterInsertsAndAdvancesCursor) {
  editor_.setText("ac");
  editor_.setCompleteBraces(false);
  editor_.setCursorPosition(Coordinates(0, 1));
  editor_.enterCharacter('b', false);
  EXPECT_THAT(editor_.getText(), Eq("abc"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 2));
}

TEST_F(TextEditorCoreTests, EnterUnicodeCharactersWritesUtf8AndInvalidCodepointFallback) {
  editor_.setText("");
  editor_.setCompleteBraces(false);
  editor_.setCursorPosition(Coordinates(0, 0));

  editor_.enterCharacter(static_cast<char32_t>(0x00E9), false);
  editor_.enterCharacter(static_cast<char32_t>(0x20AC), false);
  editor_.enterCharacter(static_cast<char32_t>(0x1F600), false);
  editor_.enterCharacter(static_cast<char32_t>(0x110000), false);

  EXPECT_THAT(editor_.getText(), Eq(std::string("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80?")));
}

TEST_F(TextEditorCoreTests, EnterNewlineAutoIndentsFollowingLine) {
  editor_.setText("  foo");
  editor_.setCompleteBraces(false);
  editor_.setSmartIndent(true);
  editor_.setCursorPosition(Coordinates(0, 5));  // end of "  foo"
  editor_.enterCharacter('\n', false);
  // The new line inherits the two leading spaces.
  EXPECT_THAT(editor_.getText(), Eq("  foo\n  "));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(1, 2));
}

TEST_F(TextEditorCoreTests, EnterWithSmartIndentDoesNotReadFreedLineStorage) {
  // Regression: handleNewLine() held a `Line&` into `lines_` (a std::vector<Line>) across
  // insertLine(), whose `lines_.insert()` can reallocate the vector - dangling the reference the
  // auto-indent loop then reads. Under ASan this is a heap-use-after-free; in release it can
  // segfault on Enter with the default smartIndent=true. The bug only fires when the insert
  // actually reallocates, so press Enter many times: each auto-indented line keeps the cursor at
  // line-end, so every Enter re-runs the auto-indent read, and `lines_` reallocates at each power
  // of two - guaranteeing the dangling read is exercised within the loop.
  editor_.setText("    x");
  editor_.setCompleteBraces(false);
  editor_.setSmartIndent(true);
  editor_.setCursorPosition(Coordinates(0, 5));  // end of "    x"
  for (int i = 0; i < 256; ++i) {
    editor_.enterCharacter('\n', false);
  }
  // Every inserted line inherited the 4-space indent, and the run did not read freed storage.
  EXPECT_THAT(editor_.getText(Coordinates(256, 0), Coordinates(256, 4)), Eq("    "));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(256, 4));
}

TEST_F(TextEditorCoreTests, EnterBraceCompletesClosingBrace) {
  editor_.setText("");
  editor_.setCompleteBraces(true);
  editor_.setCursorPosition(Coordinates(0, 0));
  editor_.enterCharacter('(', false);
  EXPECT_THAT(editor_.getText(), Eq("()"));
  // Cursor lands between the braces.
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 1));
}

TEST_F(TextEditorCoreTests, EnterBracketCompletesClosingBracket) {
  editor_.setText("");
  editor_.setCompleteBraces(true);
  editor_.setCursorPosition(Coordinates(0, 0));

  editor_.enterCharacter('[', false);

  EXPECT_THAT(editor_.getText(), Eq("[]"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 1));
}

TEST_F(TextEditorCoreTests, EnterTabInsertsSpacesWhenInsertSpacesEnabled) {
  editor_.setText("");
  editor_.setCompleteBraces(false);
  editor_.setInsertSpaces(true);
  editor_.setTabSize(4);
  editor_.setCursorPosition(Coordinates(0, 0));
  editor_.enterCharacter('\t', false);
  EXPECT_THAT(editor_.getText(), Eq("    "));
}

TEST_F(TextEditorCoreTests, EnterTabCanInsertLiteralTab) {
  editor_.setText("");
  editor_.setCompleteBraces(false);
  editor_.setInsertSpaces(false);
  editor_.setCursorPosition(Coordinates(0, 0));

  editor_.enterCharacter('\t', false);

  EXPECT_THAT(editor_.getText(), Eq("\t"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 1));
}

TEST_F(TextEditorCoreTests, RegularCharacterHelperCanExpandTabUtf8Payload) {
  editor_.setText("x");
  editor_.setInsertSpaces(true);
  editor_.setTabSize(3);

  UndoState state;
  editor_.handleRegularCharacter(state, Coordinates(0, 0), '\t');

  EXPECT_THAT(editor_.getText(), Eq("   x"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 3));
  EXPECT_EQ(state.record.added, "   ");
}

TEST_F(TextEditorCoreTests, InsertTextWithIndentUnindentsClosingBrace) {
  editor_.setText("    {tail");
  editor_.resetTextChanged();
  editor_.setCursorPosition(Coordinates(0, 5));

  editor_.insertText("\n}", /*indent=*/true);

  EXPECT_THAT(editor_.getText(), Eq("    {\n  }tail"));
  const std::vector<SourceEditIntent> intents = editor_.takePendingSourceEditIntents();
  EXPECT_THAT(
      intents,
      ElementsAre(AllOf(SourceEditIntentKindIs(SourceEditIntentKind::Insert),
                        Field("replacement", &SourceEditIntent::replacement, std::string("\n}")))));
}

TEST_F(TextEditorCoreTests, OpeningBraceCompletionCreatesIndentedClosingBrace) {
  editor_.setText("  ");
  editor_.setCompleteBraces(true);
  editor_.setSmartIndent(true);
  editor_.setCursorPosition(Coordinates(0, 2));

  editor_.enterCharacter('{', false);

  EXPECT_THAT(editor_.getText(), Eq("  {\n  }"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(1, 2));
}

TEST_F(TextEditorCoreTests, InsertLineShiftsFoldMarkersOnSplitColumn) {
  editor_.setText("{\t}\nnext");
  editor_.foldBegin() = {Coordinates(0, 0), Coordinates(1, 0)};
  editor_.foldEnd() = {Coordinates(0, 2), Coordinates(1, 4)};

  editor_.insertLine(1, 1);

  EXPECT_THAT(editor_.foldBegin(), ElementsAre(Coordinates(0, 0), Coordinates(2, 0)));
  EXPECT_THAT(editor_.foldEnd(), ElementsAre(Coordinates(1, 2), Coordinates(2, 4)));
}

TEST_F(TextEditorCoreTests, RemoveFoldsUpdatesColumnsAcrossTabsAndJoinedLines) {
  editor_.setText("{\tfoo\n  bar}\nkeep");
  editor_.setTabSize(4);
  editor_.foldBegin() = {Coordinates(0, 0), Coordinates(0, 2), Coordinates(2, 0)};
  editor_.foldEnd() = {Coordinates(1, 6), Coordinates(1, 6), Coordinates(2, 4)};

  editor_.removeFolds(Coordinates(0, 1), Coordinates(1, 2));

  EXPECT_FALSE(editor_.foldSorted());
  ASSERT_EQ(editor_.foldBegin().size(), 2u);
  EXPECT_EQ(editor_.foldBegin()[0], Coordinates(0, 0));
  EXPECT_EQ(editor_.foldBegin()[1], Coordinates(1, 0));
}

// ---------------------------------------------------------------------------
// Deletion: backspace, delete, range, selection
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, BackspaceMidLineRemovesPreviousCharacter) {
  editor_.setText("abc");
  editor_.setCursorPosition(Coordinates(0, 2));
  editor_.backspace();
  EXPECT_THAT(editor_.getText(), Eq("ac"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 1));
}

TEST_F(TextEditorCoreTests, BackspaceAtLineStartMergesWithPreviousLine) {
  editor_.setText("ab\ncd");
  editor_.setCursorPosition(Coordinates(1, 0));
  editor_.backspace();
  EXPECT_THAT(editor_.getText(), Eq("abcd"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 2));
}

TEST_F(TextEditorCoreTests, BackspaceRemovesFullIndentUnit) {
  editor_.setText("    x");
  editor_.setInsertSpaces(true);
  editor_.setTabSize(4);
  editor_.setCursorPosition(Coordinates(0, 4));
  editor_.backspace();
  // A full tab-stop of spaces is removed at once.
  EXPECT_THAT(editor_.getText(), Eq("x"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 0));
}

TEST_F(TextEditorCoreTests, BackspaceRemovesLiteralTabWhenInsertSpacesDisabled) {
  editor_.setText("\tx");
  editor_.setInsertSpaces(false);
  editor_.setCursorPosition(Coordinates(0, 2));

  editor_.backspace();

  EXPECT_THAT(editor_.getText(), Eq("x"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 1));
}

TEST_F(TextEditorCoreTests, BackspaceAtLiteralTabColumnRemovesTheTab) {
  editor_.setText("\t");
  editor_.setInsertSpaces(false);
  editor_.setCursorPosition(Coordinates(0, 1));

  editor_.backspace();

  EXPECT_THAT(editor_.getText(), IsEmpty());
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 0));
}

TEST_F(TextEditorCoreTests, DeleteMidLineRemovesCharacterUnderCursor) {
  editor_.setText("abc");
  editor_.setCursorPosition(Coordinates(0, 1));
  editor_.delete_();
  EXPECT_THAT(editor_.getText(), Eq("ac"));
}

TEST_F(TextEditorCoreTests, DeleteAtLineEndMergesNextLine) {
  editor_.setText("ab\ncd");
  editor_.setCursorPosition(Coordinates(0, 2));
  editor_.delete_();
  EXPECT_THAT(editor_.getText(), Eq("abcd"));
}

TEST_F(TextEditorCoreTests, DeleteAtBufferEndIsNoOp) {
  editor_.setText("ab");
  editor_.setCursorPosition(Coordinates(0, 2));
  editor_.delete_();
  EXPECT_THAT(editor_.getText(), Eq("ab"));
}

TEST_F(TextEditorCoreTests, DeleteRangeRemovesAcrossLines) {
  editor_.deleteRange(Coordinates(1, 2), Coordinates(3, 4));
  // Lines 1..3 collapse to "01" (line 1, cols 0-1) + "456789" (line 3, cols 4-9).
  EXPECT_THAT(editor_.getText(), Eq("0123456789\n01456789\n0123456789"));
}

TEST_F(TextEditorCoreTests, DeleteRangeUpdatesScrollbarMarkers) {
  editor_.setScrollbarMarkers(true);
  editor_.changedLines() = {0, 1, 2, 4};

  editor_.deleteRange(Coordinates(1, 0), Coordinates(3, 0));

  EXPECT_THAT(editor_.changedLines(), ElementsAre(0, 1, 2));
}

TEST_F(TextEditorCoreTests, DeleteRangeKeepsInteriorChangedLineRemapStable) {
  editor_.setScrollbarMarkers(true);
  editor_.changedLines() = {0, 2, 3, 4};

  editor_.deleteRange(Coordinates(1, 2), Coordinates(3, 4));

  EXPECT_THAT(editor_.changedLines(), ElementsAre(0, 3, 2));
}

TEST_F(TextEditorCoreTests, DeleteSelectionRemovesAndCollapsesCursor) {
  editor_.setSelection(Coordinates(1, 2), Coordinates(2, 4));
  editor_.deleteSelection();
  EXPECT_THAT(editor_.getText(), Eq("0123456789\n01456789\n0123456789\n0123456789"));
  EXPECT_FALSE(editor_.hasSelection());
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(1, 2));
}

TEST_F(TextEditorCoreTests, BackspaceWithSelectionDeletesSelection) {
  editor_.setSelection(Coordinates(0, 2), Coordinates(0, 5));
  editor_.backspace();
  EXPECT_THAT(editor_.getText(), Eq("0156789\n0123456789\n0123456789\n0123456789\n0123456789"));
}

TEST_F(TextEditorCoreTests, DeleteWithSelectionCapturesDeleteIntent) {
  editor_.resetTextChanged();
  editor_.setSelection(Coordinates(0, 2), Coordinates(0, 5));

  editor_.delete_();

  EXPECT_THAT(editor_.getText(), Eq("0156789\n0123456789\n0123456789\n0123456789\n0123456789"));
  const std::vector<SourceEditIntent> intents = editor_.takePendingSourceEditIntents();
  EXPECT_THAT(intents, ElementsAre(AllOf(
                           SourceEditIntentKindIs(SourceEditIntentKind::Delete),
                           Field("removedLength", &SourceEditIntent::removedLength, 3u),
                           Field("replacement", &SourceEditIntent::replacement, std::string("")))));
}

TEST_F(TextEditorCoreTests, BackspaceAndDeleteRemoveWholeUtf8Clusters) {
  editor_.setText(std::string("a\xC3\xA9z"));
  editor_.setCompleteBraces(false);

  editor_.setCursorPosition(Coordinates(0, 2));
  editor_.backspace();
  EXPECT_THAT(editor_.getText(), Eq("az"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 1));

  editor_.setText(std::string("a\xF0\x9F\x98\x80z"));
  editor_.setCursorPosition(Coordinates(0, 1));
  editor_.delete_();
  EXPECT_THAT(editor_.getText(), Eq("az"));
}

TEST_F(TextEditorCoreTests, BackspaceAtLineStartMergesErrorMarkers) {
  editor_.setText("a\nb\nc");
  editor_.setErrorMarkers(ErrorMarkers{{0, "first"}, {2, "last"}});
  editor_.setCursorPosition(Coordinates(1, 0));

  editor_.backspace();

  EXPECT_THAT(editor_.getText(), Eq("ab\nc"));
  EXPECT_THAT(editor_.getErrorMarkers(),
              ElementsAre(testing::Pair(0, "first"), testing::Pair(1, "last")));
}

TEST_F(TextEditorCoreTests, AdvanceStepsUtf8AndWrapsPastLineEnd) {
  editor_.setText(std::string("\xC3\xA9x\nz"));

  Coordinates coords(0, 0);
  editor_.advance(coords);
  EXPECT_EQ(coords, Coordinates(0, 1));

  coords = Coordinates(0, 2);
  editor_.advance(coords);
  EXPECT_EQ(coords, Coordinates(1, 0));
}

TEST_F(TextEditorCoreTests, InsertAndRemoveLineMaintainFoldAndMarkerBookkeeping) {
  editor_.setText("a{\nb\n}\nd");
  editor_.foldBegin() = {Coordinates(0, 1), Coordinates(1, 0)};
  editor_.foldEnd() = {Coordinates(2, 0)};
  editor_.foldSorted() = true;
  editor_.setErrorMarkers(ErrorMarkers{{0, "first"}, {2, "third"}});

  editor_.insertLine(1, 0);

  EXPECT_THAT(editor_.foldBegin(), ElementsAre(Coordinates(1, 1), Coordinates(2, 0)));
  EXPECT_THAT(editor_.foldEnd(), ElementsAre(Coordinates(3, 0)));
  ASSERT_TRUE(editor_.getErrorMarkers().contains(3));

  editor_.removeLine(1);

  EXPECT_THAT(editor_.foldBegin(), ElementsAre(Coordinates(1, 0)));
  EXPECT_THAT(editor_.foldEnd(), ElementsAre(Coordinates(2, 0)));
  ASSERT_TRUE(editor_.getErrorMarkers().contains(0));
}

TEST_F(TextEditorCoreTests, RemoveLineRangeDropsInteriorMarkersAndShiftsFollowingLines) {
  editor_.setText("a\nb\nc\nd\ne");
  editor_.setScrollbarMarkers(true);
  editor_.changedLines() = {0, 1, 3, 4};
  editor_.setErrorMarkers(ErrorMarkers{{0, "zero"}, {1, "one"}, {3, "three"}, {4, "four"}});

  editor_.removeLine(1, 3);

  EXPECT_THAT(editor_.getText(), Eq("a\ne"));
  EXPECT_THAT(editor_.changedLines(), ElementsAre(0, 1));
  EXPECT_THAT(editor_.getErrorMarkers(),
              ElementsAre(testing::Pair(0, "zero"), testing::Pair(1, "four")));
}

TEST_F(TextEditorCoreTests, RemoveSingleLineDropsAndShiftsScrollbarMarkers) {
  editor_.setText("a\nb\nc\nd");
  editor_.setScrollbarMarkers(true);
  editor_.changedLines() = {0, 1, 3};

  editor_.removeLine(1);

  EXPECT_THAT(editor_.getText(), Eq("a\nc\nd"));
  EXPECT_THAT(editor_.changedLines(), ElementsAre(0, 2));
}

TEST_F(TextEditorCoreTests, RemoveFoldsShiftsFoldsAfterDeletedFullLines) {
  editor_.setText("a\nb\nc\nd");
  editor_.foldBegin() = {Coordinates(0, 0), Coordinates(3, 0)};
  editor_.foldEnd() = {Coordinates(0, 0), Coordinates(3, 0)};
  editor_.foldSorted() = true;

  editor_.removeFolds(Coordinates(1, 0), Coordinates(2, 100000));

  EXPECT_THAT(editor_.foldBegin(), ElementsAre(Coordinates(0, 0), Coordinates(1, 0)));
  EXPECT_THAT(editor_.foldEnd(), ElementsAre(Coordinates(0, 0), Coordinates(1, 0)));
  EXPECT_TRUE(editor_.foldSorted());
}

TEST_F(TextEditorCoreTests, RemoveFoldsSameLineShiftsFoldAfterDeletedColumns) {
  editor_.setText("abcdef");
  editor_.foldBegin() = {Coordinates(0, 5)};
  editor_.foldEnd() = {Coordinates(0, 5)};
  editor_.foldSorted() = true;

  editor_.removeFolds(Coordinates(0, 1), Coordinates(0, 3));

  EXPECT_THAT(editor_.foldBegin(), ElementsAre(Coordinates(0, 3)));
  EXPECT_THAT(editor_.foldEnd(), ElementsAre(Coordinates(0, 3)));
}

TEST_F(TextEditorCoreTests, RemoveFoldsKeepsFoldBeforeSameLineDeletedRange) {
  editor_.setText("abcdef");
  editor_.foldBegin() = {Coordinates(0, 0), Coordinates(0, 5)};
  editor_.foldEnd() = {Coordinates(0, 0), Coordinates(0, 5)};
  editor_.foldSorted() = true;

  editor_.removeFolds(Coordinates(0, 2), Coordinates(0, 4));

  EXPECT_THAT(editor_.foldBegin(), ElementsAre(Coordinates(0, 0), Coordinates(0, 3)));
  EXPECT_THAT(editor_.foldEnd(), ElementsAre(Coordinates(0, 0), Coordinates(0, 3)));
}

TEST_F(TextEditorCoreTests, RemoveFoldsMultiLineKeepsStartLineFoldBeforeDeletedColumns) {
  editor_.setText("abcdef\nsecond\nthird");
  editor_.foldBegin() = {Coordinates(0, 1), Coordinates(0, 4), Coordinates(2, 0)};
  editor_.foldEnd() = {Coordinates(0, 1), Coordinates(0, 4), Coordinates(2, 0)};
  editor_.foldSorted() = true;

  editor_.removeFolds(Coordinates(0, 3), Coordinates(1, 2));

  EXPECT_THAT(editor_.foldBegin(), ElementsAre(Coordinates(0, 1), Coordinates(1, 0)));
  EXPECT_THAT(editor_.foldEnd(), ElementsAre(Coordinates(0, 1), Coordinates(1, 0)));
  EXPECT_FALSE(editor_.foldSorted());
}

TEST_F(TextEditorCoreTests, RemoveFoldsMultiLineRecomputesFoldAfterEndLineColumn) {
  editor_.setText("abcde\nWXYZ\nlast");
  editor_.foldBegin() = {Coordinates(1, 4)};
  editor_.foldEnd() = {Coordinates(1, 4)};
  editor_.foldSorted() = true;

  editor_.removeFolds(Coordinates(0, 2), Coordinates(1, 2));

  EXPECT_THAT(editor_.foldBegin(), ElementsAre(Coordinates(0, 7)));
  EXPECT_THAT(editor_.foldEnd(), ElementsAre(Coordinates(0, 7)));
  EXPECT_TRUE(editor_.foldSorted());
}

TEST_F(TextEditorCoreTests, RemoveFoldsMultiLineErasesEndLineFoldBeforeEndColumn) {
  editor_.setText("abcde\nWXYZ\nlast");
  editor_.foldBegin() = {Coordinates(1, 1), Coordinates(1, 3)};
  editor_.foldEnd() = {Coordinates(1, 1), Coordinates(1, 3)};
  editor_.foldSorted() = true;

  editor_.removeFolds(Coordinates(0, 2), Coordinates(1, 2));

  EXPECT_THAT(editor_.foldBegin(), ElementsAre(Coordinates(0, 5)));
  EXPECT_THAT(editor_.foldEnd(), ElementsAre(Coordinates(0, 6)));
  EXPECT_FALSE(editor_.foldSorted());
}

TEST_F(TextEditorCoreTests, RemoveFoldsOutOfRangeStartLeavesRecomputedColumnUnchanged) {
  editor_.setText("only");
  editor_.foldBegin() = {Coordinates(3, 2)};
  editor_.foldEnd() = {Coordinates(3, 2)};
  editor_.foldSorted() = true;

  editor_.removeFolds(Coordinates(2, 0), Coordinates(3, 1));

  EXPECT_THAT(editor_.foldBegin(), ElementsAre(Coordinates(2, 2)));
  EXPECT_THAT(editor_.foldEnd(), ElementsAre(Coordinates(2, 2)));
  EXPECT_TRUE(editor_.foldSorted());
}

TEST_F(TextEditorCoreTests, RemoveFoldsErasesMarkersStrictlyInsideDeletedLineRange) {
  editor_.setText("a\nb\nc\nd");
  editor_.foldBegin() = {Coordinates(0, 0), Coordinates(2, 0), Coordinates(3, 0)};
  editor_.foldEnd() = {Coordinates(0, 0), Coordinates(2, 1), Coordinates(3, 0)};
  editor_.foldSorted() = true;

  editor_.removeFolds(Coordinates(1, 0), Coordinates(3, 0));

  EXPECT_THAT(editor_.foldBegin(), ElementsAre(Coordinates(0, 0), Coordinates(1, 2)));
  EXPECT_THAT(editor_.foldEnd(), ElementsAre(Coordinates(0, 0), Coordinates(1, 1)));
  EXPECT_FALSE(editor_.foldSorted());
}

// ---------------------------------------------------------------------------
// Undo / redo across compound edits
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, UndoRestoresInsertedTextAndCursor) {
  editor_.setText("ac");
  editor_.setCompleteBraces(false);
  editor_.setCursorPosition(Coordinates(0, 1));
  editor_.enterCharacter('b', false);
  ASSERT_THAT(editor_.getText(), Eq("abc"));

  ASSERT_TRUE(editor_.canUndo());
  editor_.undo();
  EXPECT_THAT(editor_.getText(), Eq("ac"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 1));
  EXPECT_TRUE(editor_.canRedo());
}

TEST_F(TextEditorCoreTests, RedoReappliesUndoneInsert) {
  editor_.setText("ac");
  editor_.setCompleteBraces(false);
  editor_.setCursorPosition(Coordinates(0, 1));
  editor_.enterCharacter('b', false);
  editor_.undo();
  editor_.redo();
  EXPECT_THAT(editor_.getText(), Eq("abc"));
}

TEST_F(TextEditorCoreTests, RedoEmitsRedoKindSourceEditIntent) {
  editor_.setText("ac");
  editor_.setCompleteBraces(false);
  editor_.setCursorPosition(Coordinates(0, 1));
  editor_.enterCharacter('b', false);
  editor_.takePendingSourceEditIntents();

  editor_.undo();
  editor_.takePendingSourceEditIntents();
  editor_.redo();

  const std::vector<SourceEditIntent> intents = editor_.takePendingSourceEditIntents();
  EXPECT_THAT(intents, ElementsAre(SourceEditIntentKindIs(SourceEditIntentKind::Redo)));
}

TEST_F(TextEditorCoreTests, UndoReplaceRestoresOriginalSelectionText) {
  editor_.setText("hello world");
  editor_.setSelection(Coordinates(0, 6), Coordinates(0, 11));  // "world"
  editor_.insertText("there");
  ASSERT_THAT(editor_.getText(), Eq("hello there"));

  editor_.undo();
  EXPECT_THAT(editor_.getText(), Eq("hello world"));
  editor_.redo();
  EXPECT_THAT(editor_.getText(), Eq("hello there"));
}

TEST_F(TextEditorCoreTests, MultiStepUndoUnwindsSeveralEditsInOrder) {
  editor_.setText("");
  editor_.setCompleteBraces(false);
  editor_.setCursorPosition(Coordinates(0, 0));
  editor_.insertText("a");
  editor_.insertText("b");
  editor_.insertText("c");
  ASSERT_THAT(editor_.getText(), Eq("abc"));

  editor_.undo(2);
  EXPECT_THAT(editor_.getText(), Eq("a"));
  editor_.redo(2);
  EXPECT_THAT(editor_.getText(), Eq("abc"));
}

TEST_F(TextEditorCoreTests, NewEditAfterUndoTruncatesRedoHistory) {
  editor_.setText("");
  editor_.setCursorPosition(Coordinates(0, 0));
  editor_.insertText("a");
  editor_.insertText("b");
  editor_.undo();  // back to "a", redo available
  ASSERT_TRUE(editor_.canRedo());

  editor_.insertText("c");  // diverges, dropping the "b" redo branch
  EXPECT_THAT(editor_.getText(), Eq("ac"));
  EXPECT_FALSE(editor_.canRedo());
}

TEST_F(TextEditorCoreTests, UndoEmitsUndoKindSourceEditIntent) {
  editor_.setCursorPosition(Coordinates(0, 0));
  editor_.insertText("zzz");
  editor_.takePendingSourceEditIntents();  // drain the insert intent

  editor_.undo();
  std::vector<SourceEditIntent> intents = editor_.takePendingSourceEditIntents();
  EXPECT_THAT(intents, ElementsAre(SourceEditIntentKindIs(SourceEditIntentKind::Undo)));
}

TEST_F(TextEditorCoreTests, UndoWhenEmptyIsNoOp) {
  editor_.setText("abc");
  EXPECT_FALSE(editor_.canUndo());
  editor_.undo();
  editor_.redo();
  EXPECT_THAT(editor_.getText(), Eq("abc"));
}

// ---------------------------------------------------------------------------
// Multi-line tab indent / outdent
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, TabWithMultiLineSelectionIndentsAllLines) {
  editor_.setText("a\nb\nc");
  editor_.setInsertSpaces(true);
  editor_.setTabSize(2);
  editor_.setSelection(Coordinates(0, 0), Coordinates(1, 1));
  editor_.enterCharacter('\t', /*shift=*/false);
  EXPECT_THAT(editor_.getText(), Eq("  a\n  b\nc"));
}

TEST_F(TextEditorCoreTests, TabWithSelectionEndingAtColumnZeroExcludesFollowingLine) {
  editor_.setText("a\nb\nc");
  editor_.setInsertSpaces(true);
  editor_.setTabSize(2);
  editor_.setSelection(Coordinates(0, 0), Coordinates(2, 0));

  editor_.enterCharacter('\t', /*shift=*/false);

  EXPECT_THAT(editor_.getText(), Eq("  a\n  b\nc"));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 0));
}

TEST_F(TextEditorCoreTests, ShiftTabWithMultiLineSelectionOutdents) {
  editor_.setText("  a\n  b\nc");
  editor_.setInsertSpaces(true);
  editor_.setTabSize(2);
  editor_.setSelection(Coordinates(0, 0), Coordinates(1, 2));
  editor_.enterCharacter('\t', /*shift=*/true);
  EXPECT_THAT(editor_.getText(), Eq("a\nb\nc"));
}

TEST_F(TextEditorCoreTests, ShiftTabOutdentsMixedTabsAndSpaces) {
  editor_.setText("\ta\n  b\nc");
  editor_.setInsertSpaces(true);
  editor_.setTabSize(2);
  editor_.setSelection(Coordinates(0, 0), Coordinates(1, 3));

  editor_.enterCharacter('\t', /*shift=*/true);

  EXPECT_THAT(editor_.getText(), Eq("a\nb\nc"));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(1, 1));
}

TEST_F(TextEditorCoreTests, ShiftTabOutdentsPublicReversedMultilineSelection) {
  editor_.setText("  a\n  b\nc");
  editor_.setInsertSpaces(true);
  editor_.setTabSize(2);
  editor_.setSelection(Coordinates(1, 3), Coordinates(0, 0));

  editor_.enterCharacter('\t', /*shift=*/true);

  EXPECT_THAT(editor_.getText(), Eq("a\nb\nc"));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(1, 1));
}

TEST_F(TextEditorCoreTests, MultiLineTabCanInsertLiteralTabs) {
  editor_.setText("a\nb\nc");
  editor_.setInsertSpaces(false);
  editor_.setSelection(Coordinates(0, 0), Coordinates(1, 1));

  editor_.enterCharacter('\t', /*shift=*/false);

  EXPECT_THAT(editor_.getText(), Eq("\ta\n\tb\nc"));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(1, 3));
}

TEST_F(TextEditorCoreTests, ShiftTabNoOpDoesNotCreateUndoRecord) {
  editor_.setText("a\nb");
  editor_.setSelection(Coordinates(0, 0), Coordinates(1, 1));

  editor_.enterCharacter('\t', /*shift=*/true);

  EXPECT_THAT(editor_.getText(), Eq("a\nb"));
  EXPECT_FALSE(editor_.canUndo());
}

TEST_F(TextEditorCoreTests, MultiLineTabHelperClampsOversizedSelectionEnd) {
  editor_.setText("a\nb");
  editor_.setInsertSpaces(true);
  editor_.setTabSize(2);
  editor_.mutableState().selectionStart = Coordinates(0, 0);
  editor_.mutableState().selectionEnd = Coordinates(99, 0);

  UndoState state;
  editor_.handleMultiLineTab(state, /*shift=*/false);

  EXPECT_THAT(editor_.getText(), Eq("  a\n  b"));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(99, 0));
}

TEST_F(TextEditorCoreTests, MultiLineTabHelperHandlesReversedRawSelection) {
  editor_.setText("a\nb");
  editor_.setInsertSpaces(true);
  editor_.setTabSize(2);
  editor_.mutableState().selectionStart = Coordinates(1, 1);
  editor_.mutableState().selectionEnd = Coordinates(0, 1);

  UndoState state;
  editor_.handleMultiLineTab(state, /*shift=*/false);

  EXPECT_THAT(editor_.getText(), Eq("  a\n  b"));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 0));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(1, 3));
}

// ---------------------------------------------------------------------------
// External source edits (applyExternalSourceEdit)
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, ApplyExternalSourceEditReplacesBytesWithoutUndo) {
  editor_.setText("hello world");
  editor_.resetTextChanged();
  editor_.applyExternalSourceEdit(/*offset=*/6, /*removedLength=*/5, "there");
  EXPECT_THAT(editor_.getText(), Eq("hello there"));
  // External edits do not create undo records or pending intents.
  EXPECT_FALSE(editor_.canUndo());
  EXPECT_FALSE(editor_.hasPendingSourceEditIntents());
  EXPECT_FALSE(editor_.isTextChanged());
}

TEST_F(TextEditorCoreTests, ApplyExternalSourceEditShiftsCursorAfterEditPoint) {
  editor_.setText("hello world");
  editor_.setCursorPosition(Coordinates(0, 11));  // end, after the edit
  editor_.applyExternalSourceEdit(/*offset=*/0, /*removedLength=*/0, "XX");
  EXPECT_THAT(editor_.getText(), Eq("XXhello world"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 13));
}

TEST_F(TextEditorCoreTests, ApplyExternalSourceEditHonorsSelectionBoundaryBias) {
  editor_.setText("abcd");
  editor_.setSelection(Coordinates(0, 1), Coordinates(0, 3));
  editor_.setCursorPosition(Coordinates(0, 3));

  editor_.applyExternalSourceEdit(/*offset=*/3, /*removedLength=*/0, "X");

  EXPECT_THAT(editor_.getText(), Eq("abcXd"));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 1));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(0, 4));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 3));
}

TEST_F(TextEditorCoreTests, ApplyExternalSourceEditMapsCursorInsideReplacedRangeToReplacementEnd) {
  editor_.setText("abcdef");
  editor_.setCursorPosition(Coordinates(0, 4));

  editor_.applyExternalSourceEdit(/*offset=*/2, /*removedLength=*/3, "XY");

  EXPECT_THAT(editor_.getText(), Eq("abXYf"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 4));
  EXPECT_TRUE(editor_.cursorPositionChanged());
}

TEST_F(TextEditorCoreTests, ApplyExternalSourceEditShrinksSelectionAfterShorterReplacement) {
  editor_.setText("abcdef");
  editor_.setSelection(Coordinates(0, 1), Coordinates(0, 6));
  editor_.setCursorPosition(Coordinates(0, 6));

  editor_.applyExternalSourceEdit(/*offset=*/2, /*removedLength=*/3, "Q");

  EXPECT_THAT(editor_.getText(), Eq("abQf"));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 1));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(0, 4));
  EXPECT_EQ(editor_.interactiveStart(), Coordinates(0, 1));
  EXPECT_EQ(editor_.interactiveEnd(), Coordinates(0, 4));
}

TEST_F(TextEditorCoreTests, ApplyExternalSourceEditKeepsSelectionBeforeEditPoint) {
  editor_.setText("abcdef");
  editor_.setSelection(Coordinates(0, 1), Coordinates(0, 2));
  editor_.setCursorPosition(Coordinates(0, 2));

  editor_.applyExternalSourceEdit(/*offset=*/4, /*removedLength=*/1, "XYZ");

  EXPECT_THAT(editor_.getText(), Eq("abcdXYZf"));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 1));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(0, 2));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 2));
}

TEST_F(TextEditorCoreTests, ApplyExternalSourceEditCollapsedSelectionUsesBeforeBiasAtInsertion) {
  editor_.setText("abcd");
  editor_.setSelection(Coordinates(0, 2), Coordinates(0, 2));
  editor_.setCursorPosition(Coordinates(0, 2));

  editor_.applyExternalSourceEdit(/*offset=*/2, /*removedLength=*/0, "XX");

  EXPECT_THAT(editor_.getText(), Eq("abXXcd"));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 2));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(0, 2));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 2));
}

TEST_F(TextEditorCoreTests, ApplyExternalSourceEditExpandsSelectionAfterLongerReplacement) {
  editor_.setText("abcdef");
  editor_.setSelection(Coordinates(0, 1), Coordinates(0, 6));
  editor_.setCursorPosition(Coordinates(0, 6));

  editor_.applyExternalSourceEdit(/*offset=*/2, /*removedLength=*/1, "WXYZ");

  EXPECT_THAT(editor_.getText(), Eq("abWXYZdef"));
  EXPECT_EQ(editor_.getSelectionStart(), Coordinates(0, 1));
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(0, 9));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(0, 9));
}

// ---------------------------------------------------------------------------
// Error markers shift with line insert/delete
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, ErrorMarkersShiftDownWhenLineInserted) {
  editor_.setText("a\nb\nc");
  editor_.setErrorMarkers(ErrorMarkers{{2, "msg-on-line-2"}});

  editor_.setCompleteBraces(false);
  editor_.setCursorPosition(Coordinates(0, 0));
  editor_.enterCharacter('\n', false);  // inserts a line before the marker

  const ErrorMarkers& markers = editor_.getErrorMarkers();
  ASSERT_EQ(markers.size(), 1u);
  EXPECT_EQ(markers.begin()->first, 3);
  EXPECT_EQ(markers.begin()->second, "msg-on-line-2");
}

TEST_F(TextEditorCoreTests, ErrorMarkersAccessibleViaMutableRef) {
  editor_.mutableErrorMarkers()[4] = "err";
  EXPECT_THAT(editor_.getErrorMarkers(), ElementsAre(testing::Pair(4, "err")));
}

TEST_F(TextEditorCoreTests, AppendSourceEditIntentDeduplicatesAndInvokesHook) {
  int hookCalls = 0;
  editor_.sourceEditIntentHook = [&](const SourceEditIntent& intent) {
    ++hookCalls;
    EXPECT_EQ(intent.kind, SourceEditIntentKind::Insert);
  };

  SourceEditIntent intent;
  intent.offset = 1;
  intent.removedLength = 0;
  intent.replacement = "x";
  intent.kind = SourceEditIntentKind::Insert;

  editor_.appendSourceEditIntent(intent);
  editor_.appendSourceEditIntent(intent);
  editor_.appendSourceEditIntent(SourceEditIntent{});

  std::vector<SourceEditIntent> intents = editor_.takePendingSourceEditIntents();
  EXPECT_THAT(intents, ElementsAre(AllOf(
                           Field("offset", &SourceEditIntent::offset, 1u),
                           Field("removedLength", &SourceEditIntent::removedLength, 0u),
                           Field("replacement", &SourceEditIntent::replacement, std::string("x")),
                           SourceEditIntentKindIs(SourceEditIntentKind::Insert),
                           Field("bufferVersion", &SourceEditIntent::bufferVersion, 1u))));
  EXPECT_EQ(hookCalls, 1);
}

TEST_F(TextEditorCoreTests, AppendSourceEditIntentKeepsDistinctAdjacentVariants) {
  const auto makeIntent = [](std::size_t offset, std::size_t removedLength, std::string replacement,
                             SourceEditIntentKind kind) {
    SourceEditIntent intent;
    intent.offset = offset;
    intent.removedLength = removedLength;
    intent.replacement = std::move(replacement);
    intent.kind = kind;
    return intent;
  };

  editor_.appendSourceEditIntent(makeIntent(1, 1, "x", SourceEditIntentKind::Replace));
  editor_.appendSourceEditIntent(makeIntent(2, 1, "x", SourceEditIntentKind::Replace));
  editor_.appendSourceEditIntent(makeIntent(2, 2, "x", SourceEditIntentKind::Replace));
  editor_.appendSourceEditIntent(makeIntent(2, 2, "y", SourceEditIntentKind::Replace));
  editor_.appendSourceEditIntent(makeIntent(2, 2, "y", SourceEditIntentKind::Delete));
  editor_.appendSourceEditIntent(makeIntent(2, 2, "y", SourceEditIntentKind::Delete));

  const std::vector<SourceEditIntent> intents = editor_.takePendingSourceEditIntents();
  ASSERT_EQ(intents.size(), 5u);
  EXPECT_EQ(intents[0].bufferVersion, 1u);
  EXPECT_EQ(intents[4].bufferVersion, 5u);
}

TEST_F(TextEditorCoreTests, EditingHooksFireForAutocompleteAndContentUpdates) {
  int autocompleteRequests = 0;
  int contentUpdates = 0;
  int sourceIntentHooks = 0;
  int tooltipHooks = 0;
  editor_.requestAutocompleteHook = [&] { ++autocompleteRequests; };
  editor_.onContentUpdateInternal = [&] { ++contentUpdates; };
  editor_.sourceEditIntentHook = [&](const SourceEditIntent&) { ++sourceIntentHooks; };
  editor_.functionTooltipHook = [&](char32_t character, const Coordinates& position) {
    ++tooltipHooks;
    EXPECT_EQ(character, 'a');
    EXPECT_EQ(position, Coordinates(0, 0));
  };

  editor_.setText("");
  editor_.setCompleteBraces(false);
  editor_.setActiveAutocomplete(true);
  editor_.setCursorPosition(Coordinates(0, 0));
  editor_.enterCharacter('a', false);

  EXPECT_EQ(autocompleteRequests, 1);
  EXPECT_GE(contentUpdates, 1);
  EXPECT_EQ(sourceIntentHooks, 1);
  EXPECT_EQ(tooltipHooks, 1);
}

TEST_F(TextEditorCoreTests, ActiveAutocompleteRequestsForUnderscoreButNotDigitsOrUnicode) {
  int autocompleteRequests = 0;
  editor_.requestAutocompleteHook = [&] { ++autocompleteRequests; };
  editor_.setText("");
  editor_.setCompleteBraces(false);
  editor_.setActiveAutocomplete(true);

  editor_.setCursorPosition(Coordinates(0, 0));
  editor_.enterCharacter('_', false);
  EXPECT_EQ(autocompleteRequests, 1);

  editor_.enterCharacter('7', false);
  editor_.enterCharacter(static_cast<char32_t>(0x00E9), false);
  EXPECT_EQ(autocompleteRequests, 1);
}

TEST_F(TextEditorCoreTests, ActiveAutocompleteRequestsAfterBackspace) {
  int autocompleteRequests = 0;
  editor_.requestAutocompleteHook = [&] { ++autocompleteRequests; };
  editor_.setText("abc");
  editor_.setActiveAutocomplete(true);
  editor_.setCursorPosition(Coordinates(0, 2));

  editor_.backspace();

  EXPECT_THAT(editor_.getText(), Eq("ac"));
  EXPECT_EQ(autocompleteRequests, 1);
}

TEST_F(TextEditorCoreTests, CoreEdgeBranchesHandleNoOpAndBoundaryOperations) {
  Coordinates pastEnd(99, 3);
  editor_.advance(pastEnd);
  EXPECT_EQ(pastEnd, Coordinates(99, 3));

  editor_.setSelection(Coordinates(1, 2), Coordinates(1, 4));
  editor_.setCursorPositionChanged(false);
  editor_.setSelection(Coordinates(1, 2), Coordinates(1, 4));
  EXPECT_FALSE(editor_.cursorPositionChanged());

  editor_.setText("abc");
  editor_.resetTextChanged();
  editor_.setSelection(Coordinates(0, 1), Coordinates(0, 1));
  editor_.deleteSelection();
  EXPECT_THAT(editor_.getText(), Eq("abc"));
  EXPECT_FALSE(editor_.isTextChanged());

  editor_.setCursorPosition(Coordinates(0, 0));
  editor_.backspace();
  EXPECT_THAT(editor_.getText(), Eq("abc"));
}

TEST_F(TextEditorCoreTests, WordAndSearchEdgesHandleBoundaryCoordinates) {
  editor_.setText("alpha end");
  editor_.setColorizerEnabled(false);

  const Coordinates lineEnd(0, editor_.buffer().getLineMaxColumn(0));
  EXPECT_EQ(editor_.findWordStart(lineEnd), lineEnd);
  EXPECT_EQ(editor_.findWordEnd(lineEnd), lineEnd);
  EXPECT_TRUE(editor_.isOnWordBoundary(lineEnd));
  EXPECT_EQ(editor_.findNextWord(Coordinates(99, 0)), Coordinates(99, 0));

  Coordinates negativeStart;
  negativeStart.line = -1;
  EXPECT_EQ(editor_.findFirst("x", negativeStart),
            Coordinates(editor_.buffer().getTotalLines(), 0));
}

TEST_F(TextEditorCoreTests, UndoPureDeleteRestoresTextAndEmitsUndoIntent) {
  editor_.setText("abc");
  editor_.setSelection(Coordinates(0, 1), Coordinates(0, 2));
  editor_.delete_();
  editor_.takePendingSourceEditIntents();

  editor_.undo();

  EXPECT_THAT(editor_.getText(), Eq("abc"));
  const std::vector<SourceEditIntent> intents = editor_.takePendingSourceEditIntents();
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].kind, SourceEditIntentKind::Undo);
  EXPECT_EQ(intents[0].removedLength, 0u);
  EXPECT_EQ(intents[0].replacement, "b");
}

TEST_F(TextEditorCoreTests, UndoRecordUndoAndRedoHandlePureInsertAndPureDelete) {
  editor_.setText("abc");

  EditorState before;
  before.cursorPosition = Coordinates(0, 1);
  before.selectionStart = before.cursorPosition;
  before.selectionEnd = before.cursorPosition;

  EditorState afterInsert;
  afterInsert.cursorPosition = Coordinates(0, 2);
  afterInsert.selectionStart = afterInsert.cursorPosition;
  afterInsert.selectionEnd = afterInsert.cursorPosition;

  UndoRecord insertRecord("X", Coordinates(0, 1), Coordinates(0, 2), "", Coordinates(0, 1),
                          Coordinates(0, 1), before, afterInsert);
  insertRecord.redo(&editor_);
  EXPECT_THAT(editor_.getText(), Eq("aXbc"));
  insertRecord.undo(&editor_);
  EXPECT_THAT(editor_.getText(), Eq("abc"));

  EditorState afterDelete = before;
  UndoRecord deleteRecord("", Coordinates(0, 1), Coordinates(0, 1), "b", Coordinates(0, 1),
                          Coordinates(0, 2), before, afterDelete);
  deleteRecord.redo(&editor_);
  EXPECT_THAT(editor_.getText(), Eq("ac"));
  deleteRecord.undo(&editor_);
  EXPECT_THAT(editor_.getText(), Eq("abc"));
}

TEST_F(TextEditorCoreTests, NewlineWithoutLanguageAutoIndentDoesNotCopyWhitespace) {
  LanguageDefinition language;
  language.autoIndentation = false;
  editor_.setLanguageDefinition(language);
  editor_.setText("  x");
  editor_.setCompleteBraces(false);
  editor_.setSmartIndent(true);
  editor_.setCursorPosition(Coordinates(0, 3));

  editor_.enterCharacter('\n', false);

  EXPECT_THAT(editor_.getText(), Eq("  x\n"));
  EXPECT_EQ(editor_.getCursorPosition(), Coordinates(1, 0));
}

TEST_F(TextEditorCoreTests, ChangeTrackingSkipsLineAlreadyMarked) {
  editor_.setText("abc");
  editor_.setScrollbarMarkers(true);
  editor_.changedLines() = {0};
  editor_.setCompleteBraces(false);
  editor_.setCursorPosition(Coordinates(0, 1));

  editor_.enterCharacter('X', false);

  EXPECT_THAT(editor_.getText(), Eq("aXbc"));
  EXPECT_THAT(editor_.changedLines(), ElementsAre(0));
}

TEST_F(TextEditorCoreTests, InsertTextAtRecordsFoldsAndScrollbarMarker) {
  editor_.setText("tail");
  editor_.setScrollbarMarkers(true);
  Coordinates where(0, 0);

  editor_.insertTextAt(where, "{}");

  EXPECT_THAT(editor_.getText(), Eq("{}tail"));
  EXPECT_THAT(editor_.changedLines(), ElementsAre(0));
  ASSERT_EQ(editor_.foldBegin().size(), 1u);
  ASSERT_EQ(editor_.foldEnd().size(), 1u);
}

TEST_F(TextEditorCoreTests, InsertLineShiftsFoldsAtAndAfterSplitPoint) {
  editor_.setText("abcdef\nnext");
  editor_.foldBegin() = {Coordinates(0, 1), Coordinates(0, 4), Coordinates(1, 0)};
  editor_.foldEnd() = {Coordinates(0, 1), Coordinates(0, 4), Coordinates(1, 0)};

  editor_.insertLine(1, 3);

  EXPECT_THAT(editor_.foldBegin(),
              ElementsAre(Coordinates(0, 1), Coordinates(1, 4), Coordinates(2, 0)));
  EXPECT_THAT(editor_.foldEnd(),
              ElementsAre(Coordinates(0, 1), Coordinates(1, 4), Coordinates(2, 0)));
}

// ---------------------------------------------------------------------------
// Tab size / indentation detection
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, SetTabSizeClampsToRange) {
  editor_.setTabSize(99);
  EXPECT_EQ(editor_.getTabSize(), 32);
  editor_.setTabSize(-5);
  EXPECT_EQ(editor_.getTabSize(), 0);
}

TEST_F(TextEditorCoreTests, InlineConfigurationAccessorsRoundTrip) {
  Palette palette{};
  palette[static_cast<size_t>(ColorIndex::Default)] = 0x12345678;

  editor_.setPalette(palette);
  EXPECT_EQ(editor_.getPalette()[static_cast<size_t>(ColorIndex::Default)], 0x12345678u);

  editor_.setInsertSpaces(false);
  EXPECT_FALSE(editor_.getInsertSpaces());
  editor_.setInsertSpaces(true);
  EXPECT_TRUE(editor_.getInsertSpaces());

  editor_.setSmartIndent(false);
  editor_.setCompleteBraces(false);
  editor_.setActiveAutocomplete(true);

  editor_.setAutoIndentOnPaste(true);
  EXPECT_TRUE(editor_.autoIndentOnPasteRef());
  editor_.setAutoIndentOnPaste(false);
  EXPECT_FALSE(editor_.autoIndentOnPasteRef());

  editor_.setScrollbarMarkers(true);
  EXPECT_TRUE(editor_.scrollbarMarkersRef());
  editor_.setScrollbarMarkers(false);
  EXPECT_FALSE(editor_.scrollbarMarkersRef());

  editor_.setSelectionMode(SelectionMode::Word);
  EXPECT_EQ(editor_.selectionMode(), SelectionMode::Word);

  EXPECT_FALSE(editor_.scrollToCursorRequested());
  editor_.requestScrollToCursor();
  EXPECT_TRUE(editor_.scrollToCursorRequested());
  editor_.clearScrollToCursor();
  EXPECT_FALSE(editor_.scrollToCursorRequested());

  editor_.setReplaceIndex(7);
  EXPECT_EQ(editor_.replaceIndex(), 7);
}

TEST_F(TextEditorCoreTests, DetectIndentationStylePicksTabsOverSpaces) {
  editor_.setText("\tfoo\n\tbar\n\tbaz");
  // setText() runs detectIndentationStyle().
  EXPECT_FALSE(editor_.getInsertSpaces());
}

TEST_F(TextEditorCoreTests, DetectIndentationStyleInfersSpaceTabSize) {
  editor_.setText("    a\n        b\n    c");
  EXPECT_TRUE(editor_.getInsertSpaces());
  EXPECT_EQ(editor_.getTabSize(), 4);
}

TEST_F(TextEditorCoreTests, DetectIndentationStyleFallsBackToFourSpacesWithoutIndent) {
  editor_.setTabSize(2);
  editor_.setText("a\nb\nc");

  EXPECT_TRUE(editor_.getInsertSpaces());
  EXPECT_EQ(editor_.getTabSize(), 4);
}

TEST_F(TextEditorCoreTests, DetectIndentationStyleUsesFourWhenSpaceGcdIsTiny) {
  editor_.setText("  a\n   b\n  c");

  EXPECT_TRUE(editor_.getInsertSpaces());
  EXPECT_EQ(editor_.getTabSize(), 4);
}

// ---------------------------------------------------------------------------
// Syntax highlighting / colorizer
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, ColorizerDisabledLeavesGlyphsDefault) {
  editor_.setText("<rect/>");
  editor_.setLanguageDefinition(LanguageDefinition::SVG());
  editor_.setColorizerEnabled(false);
  editor_.colorizeInternal();
  const Line& line = editor_.buffer().getLineGlyphs(0);
  EXPECT_THAT(line, Each(Field("colorIndex", &Glyph::colorIndex, ColorIndex::Default)));
}

TEST_F(TextEditorCoreTests, ColorizeRangeDisabledLeavesExistingGlyphColorsUntouched) {
  editor_.setText("<rect/>");
  editor_.setLanguageDefinition(LanguageDefinition::SVG());
  Line& line = editor_.buffer().getLineGlyphsMutable(0);
  ASSERT_GT(line.size(), 1u);
  line[1].colorIndex = ColorIndex::Keyword;

  editor_.setColorizerEnabled(false);
  editor_.colorizeRange(0, 1);

  EXPECT_EQ(line[1].colorIndex, ColorIndex::Keyword);
}

TEST_F(TextEditorCoreTests, SvgKeywordColorizedAsKeyword) {
  editor_.setText("<rect x=\"1\"/>");
  editor_.setLanguageDefinition(LanguageDefinition::SVG());
  editor_.colorizeInternal();  // process comment pass
  editor_.colorizeInternal();  // process the colorize range
  const Line& line = editor_.buffer().getLineGlyphs(0);
  // "rect" starts at index 1 (after '<').
  EXPECT_THAT(ColorIndexesAt(line, {1}), ElementsAre(Optional(ColorIndex::Keyword)));
}

TEST_F(TextEditorCoreTests, GetGlyphColorUsesDefaultWhenColorizerDisabled) {
  Palette palette{};
  palette[static_cast<size_t>(ColorIndex::Default)] = 0xDEADBEEF;
  palette[static_cast<size_t>(ColorIndex::Keyword)] = 0x12345678;
  editor_.mutablePalette() = palette;
  editor_.setColorizerEnabled(false);

  Glyph keywordGlyph('k', ColorIndex::Keyword);
  EXPECT_EQ(editor_.getGlyphColor(keywordGlyph), 0xDEADBEEFu);
}

TEST_F(TextEditorCoreTests, GetGlyphColorHonorsCommentFlag) {
  Palette palette{};
  palette[static_cast<size_t>(ColorIndex::Comment)] = 0xCAFEF00D;
  editor_.mutablePalette() = palette;
  editor_.setColorizerEnabled(true);

  Glyph commentGlyph('x', ColorIndex::Default);
  commentGlyph.isComment = true;
  EXPECT_EQ(editor_.getGlyphColor(commentGlyph), 0xCAFEF00Du);
}

TEST_F(TextEditorCoreTests, GetGlyphColorHonorsMultiLineCommentFlag) {
  Palette palette{};
  palette[static_cast<size_t>(ColorIndex::MultiLineComment)] = 0xBEEFCACE;
  editor_.mutablePalette() = palette;
  editor_.setColorizerEnabled(true);

  Glyph commentGlyph('x', ColorIndex::Default);
  commentGlyph.isMultiLineComment = true;
  EXPECT_EQ(editor_.getGlyphColor(commentGlyph), 0xBEEFCACEu);
}

TEST_F(TextEditorCoreTests, CustomLanguageColorizesCaseInsensitiveIdentifiers) {
  LanguageDefinition language;
  language.keywords.insert("FOO");
  language.identifiers.emplace("BAR", Identifier("BAR"));
  language.caseSensitive = false;
  language.tokenRegexStrings.push_back({R"([A-Za-z_]+)", ColorIndex::Identifier});

  editor_.setText("foo bar baz");
  editor_.setLanguageDefinition(language);
  editor_.colorizeInternal();
  editor_.colorizeInternal();

  const Line& line = editor_.buffer().getLineGlyphs(0);
  EXPECT_THAT(ColorIndexesAt(line, {0, 4, 8}),
              ElementsAre(Optional(ColorIndex::Keyword), Optional(ColorIndex::KnownIdentifier),
                          Optional(ColorIndex::Identifier)));
}

TEST_F(TextEditorCoreTests, CustomTokenizerTakesPrecedenceOverRegexRules) {
  LanguageDefinition language;
  language.tokenize = [](const char* inBegin, const char* inEnd, const char*& outBegin,
                         const char*& outEnd, ColorIndex& paletteIndex) {
    if (inBegin != inEnd && *inBegin == '@') {
      outBegin = inBegin;
      outEnd = inBegin + 1;
      paletteIndex = ColorIndex::UserFunction;
      return true;
    }
    return false;
  };
  language.tokenRegexStrings.push_back({R"(@)", ColorIndex::Punctuation});

  editor_.setText("@");
  editor_.setLanguageDefinition(language);
  editor_.colorizeInternal();

  const Line& line = editor_.buffer().getLineGlyphs(0);
  EXPECT_THAT(ColorIndexesAt(line, {0}), ElementsAre(Optional(ColorIndex::UserFunction)));
}

namespace {

struct TokenExpectation {
  std::string_view input;
  std::string_view token;
  ColorIndex color;
};

void ExpectSvgToken(const TokenExpectation& expectation) {
  const LanguageDefinition& language = LanguageDefinition::SVG();
  const char* outBegin = nullptr;
  const char* outEnd = nullptr;
  ColorIndex color = ColorIndex::Default;

  ASSERT_TRUE(language.tokenize(expectation.input.data(),
                                expectation.input.data() + expectation.input.size(), outBegin,
                                outEnd, color))
      << expectation.input;
  EXPECT_EQ(std::string_view(outBegin, static_cast<std::size_t>(outEnd - outBegin)),
            expectation.token);
  EXPECT_EQ(color, expectation.color);
}

}  // namespace

TEST_F(TextEditorCoreTests, SvgTokenizerClassifiesXmlSyntaxTokens) {
  const TokenExpectation cases[] = {
      {"", "", ColorIndex::Default},
      {"<", "<", ColorIndex::Punctuation},
      {"<rect", "<", ColorIndex::Punctuation},
      {"</rect>", "</", ColorIndex::Punctuation},
      {">", ">", ColorIndex::Punctuation},
      {"/", "", ColorIndex::Default},
      {"/rect", "", ColorIndex::Default},
      {"/>", "/>", ColorIndex::Punctuation},
      {"\"value\"", "\"", ColorIndex::Punctuation},
      {"'value'", "'", ColorIndex::Punctuation},
      {"=", "=", ColorIndex::Punctuation},
      {"@unknown", "", ColorIndex::Default},
  };

  for (const TokenExpectation& expectation : cases) {
    SCOPED_TRACE(expectation.input);
    if (expectation.token.empty()) {
      const LanguageDefinition& language = LanguageDefinition::SVG();
      const char* outBegin = nullptr;
      const char* outEnd = nullptr;
      ColorIndex color = ColorIndex::Default;
      EXPECT_FALSE(language.tokenize(expectation.input.data(),
                                     expectation.input.data() + expectation.input.size(), outBegin,
                                     outEnd, color));
    } else {
      ExpectSvgToken(expectation);
    }
  }
}

TEST_F(TextEditorCoreTests, SvgTokenizerClassifiesEntitiesNumbersAndIdentifiers) {
  const TokenExpectation cases[] = {
      {"&", "&", ColorIndex::Number},
      {"&amp; tail", "&amp;", ColorIndex::Number},
      {"&name<tag", "&name", ColorIndex::Number},
      {"&name>tail", "&name", ColorIndex::Number},
      {"&name\ttail", "&name", ColorIndex::Number},
      {"&name\ntail", "&name", ColorIndex::Number},
      {"&#", "&#", ColorIndex::Number},
      {"&#65; tail", "&#65;", ColorIndex::Number},
      {"&#x41; tail", "&#x41;", ColorIndex::Number},
      {"&#X41; tail", "&#X41;", ColorIndex::Number},
      {"&unterminated tail", "&unterminated", ColorIndex::Number},
      {"1", "1", ColorIndex::Number},
      {"1 ", "1", ColorIndex::Number},
      {"1)", "1", ColorIndex::Number},
      {"12px", "12px", ColorIndex::Number},
      {"1e2", "1e2", ColorIndex::Number},
      {"1e", "1e", ColorIndex::Number},
      {"1e next", "1e", ColorIndex::Number},
      {"1E+", "1E+", ColorIndex::Number},
      {"-1.5e+2px", "-1.5e+2px", ColorIndex::Number},
      {".5%", ".5%", ColorIndex::Number},
      {"9E-2em", "9E-2em", ColorIndex::Number},
      {"a", "a", ColorIndex::Identifier},
      {"a@", "a", ColorIndex::Identifier},
      {":href", ":href", ColorIndex::Identifier},
      {"A1", "A1", ColorIndex::Identifier},
      {"_private:name", "_private:name", ColorIndex::Identifier},
      {"xml:name-1.2", "xml:name-1.2", ColorIndex::Identifier},
      {"a:b.c-1", "a:b.c-1", ColorIndex::Identifier},
      {std::string_view("\xC3\xA9"
                        "lement attr"),
       std::string_view("\xC3\xA9"
                        "lement"),
       ColorIndex::Identifier},
  };

  for (const TokenExpectation& expectation : cases) {
    SCOPED_TRACE(expectation.input);
    ExpectSvgToken(expectation);
  }
}

TEST_F(TextEditorCoreTests, SvgTokenizerRejectsNumberLookalikes) {
  const char* inputs[] = {"-", "-x", ".", ".x"};
  const LanguageDefinition& language = LanguageDefinition::SVG();

  for (const char* input : inputs) {
    SCOPED_TRACE(input);
    const char* outBegin = nullptr;
    const char* outEnd = nullptr;
    ColorIndex color = ColorIndex::Default;
    EXPECT_FALSE(language.tokenize(input, input + std::strlen(input), outBegin, outEnd, color));
  }
}

TEST_F(TextEditorCoreTests, SvgTokenizerReturnsFalseForEmptyInput) {
  const LanguageDefinition& language = LanguageDefinition::SVG();
  const char* begin = "";
  const char* outBegin = nullptr;
  const char* outEnd = nullptr;
  ColorIndex color = ColorIndex::Default;

  EXPECT_FALSE(language.tokenize(begin, begin, outBegin, outEnd, color));
}

TEST_F(TextEditorCoreTests, ColorizeInternalMarksSingleAndMultilineComments) {
  LanguageDefinition language;
  language.commentStart = "/*";
  language.commentEnd = "*/";
  language.singleLineComment = "//";

  editor_.setText("a // one\n/* multi\nstill */ \"not /* comment\"");
  editor_.setLanguageDefinition(language);
  editor_.colorizeInternal();

  const Line& line0 = editor_.buffer().getLineGlyphs(0);
  EXPECT_FALSE(line0[0].isComment);
  EXPECT_TRUE(line0[2].isComment);

  const Line& line1 = editor_.buffer().getLineGlyphs(1);
  EXPECT_TRUE(line1[0].isMultiLineComment);
  EXPECT_TRUE(line1[3].isMultiLineComment);

  const Line& line2 = editor_.buffer().getLineGlyphs(2);
  EXPECT_TRUE(line2[0].isMultiLineComment);
  EXPECT_FALSE(line2[9].isMultiLineComment);
}

TEST_F(TextEditorCoreTests, ColorizeInternalHandlesStringEscapesAndDoubledQuotes) {
  LanguageDefinition language;
  language.commentStart = "/*";
  language.commentEnd = "*/";
  language.singleLineComment = "//";

  const std::string text =
      "\"a\"\"b /* not comment */\" c // line\n\"slash \\\" /* still string */\"";
  editor_.setText(text);
  editor_.setLanguageDefinition(language);
  editor_.colorizeInternal();

  const Line& doubledQuoteLine = editor_.buffer().getLineGlyphs(0);
  const std::size_t firstLineBlockComment = text.find("/*");
  ASSERT_NE(firstLineBlockComment, std::string::npos);
  EXPECT_FALSE(doubledQuoteLine[firstLineBlockComment].isMultiLineComment);
  const std::size_t singleLineComment = text.find("//");
  ASSERT_NE(singleLineComment, std::string::npos);
  EXPECT_TRUE(doubledQuoteLine[singleLineComment].isComment);

  const Line& escapedLine = editor_.buffer().getLineGlyphs(1);
  const std::string_view escapedLineText = "\"slash \\\" /* still string */\"";
  const std::size_t escapedLineBlockComment = escapedLineText.find("/*");
  ASSERT_NE(escapedLineBlockComment, std::string_view::npos);
  EXPECT_FALSE(escapedLine[escapedLineBlockComment].isMultiLineComment);
}

}  // namespace donner::editor
