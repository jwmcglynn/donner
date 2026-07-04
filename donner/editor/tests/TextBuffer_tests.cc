#include "donner/editor/TextBuffer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::editor {
namespace {

MATCHER_P2(GlyphIs, expectedCharacter, expectedColorIndex, "") {
  return testing::ExplainMatchResult(
      testing::AllOf(
          testing::Field("character", &Glyph::character, testing::Eq(expectedCharacter)),
          testing::Field("colorIndex", &Glyph::colorIndex, testing::Eq(expectedColorIndex))),
      arg, result_listener);
}

}  // namespace

/**
 * Test that a newly created TextBuffer has a single empty line.
 */
TEST(TextBuffer, DefaultConstructor) {
  TextBuffer buffer;
  EXPECT_EQ(buffer.getTotalLines(), 1);
  EXPECT_EQ(buffer.getLineCharacterCount(0), 0);
  EXPECT_EQ(buffer.getText(), "");
}

TEST(TextBuffer, LineEmplaceInsertsGlyphAtIterator) {
  Line line;
  line.emplace_back('a', ColorIndex::Default);
  line.emplace_back('c', ColorIndex::Default);

  line.emplace(line.begin() + 1, 'b', ColorIndex::Keyword);

  EXPECT_THAT(line, testing::ElementsAre(GlyphIs('a', ColorIndex::Default),
                                         GlyphIs('b', ColorIndex::Keyword),
                                         GlyphIs('c', ColorIndex::Default)));
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

TEST(TextBuffer, GetTextRangeRejectsInvalidAndEmptyRanges) {
  TextBuffer buffer;
  buffer.setText("abc\nxyz");

  EXPECT_EQ(buffer.getText({0, 1}, {0, 1}), "");
  EXPECT_EQ(buffer.getText({9, 0}, {9, 1}), "");
  EXPECT_EQ(buffer.getText({0, 0}, {9, 1}), "");
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

TEST(TextBuffer, InsertTextAtSplitsCurrentLine) {
  TextBuffer buffer;
  buffer.setText("abcd");

  Coordinates pos(0, 2);
  buffer.insertTextAt(pos, "\n");

  EXPECT_EQ(pos, Coordinates(1, 0));
  EXPECT_EQ(buffer.getTotalLines(), 2);
  EXPECT_EQ(buffer.getText(), "ab\ncd");
}

TEST(TextBuffer, InsertTextAtConsumesInvalidUtf8Byte) {
  TextBuffer buffer;
  buffer.setText("ab");

  Coordinates pos(0, 1);
  const std::string invalidUtf8("\xA7", 1);
  buffer.insertTextAt(pos, invalidUtf8);

  EXPECT_EQ(pos.line, 0);
  EXPECT_EQ(pos.column, 2);
  EXPECT_EQ(buffer.getText(), std::string("a") + invalidUtf8 + "b");
  EXPECT_EQ(buffer.getLineCharacterCount(0), 3);
  EXPECT_EQ(buffer.getLineMaxColumn(0), 3);
  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(2), Coordinates(0, 2));
}

TEST(TextBuffer, InsertTextAtWithIndentSkipsCarriageReturnsAndUnindentsClosingBrace) {
  TextBuffer buffer;
  buffer.setText("    {tail");

  Coordinates pos(0, 5);
  const int insertedLines = buffer.insertTextAt(pos, "\r\n}", /*indent=*/true);

  EXPECT_EQ(insertedLines, 1);
  EXPECT_EQ(pos, Coordinates(1, 3));
  EXPECT_EQ(buffer.getText(), "    {\n  }tail");
}

TEST(TextBuffer, InsertTextAtWithIndentAccountsForExplicitLeadingWhitespace) {
  TextBuffer buffer;
  buffer.setText("    start");

  Coordinates pos(0, 9);
  buffer.insertTextAt(pos, "\n  child", /*indent=*/true);

  EXPECT_EQ(pos, Coordinates(1, 7));
  EXPECT_EQ(buffer.getText(), "    start\n  child");
}

TEST(TextBuffer, InsertTextAtWithIndentExpandsTabIndent) {
  TextBuffer buffer;
  buffer.setText("\tparent");

  Coordinates pos(0, 8);
  const int insertedLines = buffer.insertTextAt(pos, "\nchild", /*indent=*/true);

  EXPECT_EQ(insertedLines, 1);
  EXPECT_EQ(pos, Coordinates(1, 7));
  EXPECT_EQ(buffer.getText(), "\tparent\n  child");
}

TEST(TextBuffer, InsertTextAtTabAdvancesByTabSize) {
  TextBuffer buffer;
  buffer.setText("ab");

  Coordinates pos(0, 1);
  const int insertedLines = buffer.insertTextAt(pos, "\t");

  EXPECT_EQ(insertedLines, 0);
  EXPECT_EQ(pos, Coordinates(0, 3));
  EXPECT_EQ(buffer.getText(), "a\tb");
  EXPECT_EQ(buffer.getLineMaxColumn(0), 3);
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

TEST(TextBuffer, DeleteRangeEmptyRangeDoesNothing) {
  TextBuffer buffer;
  buffer.setText("abc\nxyz");

  buffer.deleteRange({1, 2}, {1, 2});

  EXPECT_EQ(buffer.getText(), "abc\nxyz");
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

TEST(TextBuffer, RemoveLineInvalidInputsAndFullRangeKeepOneLine) {
  TextBuffer buffer;
  buffer.setText("only");

  buffer.removeLine(-1);
  buffer.removeLine(10);
  EXPECT_EQ(buffer.getText(), "only");

  buffer.setText("a\nb");
  buffer.removeLine(-5, 99);
  EXPECT_EQ(buffer.getTotalLines(), 1);
  EXPECT_EQ(buffer.getText(), "");

  buffer.setText("a\nb");
  buffer.removeLine(1, 1);
  EXPECT_EQ(buffer.getText(), "a\nb");
}

TEST(TextBuffer, RemoveOnlyLineKeepsOneEmptyLine) {
  TextBuffer buffer;
  buffer.setText("only");

  buffer.removeLine(0);

  EXPECT_EQ(buffer.getTotalLines(), 1);
  EXPECT_EQ(buffer.getText(), "");
}

TEST(TextBuffer, InsertLineClampsIndexAndSplitsAtBoundedColumn) {
  TextBuffer buffer;
  buffer.setText("abcd");

  buffer.insertLine(-5);
  EXPECT_EQ(buffer.getText(), "\nabcd");

  buffer.setText("abcd");
  buffer.insertLine(99);
  EXPECT_EQ(buffer.getText(), "abcd\n");

  buffer.setText("abcd");
  buffer.insertLine(0, 99);
  EXPECT_EQ(buffer.getText(), "abcd\n");
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

  EXPECT_EQ(buffer.getLineMaxColumn(99), 0);
  EXPECT_EQ(buffer.getLineCharacterCount(99), 0);
  EXPECT_EQ(buffer.getCharacterColumn(99, 1), 0);
}

TEST(TextBuffer, GetByteOffsetResolvesMultilineCoordinates) {
  TextBuffer buffer;
  buffer.setText(
      "abc\n"
      "defg\n"
      "hi");

  EXPECT_EQ(buffer.getByteOffset({0, 0}), 0u);
  EXPECT_EQ(buffer.getByteOffset({0, 2}), 2u);
  EXPECT_EQ(buffer.getByteOffset({1, 0}), 4u);
  EXPECT_EQ(buffer.getByteOffset({1, 3}), 7u);
  EXPECT_EQ(buffer.getByteOffset({2, 2}), 11u);
  EXPECT_EQ(buffer.getByteOffset({3, 0}), buffer.getText().size());

  TextBuffer empty;
  EXPECT_EQ(empty.getByteOffset({2, 0}), 0u);
}

TEST(TextBuffer, GetCoordinatesAtByteOffsetResolvesMultilineBoundaries) {
  TextBuffer buffer;
  buffer.setText(
      "abc\n"
      "\tde\n"
      "fg");

  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(0), Coordinates(0, 0));
  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(3), Coordinates(0, 3));
  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(4), Coordinates(1, 0));
  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(5), Coordinates(1, 2));
  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(7), Coordinates(1, 4));
  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(8), Coordinates(2, 0));
  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(buffer.getText().size()), Coordinates(2, 2));
  EXPECT_EQ(buffer.getCoordinatesAtByteOffset(999), Coordinates(2, 2));
}

}  // namespace donner::editor
