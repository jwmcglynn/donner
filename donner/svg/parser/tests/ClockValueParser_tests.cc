#include "donner/svg/parser/ClockValueParser.h"

#include <gtest/gtest.h>

namespace donner::svg::parser {

TEST(ClockValueParser, BareNumber) {
  auto result = ClockValueParser::Parse("2");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 2.0);
}

TEST(ClockValueParser, NumberWithS) {
  auto result = ClockValueParser::Parse("2.5s");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 2.5);
}

TEST(ClockValueParser, NumberWithMs) {
  auto result = ClockValueParser::Parse("500ms");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 0.5);
}

TEST(ClockValueParser, NumberWithMin) {
  auto result = ClockValueParser::Parse("1.5min");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 90.0);
}

TEST(ClockValueParser, NumberWithH) {
  auto result = ClockValueParser::Parse("1h");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 3600.0);
}

TEST(ClockValueParser, PartialClock) {
  auto result = ClockValueParser::Parse("01:30");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 90.0);
}

TEST(ClockValueParser, PartialClockFractional) {
  auto result = ClockValueParser::Parse("01:30.5");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 90.5);
}

TEST(ClockValueParser, FullClock) {
  auto result = ClockValueParser::Parse("01:30:00");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 5400.0);
}

TEST(ClockValueParser, FullClockFractional) {
  auto result = ClockValueParser::Parse("00:02:30.5");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 150.5);
}

TEST(ClockValueParser, Indefinite) {
  auto result = ClockValueParser::Parse("indefinite");
  ASSERT_TRUE(result.hasResult());
  EXPECT_TRUE(result.result().isIndefinite());
}

TEST(ClockValueParser, Negative) {
  auto result = ClockValueParser::Parse("-1s");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), -1.0);
}

TEST(ClockValueParser, Positive) {
  auto result = ClockValueParser::Parse("+2s");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 2.0);
}

TEST(ClockValueParser, Zero) {
  auto result = ClockValueParser::Parse("0s");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 0.0);
}

TEST(ClockValueParser, WhitespaceTrimed) {
  auto result = ClockValueParser::Parse("  2s  ");
  ASSERT_TRUE(result.hasResult());
  EXPECT_DOUBLE_EQ(result.result().seconds(), 2.0);
}

TEST(ClockValueParser, EmptyIsError) {
  auto result = ClockValueParser::Parse("");
  EXPECT_TRUE(result.hasError());
}

TEST(ClockValueParser, InvalidMetric) {
  auto result = ClockValueParser::Parse("2foo");
  EXPECT_TRUE(result.hasError());
}

}  // namespace donner::svg::parser
