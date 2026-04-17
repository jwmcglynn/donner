#include "donner/editor/TextEditor.h"

#include <gtest/gtest.h>

#include "donner/editor/ImGuiIncludes.h"

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

TEST_F(TextEditorTests, CutWithoutSelectionDoesNothing) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.cut();
  EXPECT_EQ(editor.getText(), "Hello") << "cut without selection should not delete";
}

TEST_F(TextEditorTests, PasteInsertsClipboardText) {
  editor.setText("Hello");
  ImGui::SetClipboardText(" world");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.paste();
  EXPECT_EQ(editor.getText(), "Hello world") << "paste should insert clipboard text at cursor";
}

TEST_F(TextEditorTests, PasteWithSelectionReplacesSelection) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  ImGui::SetClipboardText("Hi");
  editor.paste();
  EXPECT_EQ(editor.getText(), "Hi world") << "paste with selection should replace selection";
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
