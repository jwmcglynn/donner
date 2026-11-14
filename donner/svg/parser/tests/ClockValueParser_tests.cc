#include "donner/svg/parser/ClockValueParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace donner::svg::parser {

using testing::DoubleEq;
using testing::Eq;
using testing::Optional;

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

TEST(ClockValueParser, RejectsNonFiniteValues) {
  EXPECT_TRUE(ClockValueParser::Parse("nan").hasError());
  EXPECT_TRUE(ClockValueParser::Parse("1e999s").hasError());
  EXPECT_TRUE(ClockValueParser::Parse("1e308h").hasError());
}

namespace {

/// Returns the parsed offset in seconds, or std::nullopt when the parse fails or is indefinite.
std::optional<double> ParsedSeconds(std::string_view input) {
  auto result = ClockValueParser::Parse(input);
  if (result.hasError() || result.result().isIndefinite()) {
    return std::nullopt;
  }
  return result.result().seconds();
}

}  // namespace

TEST(ClockValueParser, RejectsFormsOutsideTheClockValueGrammar) {
  for (const std::string_view input :
       {"0x10s", "0x1p4", "--5s", "+-5s", "-+5s", "inf", "-infinity", "5.s", "\v5s", "\f5s",
        "00:00:-5", "00:00: 5", "00:00:+5", "00:00:0x5", "00:00:1e1", "00:00:.5", "00:00:5.",
        "01:-30", "00:-1:00"}) {
    EXPECT_THAT(ParsedSeconds(input), Eq(std::nullopt)) << "input: \"" << input << "\"";
  }
}

TEST(ClockValueParser, AcceptsWhitespaceBetweenSignAndValue) {
  EXPECT_THAT(ParsedSeconds("- 5s"), Optional(DoubleEq(-5.0)));
  EXPECT_THAT(ParsedSeconds("+\t2s"), Optional(DoubleEq(2.0)));
  EXPECT_THAT(ParsedSeconds("- 01:00"), Optional(DoubleEq(-60.0)));
}

TEST(ClockValueParser, TrimsSurroundingXmlWhitespace) {
  EXPECT_THAT(ParsedSeconds("\r\n 2s\t\r\n"), Optional(DoubleEq(2.0)));
}

TEST(ClockValueParser, AcceptsNumberTokenTimecounts) {
  EXPECT_THAT(ParsedSeconds("1.5e1s"), Optional(DoubleEq(15.0)));
  EXPECT_THAT(ParsedSeconds(".5s"), Optional(DoubleEq(0.5)));
}

TEST(ClockValueParser, LongFractionsKeepTheirMagnitude) {
  EXPECT_THAT(ParsedSeconds("1.9999999999999999999s"), Optional(DoubleEq(2.0)));
  EXPECT_THAT(ParsedSeconds("18.500000000000000000s"), Optional(DoubleEq(18.5)));
  EXPECT_THAT(ParsedSeconds("00:00:18.500000000000000000"), Optional(DoubleEq(18.5)));
}

}  // namespace donner::svg::parser
