#include "donner/editor/TextBuffer.h"

#include <gtest/gtest.h>

namespace donner::editor {

/**
 * Test that a newly created TextBuffer has a single empty line.
 */
TEST(TextBuffer, DefaultConstructor) {
  TextBuffer buffer;
  EXPECT_EQ(buffer.getTotalLines(), 1);
  EXPECT_EQ(buffer.getLineCharacterCount(0), 0);
  EXPECT_EQ(buffer.getText(), "");
}

/**
 * Test loading single-line text and verify.
 */
TEST(TextBuffer, SetTextSingleLine) {
  TextBuffer buffer;
  buffer.setText("Hello, world!");
  EXPECT_EQ(buffer.getTotalLines(), 1);
  EXPECT_EQ(buffer.getLineCharacterCount(0), 13);
  EXPECT_EQ(buffer.getText(), "Hello, world!");
}

/**
 * Test loading multi-line text and verify line count / content.
 */
TEST(TextBuffer, SetTextMultiLine) {
  TextBuffer buffer;
  buffer.setText("Line1\nLine2\nLine3");
  EXPECT_EQ(buffer.getTotalLines(), 3);
  EXPECT_EQ(buffer.getLineCharacterCount(0), 5);
  EXPECT_EQ(buffer.getLineCharacterCount(1), 5);
  EXPECT_EQ(buffer.getLineCharacterCount(2), 5);
  EXPECT_EQ(buffer.getText(), "Line1\nLine2\nLine3");
}

/**
 * Test getText(start, end) for extracting a subrange.
 */
TEST(TextBuffer, GetTextRange) {
  TextBuffer buffer;
  buffer.setText("Line0\nLine1\nLine2\nLine3\nLine4");
  // Coordinates(line, column)
  // Grab "ine1\nLine2\nLi"
  // start = (1,1) => from 'i' in "Line1"
  // end   = (3,2) => up to 'i' in "Line3"
  std::string sub = buffer.getText({1, 1}, {3, 2});
  EXPECT_EQ(sub, "ine1\nLine2\nLi");
}

/**
 * Test inserting text at arbitrary coordinates, including newlines.
 */
TEST(TextBuffer, InsertTextAt) {
  TextBuffer buffer;
  buffer.setText("Hello");
  // Insert " there\nLine2" after 'Hello'
  Coordinates pos(0, 5);
  buffer.insertTextAt(pos, " there\nLine2");
  EXPECT_EQ(buffer.getTotalLines(), 2);
  EXPECT_EQ(buffer.getText(), "Hello there\nLine2");

  // Insert at the beginning of second line
  Coordinates pos2(1, 0);
  buffer.insertTextAt(pos2, "++ ");
  EXPECT_EQ(buffer.getTotalLines(), 2);
  EXPECT_EQ(buffer.getText(), "Hello there\n++ Line2");
}

/**
 * Test deleting a range within a single line.
 */
TEST(TextBuffer, DeleteRangeSingleLine) {
  TextBuffer buffer;
  buffer.setText("abcdef");
  // Delete "bcd"
  buffer.deleteRange({0, 1}, {0, 4});  // coords inclusive at start, exclusive at end
  EXPECT_EQ(buffer.getText(), "aef");

  buffer.setText("Line0\nLine1");
  // Delete "Line0"
  buffer.deleteRange({0, 0}, {0, 6});
  EXPECT_EQ(buffer.getText(), "Line1");
}

/**
 * Test deleting across multiple lines.
 */
TEST(TextBuffer, DeleteRangeMultiLine) {
  const std::string_view kText =
      "Line0\n"
      "Line1\n"
      "Line2\n"
      "Line3";

  TextBuffer buffer;
  buffer.setText(
      "Line0\n"
      "Line1\n"
      "Line2\n"
      "Line3");
  // Remove "ine1\nLine2\nLi" => start=(1,1), end=(3,3)
  buffer.deleteRange({1, 1}, {3, 3});
  EXPECT_EQ(buffer.getText(), "Line0\nLe3");

  // Remove "0Lin"
  buffer.setText(kText);
  buffer.deleteRange({0, 4}, {1, 3});
  EXPECT_EQ(buffer.getText(), "Linee1\nLine2\nLine3");
}

/**
 * Test removing one or more lines entirely.
 */
TEST(TextBuffer, RemoveLine) {
  TextBuffer buffer;
  buffer.setText("Line0\nLine1\nLine2\nLine3\nLine4");
  // Remove just line 2
  buffer.removeLine(2);
  EXPECT_EQ(buffer.getTotalLines(), 4);
  EXPECT_EQ(buffer.getText(), "Line0\nLine1\nLine3\nLine4");

  // Remove lines [1, 3), so remove line1 and line3
  buffer.removeLine(1, 3);
  EXPECT_EQ(buffer.getTotalLines(), 2);
  EXPECT_EQ(buffer.getText(), "Line0\nLine4");
}

/**
 * Test some coordinate helpers and line info.
 */
TEST(TextBuffer, LineColumnHelpers) {
  TextBuffer buffer;
  buffer.setText(
      "abc\n"
      "\tTabLine\n"
      "SomeOtherLine");
  // "abc": 3 chars
  // "\tTabLine": tab is 2 spaces if tabSize=2
  // So second line raw glyph count = 8
  EXPECT_EQ(buffer.getLineMaxColumn(0), 3);
  EXPECT_EQ(buffer.getLineMaxColumn(1), 9);
  EXPECT_EQ(buffer.getLineCharacterCount(1), 8);

  // getCharacterIndex for second line at column=0 => returns 0
  EXPECT_EQ(buffer.getCharacterIndex({1, 0}), 0);
  // getCharacterIndex for second line at column=2 => tab consumes them
  EXPECT_EQ(buffer.getCharacterIndex({1, 2}), 1);

  // getCharacterColumn(1,1) => means second line, index=1 => we are inside the tab, so column=2
  EXPECT_EQ(buffer.getCharacterColumn(1, 1), 2);

  // Third line
  EXPECT_EQ(buffer.getLineMaxColumn(2), 13);
  EXPECT_EQ(buffer.getTotalLines(), 3);
}

}  // namespace donner::editor
