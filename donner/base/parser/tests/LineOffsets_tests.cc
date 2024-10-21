#include "donner/base/parser/LineOffsets.h"

#include <gmock/gmock.h>
#include <gtest/gtest-death-test.h>
#include <gtest/gtest.h>

using testing::ElementsAre;

namespace donner::base::parser {

TEST(LineOffsets, NoLines) {
  {
    LineOffsets offsets("");
    EXPECT_THAT(offsets.offsets(), ElementsAre());
    EXPECT_EQ(offsets.offsetToLine(0), 1);
    EXPECT_EQ(offsets.offsetToLine(1234), 1);

    EXPECT_EQ(offsets.lineOffset(1), 0);
  }

  {
    LineOffsets offsets("asdf\t\fasdf");
    EXPECT_THAT(offsets.offsets(), ElementsAre());
    EXPECT_EQ(offsets.offsetToLine(0), 1);
    EXPECT_EQ(offsets.offsetToLine(1234), 1);

    EXPECT_EQ(offsets.lineOffset(1), 0);
  }
}

TEST(LineOffsets, LineBreak) {
  {
    LineOffsets offsets("012\r456");
    EXPECT_THAT(offsets.offsets(), ElementsAre(4));
    EXPECT_EQ(offsets.offsetToLine(0), 1);
    EXPECT_EQ(offsets.offsetToLine(2), 1);
    EXPECT_EQ(offsets.offsetToLine(3), 1);
    EXPECT_EQ(offsets.offsetToLine(4), 2);
    EXPECT_EQ(offsets.offsetToLine(6), 2);
    EXPECT_EQ(offsets.offsetToLine(1234), 2);

    EXPECT_EQ(offsets.lineOffset(1), 0);
    EXPECT_EQ(offsets.lineOffset(2), 4);
  }

  {
    LineOffsets offsets("012\r\n567");
    EXPECT_THAT(offsets.offsets(), ElementsAre(5));
    EXPECT_EQ(offsets.offsetToLine(0), 1);
    EXPECT_EQ(offsets.offsetToLine(2), 1);
    EXPECT_EQ(offsets.offsetToLine(3), 1);
    EXPECT_EQ(offsets.offsetToLine(4), 1);
    EXPECT_EQ(offsets.offsetToLine(5), 2);
    EXPECT_EQ(offsets.offsetToLine(7), 2);
    EXPECT_EQ(offsets.offsetToLine(1234), 2);

    EXPECT_EQ(offsets.lineOffset(1), 0);
    EXPECT_EQ(offsets.lineOffset(2), 5);
  }
}

TEST(LineOffsets, MultipleBreaks) {
  LineOffsets offsets(
      "0\r\n"  // 1: [0-2]
      "\r"     // 2: [3]
      "\r"     // 3: [4]
      "567\n"  // 4: [5-8]
      "\n"     // 5: [9]
      "01");   // 6: [10, 11]
  EXPECT_THAT(offsets.offsets(), ElementsAre(3, 4, 5, 9, 10));
  EXPECT_EQ(offsets.offsetToLine(0), 1);
  EXPECT_EQ(offsets.offsetToLine(1), 1);
  EXPECT_EQ(offsets.offsetToLine(2), 1);
  EXPECT_EQ(offsets.offsetToLine(3), 2);
  EXPECT_EQ(offsets.offsetToLine(4), 3);
  EXPECT_EQ(offsets.offsetToLine(5), 4);
  EXPECT_EQ(offsets.offsetToLine(6), 4);
  EXPECT_EQ(offsets.offsetToLine(7), 4);
  EXPECT_EQ(offsets.offsetToLine(8), 4);
  EXPECT_EQ(offsets.offsetToLine(9), 5);
  EXPECT_EQ(offsets.offsetToLine(10), 6);
  EXPECT_EQ(offsets.offsetToLine(11), 6);
  EXPECT_EQ(offsets.offsetToLine(12), 6);
  EXPECT_EQ(offsets.offsetToLine(1234), 6);
}

TEST(LineOffsets, LineOffsetErrors) {
  {
    LineOffsets offsets("");

    EXPECT_DEATH(offsets.lineOffset(0), "");
    EXPECT_EQ(offsets.lineOffset(1), 0);
    EXPECT_DEATH(offsets.lineOffset(2), "");
  }

  {
    LineOffsets offsets("012\r\n567\n9");

    EXPECT_DEATH(offsets.lineOffset(0), "");
    EXPECT_EQ(offsets.lineOffset(1), 0);
    EXPECT_EQ(offsets.lineOffset(2), 5);
    EXPECT_EQ(offsets.lineOffset(3), 9);
    EXPECT_DEATH(offsets.lineOffset(4), "");
  }
}

}  // namespace donner::base::parser
