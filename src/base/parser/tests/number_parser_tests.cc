#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/number_parser.h"
#include "src/base/parser/tests/parse_result_test_utils.h"

using testing::HasSubstr;

namespace donner {

using Result = NumberParser::Result;

static bool operator==(const Result& lhs, const Result& rhs) {
  return lhs.number == rhs.number && lhs.consumedChars == rhs.consumedChars;
}

static void PrintTo(const Result& result, std::ostream* os) {
  *os << "(" << result.number << ", consumed: " << result.consumedChars << ")";
}

TEST(NumberParser, TestHelpers) {
  NumberParser::Result result;
  result.consumedChars = 1;
  result.number = 2.0;

  EXPECT_EQ(testing::PrintToString(result), "(2, consumed: 1)");

  NumberParser::Result other;
  other.consumedChars = 1;
  other.number = 2.0;
  EXPECT_EQ(result, other);

  other.consumedChars = 2;
  EXPECT_NE(result, other);

  other.consumedChars = 1;
  other.number = -2.0;
  EXPECT_NE(result, other);
}

TEST(NumberParser, Empty) {
  EXPECT_THAT(NumberParser::Parse(""), ParseErrorIs(HasSubstr("Invalid argument")));
}

TEST(NumberParser, Integers) {
  EXPECT_THAT(NumberParser::Parse("0"), ParseResultIs(Result{0.0, 1}));
  EXPECT_THAT(NumberParser::Parse("1"), ParseResultIs(Result{1.0, 1}));
  EXPECT_THAT(NumberParser::Parse("4294967295"), ParseResultIs(Result{4294967295.0, 10}));
  // UINT32_MAX + 1
  EXPECT_THAT(NumberParser::Parse("4294967296"), ParseResultIs(Result{4294967296.0, 10}));
}

TEST(NumberParser, Signs) {
  EXPECT_THAT(NumberParser::Parse("+0"), ParseResultIs(Result{0.0, 2}));
  EXPECT_THAT(NumberParser::Parse("-0"), ParseResultIs(Result{0.0, 2}));
  EXPECT_THAT(NumberParser::Parse("+-0"), ParseErrorIs(HasSubstr("Invalid sign")));
  EXPECT_THAT(NumberParser::Parse("-+0"), ParseErrorIs(HasSubstr("Invalid argument")));
  EXPECT_THAT(NumberParser::Parse("+"), ParseErrorIs(HasSubstr("Invalid argument")));
  EXPECT_THAT(NumberParser::Parse("-"), ParseErrorIs(HasSubstr("Invalid argument")));
  EXPECT_THAT(NumberParser::Parse("+-"), ParseErrorIs(HasSubstr("Invalid argument")));
  EXPECT_THAT(NumberParser::Parse("-+"), ParseErrorIs(HasSubstr("Invalid argument")));
}

TEST(NumberParser, Decimal) {
  EXPECT_THAT(NumberParser::Parse("."), ParseErrorIs(HasSubstr("Invalid argument")));

  // Zero decimal digits before the dot are allowed.
  EXPECT_THAT(NumberParser::Parse(".0"), ParseResultIs(Result{0.0, 2}));
  EXPECT_THAT(NumberParser::Parse(".1"), ParseResultIs(Result{0.1, 2}));
  EXPECT_THAT(NumberParser::Parse("-.1"), ParseResultIs(Result{-0.1, 3}));
  EXPECT_THAT(NumberParser::Parse("+.1"), ParseResultIs(Result{0.1, 3}));

  // Numbers ending with a dot are out-of-spec, they should be parsed up until the dot.
  EXPECT_THAT(NumberParser::Parse("0."), ParseResultIs(Result{0.0, 1}));

  // Per the SVG BNF, https://www.w3.org/TR/SVG/paths.html#PathDataBNF, 0.6.5 should parse as 0.6
  // and 0.5:
  EXPECT_THAT(NumberParser::Parse("0.6.5"), ParseResultIs(Result{0.6, 3}));
  EXPECT_THAT(NumberParser::Parse(".5"), ParseResultIs(Result{0.5, 2}));
}

TEST(NumberParser, Exponent) {
  // Zero exponent is valid.
  EXPECT_THAT(NumberParser::Parse("1e0"), ParseResultIs(Result{1.0, 3}));
  EXPECT_THAT(NumberParser::Parse("-1e0"), ParseResultIs(Result{-1.0, 4}));
  EXPECT_THAT(NumberParser::Parse("1e+0"), ParseResultIs(Result{1.0, 4}));
  EXPECT_THAT(NumberParser::Parse("-1e+0"), ParseResultIs(Result{-1.0, 5}));
  EXPECT_THAT(NumberParser::Parse("1e-0"), ParseResultIs(Result{1.0, 4}));
  EXPECT_THAT(NumberParser::Parse("-1e-0"), ParseResultIs(Result{-1.0, 5}));

  // Standard cases.
  EXPECT_THAT(NumberParser::Parse("1e1"), ParseResultIs(Result{10.0, 3}));
  EXPECT_THAT(NumberParser::Parse("-1e1"), ParseResultIs(Result{-10.0, 4}));
  EXPECT_THAT(NumberParser::Parse("1e+1"), ParseResultIs(Result{10.0, 4}));
  EXPECT_THAT(NumberParser::Parse("-1e+1"), ParseResultIs(Result{-10.0, 5}));
  EXPECT_THAT(NumberParser::Parse("1e2"), ParseResultIs(Result{100.0, 3}));
  EXPECT_THAT(NumberParser::Parse("1e-2"), ParseResultIs(Result{0.01, 4}));
  EXPECT_THAT(NumberParser::Parse("+1e2"), ParseResultIs(Result{100.0, 4}));
  EXPECT_THAT(NumberParser::Parse("-1e2"), ParseResultIs(Result{-100.0, 4}));
  EXPECT_THAT(NumberParser::Parse("-1e-2"), ParseResultIs(Result{-0.01, 5}));

  // Uppercase exponent character.
  EXPECT_THAT(NumberParser::Parse("1E2"), ParseResultIs(Result{100.0, 3}));
}

TEST(NumberParser, StopsParsingAtCharacter) {
  EXPECT_THAT(NumberParser::Parse("100L200"), ParseResultIs(Result{100.0, 3}));
  EXPECT_THAT(NumberParser::Parse("1e1M1"), ParseResultIs(Result{10.0, 3}));
  EXPECT_THAT(NumberParser::Parse("13,000.56"), ParseResultIs(Result{13.0, 2}));

  EXPECT_THAT(NumberParser::Parse("1e"), ParseResultIs(Result{1.0, 1}));
  EXPECT_THAT(NumberParser::Parse("1e-"), ParseResultIs(Result{1.0, 1}));
  EXPECT_THAT(NumberParser::Parse("1e.3"), ParseResultIs(Result{1.0, 1}));
  EXPECT_THAT(NumberParser::Parse("1e2.3"), ParseResultIs(Result{100.0, 3}));

  // Hex should not parse either.
  EXPECT_THAT(NumberParser::Parse("0x1"), ParseResultIs(Result{0.0, 1}));
}

TEST(NumberParser, InfAndNaN) {
  EXPECT_THAT(NumberParser::Parse("Inf"), ParseErrorIs(HasSubstr("Not finite")));
  EXPECT_THAT(NumberParser::Parse("+Inf"), ParseErrorIs(HasSubstr("Not finite")));
  EXPECT_THAT(NumberParser::Parse("-Inf"), ParseErrorIs(HasSubstr("Not finite")));
  EXPECT_THAT(NumberParser::Parse("NaN"), ParseErrorIs(HasSubstr("Not finite")));
  EXPECT_THAT(NumberParser::Parse("+NaN"), ParseErrorIs(HasSubstr("Not finite")));
  EXPECT_THAT(NumberParser::Parse("-NaN"), ParseErrorIs(HasSubstr("Not finite")));

  EXPECT_THAT(NumberParser::Parse("99e999999999999999"), ParseErrorIs(HasSubstr("Out of range")));
  EXPECT_THAT(NumberParser::Parse("-99e999999999999999"), ParseErrorIs(HasSubstr("Out of range")));
}

TEST(NumberParser, AllowOutOfRange) {
  NumberParser::Options options;
  options.forbidOutOfRange = false;

  // Still don't allow Inf/NaN.
  EXPECT_THAT(NumberParser::Parse("Inf", options), ParseErrorIs(HasSubstr("Not finite")));
  EXPECT_THAT(NumberParser::Parse("+Inf", options), ParseErrorIs(HasSubstr("Not finite")));
  EXPECT_THAT(NumberParser::Parse("-Inf", options), ParseErrorIs(HasSubstr("Not finite")));
  EXPECT_THAT(NumberParser::Parse("NaN", options), ParseErrorIs(HasSubstr("Not finite")));
  EXPECT_THAT(NumberParser::Parse("+NaN", options), ParseErrorIs(HasSubstr("Not finite")));
  EXPECT_THAT(NumberParser::Parse("-NaN", options), ParseErrorIs(HasSubstr("Not finite")));

  // Allow large numbers that become inf.
  EXPECT_THAT(NumberParser::Parse("99e999999999999999", options),
              ParseResultIs(Result{std::numeric_limits<double>::infinity(), 18}));
  EXPECT_THAT(NumberParser::Parse("+99e999999999999999", options),
              ParseResultIs(Result{std::numeric_limits<double>::infinity(), 19}));
  EXPECT_THAT(NumberParser::Parse("-99e999999999999999", options),
              ParseResultIs(Result{-std::numeric_limits<double>::infinity(), 19}));
}

}  // namespace donner
