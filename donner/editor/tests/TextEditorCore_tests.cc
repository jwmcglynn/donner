#include "donner/editor/TextEditorCore.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::editor {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;

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
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].offset, 14u);
  EXPECT_EQ(intents[0].removedLength, 0u);
  EXPECT_EQ(intents[0].replacement, "abc");
  EXPECT_EQ(intents[0].kind, SourceEditIntentKind::Insert);
  EXPECT_NE(intents[0].bufferVersion, 0u);
}

TEST_F(TextEditorCoreTests, ReplaceSelectionCapturesSourceEditIntent) {
  editor_.resetTextChanged();
  editor_.setSelection(Coordinates(1, 2), Coordinates(2, 4));

  editor_.insertText("replacement");

  std::vector<SourceEditIntent> intents = editor_.takePendingSourceEditIntents();
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].offset, 13u);
  EXPECT_EQ(intents[0].removedLength, 13u);
  EXPECT_EQ(intents[0].replacement, "replacement");
  EXPECT_EQ(intents[0].kind, SourceEditIntentKind::Replace);
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

TEST_F(TextEditorCoreTests, ShiftMoveDownThenUpContractsSelection) {
  editor_.setCursorPosition(Coordinates(1, 0));
  editor_.interactiveStart() = Coordinates(1, 0);
  editor_.interactiveEnd() = Coordinates(1, 0);
  editor_.moveDown(2, /*select=*/true);
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(3, 0));
  editor_.moveUp(1, /*select=*/true);
  EXPECT_EQ(editor_.getSelectionEnd(), Coordinates(2, 0));
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

TEST_F(TextEditorCoreTests, EnterBraceCompletesClosingBrace) {
  editor_.setText("");
  editor_.setCompleteBraces(true);
  editor_.setCursorPosition(Coordinates(0, 0));
  editor_.enterCharacter('(', false);
  EXPECT_THAT(editor_.getText(), Eq("()"));
  // Cursor lands between the braces.
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
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].kind, SourceEditIntentKind::Undo);
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

TEST_F(TextEditorCoreTests, ShiftTabWithMultiLineSelectionOutdents) {
  editor_.setText("  a\n  b\nc");
  editor_.setInsertSpaces(true);
  editor_.setTabSize(2);
  editor_.setSelection(Coordinates(0, 0), Coordinates(1, 2));
  editor_.enterCharacter('\t', /*shift=*/true);
  EXPECT_THAT(editor_.getText(), Eq("a\nb\nc"));
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

// ---------------------------------------------------------------------------
// Tab size / indentation detection
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, SetTabSizeClampsToRange) {
  editor_.setTabSize(99);
  EXPECT_EQ(editor_.getTabSize(), 32);
  editor_.setTabSize(-5);
  EXPECT_EQ(editor_.getTabSize(), 0);
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

// ---------------------------------------------------------------------------
// Syntax highlighting / colorizer
// ---------------------------------------------------------------------------

TEST_F(TextEditorCoreTests, ColorizerDisabledLeavesGlyphsDefault) {
  editor_.setText("<rect/>");
  editor_.setLanguageDefinition(LanguageDefinition::SVG());
  editor_.setColorizerEnabled(false);
  editor_.colorizeInternal();
  const Line& line = editor_.buffer().getLineGlyphs(0);
  for (const auto& g : line) {
    EXPECT_EQ(g.colorIndex, ColorIndex::Default);
  }
}

TEST_F(TextEditorCoreTests, SvgKeywordColorizedAsKeyword) {
  editor_.setText("<rect x=\"1\"/>");
  editor_.setLanguageDefinition(LanguageDefinition::SVG());
  editor_.colorizeInternal();  // process comment pass
  editor_.colorizeInternal();  // process the colorize range
  const Line& line = editor_.buffer().getLineGlyphs(0);
  // "rect" starts at index 1 (after '<').
  EXPECT_EQ(line[1].colorIndex, ColorIndex::Keyword);
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

}  // namespace donner::editor
