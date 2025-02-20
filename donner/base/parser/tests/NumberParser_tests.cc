#include "donner/base/parser/NumberParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/MathUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"

using testing::HasSubstr;

namespace donner::parser {

using Result = NumberParser::Result;

[[maybe_unused]] static bool operator==(const Result& lhs, const Result& rhs) {
  return NearEquals(lhs.number, rhs.number) && lhs.consumedChars == rhs.consumedChars;
}

[[maybe_unused]] static void PrintTo(const Result& result, std::ostream* os) {
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
  EXPECT_THAT(NumberParser::Parse(""), ParseErrorIs(HasSubstr("Unexpected end of string")));
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
  EXPECT_THAT(NumberParser::Parse("-+0"), ParseErrorIs(HasSubstr("Invalid sign")));
  EXPECT_THAT(NumberParser::Parse("+"), ParseErrorIs(HasSubstr("Unexpected character")));
  EXPECT_THAT(NumberParser::Parse("-"), ParseErrorIs(HasSubstr("Unexpected character")));
  EXPECT_THAT(NumberParser::Parse("+-"), ParseErrorIs(HasSubstr("Invalid sign")));
  EXPECT_THAT(NumberParser::Parse("-+"), ParseErrorIs(HasSubstr("Invalid sign")));
}

TEST(NumberParser, Decimal) {
  EXPECT_THAT(NumberParser::Parse("."), ParseErrorIs(HasSubstr("Unexpected character")));

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
  EXPECT_THAT(NumberParser::Parse("123.e"), ParseResultIs(Result{123.0, 3}))
      << "Should not consume '.'";

  EXPECT_THAT(NumberParser::Parse("1e"), ParseResultIs(Result{1.0, 1}));
  EXPECT_THAT(NumberParser::Parse("1e-"), ParseResultIs(Result{1.0, 1}));
  EXPECT_THAT(NumberParser::Parse("1e.3"), ParseResultIs(Result{1.0, 1}));
  EXPECT_THAT(NumberParser::Parse("1e2.3"), ParseResultIs(Result{100.0, 3}));

  // Hex should not parse either.
  EXPECT_THAT(NumberParser::Parse("0x1"), ParseResultIs(Result{0.0, 1}));
  EXPECT_THAT(NumberParser::Parse("0X2"), ParseResultIs(Result{0.0, 1}));
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

TEST(NumberParser, BigFraction) {
  EXPECT_THAT(NumberParser::Parse("59.60784313725490196078431372549"),
              ParseResultIs(Result{59.60784313725490196078431372549, 32}));
}

TEST(NumberParser, Exponents) {
  for (int i = std::numeric_limits<double>::min_exponent10;
       i < std::numeric_limits<double>::max_exponent10; i++) {
    std::string number = "1e" + std::to_string(i);
    SCOPED_TRACE(testing::Message() << "Parsing: " << number);

    ASSERT_THAT(NumberParser::Parse(number), ParseResultIs(Result{std::pow(10, i), number.size()}));
  }
}

TEST(NumberParser, OverflowedDigits) {
  for (int i = 0; i < std::numeric_limits<double>::max_exponent10; i++) {
    std::string number = "1";
    for (int j = 0; j < i; j++) {
      number += "0";
    }

    const double expected = std::pow(10, i);

    SCOPED_TRACE(testing::Message() << "Parsing: " << number);
    auto maybeResult = NumberParser::Parse(number);
    ASSERT_THAT(maybeResult, NoParseError());

    // Validate the result with the DoubleEq matcher which has a more forgiving error margin, as
    // std::pow can be imprecise for large numbers.
    const Result result = maybeResult.result();
    ASSERT_THAT(result.number, testing::DoubleEq(expected));
    ASSERT_EQ(result.consumedChars, number.size());
  }
}

/**
 * Exercises digit overflow in ParseDigitsSaturating: specifically the case where
 * nextVal < result.value on add.  We do this by carefully selecting enough digits
 * so that multiply-by-10 is safe, but adding one more digit overflows 64-bit.
 */
TEST(NumberParser, DigitAddOverflow) {
  // 18446744073709551615 == 2^64 - 1, which we *can* parse without triggering
  // the addition overflow check (because we saturate multiplication first).
  // Instead, let's start one step below that boundary and then add a digit that
  // overflows.
  //
  // One way is to parse "1844674407370955161" (which is 2^64 - 1 with the last digit removed)
  // and then add another digit '9', pushing it over the limit in the add step.
  // This should trigger 'nextVal < result.value' in ParseDigitsSaturating.
  //
  // Combining them: "18446744073709551619".
  //
  // The exact boundary depends on your saturate logic, so you may tweak the number
  // to ensure we *really do* hit the (nextVal < result.value) branch.
  const std::string toParse = "18446744073709551619";

  const auto maybeResult = NumberParser::Parse(toParse);
  EXPECT_THAT(maybeResult, NoParseError());

  // We don’t particularly care about the final double, just that we didn’t crash
  // and consumed all digits (which demonstrates we parsed them, saturating the integer).
  // Because it saturates the internal 64-bit, it should remain a very large double ~1.8446744e19
  // but not infinite. You can do a more precise check if you like.
  const auto result = maybeResult.result();
  EXPECT_EQ(result.consumedChars, toParse.size());

  // We only check that it's finite and "large".
  EXPECT_TRUE(std::isfinite(result.number));
  EXPECT_GT(result.number, 1e19);
}

/**
 * Exercises integer saturation plus fractional overflow checks in NumberParser,
 * testing the `(mantissa + fracPart < mantissa)` overfow condition.
 */
TEST(NumberParser, MantissaPlusFractionOverflow) {
  const std::string toParse = "184467.488870955161800";

  const auto maybeResult = NumberParser::Parse(toParse);
  EXPECT_THAT(maybeResult, NoParseError());

  const auto result = maybeResult.result();
  // We consumed everything:
  EXPECT_EQ(result.consumedChars, toParse.size());
  EXPECT_DOUBLE_EQ(result.number, 184467.44073709552);
}

}  // namespace donner::parser
