#include "donner/editor/TextEditorCore.h"

#include <gtest/gtest.h>

namespace donner::editor {

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

}  // namespace donner::editor
