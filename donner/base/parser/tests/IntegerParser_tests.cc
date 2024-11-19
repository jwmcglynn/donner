#include "donner/base/parser/IntegerParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"

using testing::HasSubstr;

namespace donner::base::parser {

using Result = IntegerParser::Result;

static bool operator==(const Result& lhs, const Result& rhs) {
  return lhs.number == rhs.number && lhs.consumedChars == rhs.consumedChars;
}

static void PrintTo(const Result& result, std::ostream* os) {
  *os << "(" << result.number << ", consumed: " << result.consumedChars << ")";
}

TEST(IntegerParser, TestHelpers) {
  IntegerParser::Result result;
  result.consumedChars = 1;
  result.number = 2;

  EXPECT_EQ(testing::PrintToString(result), "(2, consumed: 1)");

  IntegerParser::Result other;
  other.consumedChars = 1;
  other.number = 2;
  EXPECT_EQ(result, other);

  other.consumedChars = 2;
  EXPECT_NE(result, other);

  other.consumedChars = 1;
  other.number = 3;
  EXPECT_NE(result, other);
}

TEST(IntegerParser, Empty) {
  EXPECT_THAT(IntegerParser::Parse(""), ParseErrorIs(HasSubstr("Unexpected end of string")));
  EXPECT_THAT(IntegerParser::ParseHex(""), ParseErrorIs(HasSubstr("Unexpected end of string")));
}

TEST(IntegerParser, Integers) {
  EXPECT_THAT(IntegerParser::Parse("0"), ParseResultIs(Result{0, 1}));
  EXPECT_THAT(IntegerParser::Parse("1"), ParseResultIs(Result{1, 1}));
  EXPECT_THAT(IntegerParser::Parse("4294967295"), ParseResultIs(Result{4294967295, 10}));
  // UINT32_MAX + 1
  EXPECT_THAT(IntegerParser::Parse("4294967296"), ParseErrorIs(HasSubstr("Integer overflow")));
}

TEST(IntegerParser, HexIntegers) {
  EXPECT_THAT(IntegerParser::ParseHex("0"), ParseResultIs(Result{0, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("1"), ParseResultIs(Result{1, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("A"), ParseResultIs(Result{10, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("a"), ParseResultIs(Result{10, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("F"), ParseResultIs(Result{15, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("f"), ParseResultIs(Result{15, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("FFFF"), ParseResultIs(Result{65535, 4}));
  EXPECT_THAT(IntegerParser::ParseHex("FFFFFFFF"), ParseResultIs(Result{4294967295, 8}));
  EXPECT_THAT(IntegerParser::ParseHex("4294967295"), ParseErrorIs(HasSubstr("Integer overflow")));
  // UINT32_MAX + 1
  EXPECT_THAT(IntegerParser::ParseHex("100000000"), ParseErrorIs(HasSubstr("Integer overflow")));
}

TEST(IntegerParser, Signs) {
  EXPECT_THAT(IntegerParser::Parse("+0"), ParseErrorIs(HasSubstr("Unexpected character")));
  EXPECT_THAT(IntegerParser::Parse("-0"), ParseErrorIs(HasSubstr("Unexpected character")));
  EXPECT_THAT(IntegerParser::ParseHex("+0"), ParseErrorIs(HasSubstr("Unexpected character")));
  EXPECT_THAT(IntegerParser::ParseHex("-0"), ParseErrorIs(HasSubstr("Unexpected character")));
}

TEST(IntegerParser, Decimal) {
  EXPECT_THAT(IntegerParser::Parse("."), ParseErrorIs(HasSubstr("Unexpected character")));
  EXPECT_THAT(IntegerParser::Parse(".0"), ParseErrorIs(HasSubstr("Unexpected character")));
  EXPECT_THAT(IntegerParser::ParseHex("."), ParseErrorIs(HasSubstr("Unexpected character")));
  EXPECT_THAT(IntegerParser::ParseHex(".0"), ParseErrorIs(HasSubstr("Unexpected character")));

  // The dot at the end of the number should be where parsing stops.
  EXPECT_THAT(IntegerParser::Parse("0."), ParseResultIs(Result{0, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("0."), ParseResultIs(Result{0, 1}));
}

TEST(IntegerParser, Exponent) {
  // Zero exponent is valid.
  EXPECT_THAT(IntegerParser::Parse("1e0"), ParseResultIs(Result{1, 1}));
  // Uppercase exponent character.
  EXPECT_THAT(IntegerParser::Parse("10E2"), ParseResultIs(Result{10, 2}));

  // For hex numbers, the 'e' character is parsed as a normal digit
  EXPECT_THAT(IntegerParser::ParseHex("1e0"), ParseResultIs(Result{480, 3}));
  EXPECT_THAT(IntegerParser::ParseHex("10E2"), ParseResultIs(Result{4322, 4}));
}

TEST(IntegerParser, StopsParsingAtCharacter) {
  EXPECT_THAT(IntegerParser::Parse("100L200"), ParseResultIs(Result{100, 3}));
  EXPECT_THAT(IntegerParser::ParseHex("100L200"), ParseResultIs(Result{256, 3}));
  EXPECT_THAT(IntegerParser::Parse("1e1M1"), ParseResultIs(Result{1, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("1e1M1"), ParseResultIs(Result{481, 3}));
  EXPECT_THAT(IntegerParser::Parse("13,000.56"), ParseResultIs(Result{13, 2}));
  EXPECT_THAT(IntegerParser::ParseHex("13,000.56"), ParseResultIs(Result{19, 2}));

  EXPECT_THAT(IntegerParser::Parse("1e"), ParseResultIs(Result{1, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("1e"), ParseResultIs(Result{30, 2}));
  EXPECT_THAT(IntegerParser::Parse("1e-"), ParseResultIs(Result{1, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("1e-"), ParseResultIs(Result{30, 2}));
  EXPECT_THAT(IntegerParser::Parse("1e.3"), ParseResultIs(Result{1, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("1e.3"), ParseResultIs(Result{30, 2}));
  EXPECT_THAT(IntegerParser::Parse("1e2.3"), ParseResultIs(Result{1, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("1e2.3"), ParseResultIs(Result{482, 3}));
}

TEST(IntegerParser, HexNoPrefix) {
  EXPECT_THAT(IntegerParser::Parse("0x1"), ParseResultIs(Result{0, 1}));
  EXPECT_THAT(IntegerParser::Parse("0X2"), ParseResultIs(Result{0, 1}));

  EXPECT_THAT(IntegerParser::ParseHex("0x1"), ParseResultIs(Result{0, 1}));
  EXPECT_THAT(IntegerParser::ParseHex("0X2"), ParseResultIs(Result{0, 1}));
}

}  // namespace donner::base::parser
