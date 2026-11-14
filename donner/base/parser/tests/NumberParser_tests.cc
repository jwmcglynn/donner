#include "donner/base/parser/NumberParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <string_view>

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

/// Matches a double within one unit in the last place of \p expected.
MATCHER_P(WithinOneUlpOf, expected, "is within one ULP of " + testing::PrintToString(expected)) {
  const double ulp = std::abs(std::nextafter(expected, HUGE_VAL) - expected);
  *result_listener << "differs by " << std::abs(arg - expected) << ", one ULP is " << ulp;
  return std::abs(arg - expected) <= ulp;
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

TEST(NumberParser, HugeExponentKeepsItsSign) {
  NumberParser::Options permissive;
  permissive.forbidOutOfRange = false;
  for (const std::string_view magnitude :
       {"9223372036854775807", "9223372036854775808", "9223372036854775809", "18446744073709551615",
        "18446744073709551616", "999999999999999999999999"}) {
    SCOPED_TRACE(magnitude);
    for (const std::string_view sign : {"", "-"}) {
      const std::string positive = std::string(sign) + "0.8777771e" + std::string(magnitude);
      EXPECT_THAT(NumberParser::Parse(positive),
                  ParseErrorIs("Failed to parse number: Out of range"));
      const double infinity = std::numeric_limits<double>::infinity();
      EXPECT_THAT(NumberParser::Parse(positive, permissive),
                  ParseResultIs(Result{sign.empty() ? infinity : -infinity, positive.size()}));
      const std::string negative = std::string(sign) + "0.8777771e-" + std::string(magnitude);
      const auto underflow = NumberParser::Parse(negative);
      ASSERT_THAT(underflow, NoParseError());
      EXPECT_THAT(underflow, ParseResultIs(Result{0.0, negative.size()}));
      EXPECT_EQ(std::signbit(underflow.result().number), !sign.empty());
      EXPECT_THAT(NumberParser::Parse(negative, permissive),
                  ParseResultIs(Result{0.0, negative.size()}));
      const std::string zero = std::string(sign) + "0e" + std::string(magnitude);
      const auto zeroResult = NumberParser::Parse(zero);
      ASSERT_THAT(zeroResult, NoParseError());
      EXPECT_THAT(zeroResult, ParseResultIs(Result{0.0, zero.size()}));
      EXPECT_EQ(std::signbit(zeroResult.result().number), !sign.empty());
    }
  }
}

TEST(NumberParser, LongIntegerExponentCancellationPrecedesClamp) {
  const std::string integer = "1" + std::string(25000, '0');
  const std::string input = integer + "e-25000";
  const auto parsed = NumberParser::Parse(input);
  ASSERT_THAT(parsed, NoParseError());
  EXPECT_EQ(parsed.result().consumedChars, input.size());
  EXPECT_THAT(parsed.result().number, testing::DoubleNear(1.0, 1e-14));
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

// ---------------------------------------------------------------------------
// Range-accuracy tests: verify that error SourceRanges cover the right span.
// ---------------------------------------------------------------------------

TEST(NumberParser, RangeEmpty) {
  // Empty string => EndOfString for both start and end.
  EXPECT_THAT(NumberParser::Parse(""), ParseErrorEndOfString());
}

TEST(NumberParser, RangeUnexpectedCharacter) {
  // Non-digit at position 0 => range [0,1).
  EXPECT_THAT(NumberParser::Parse("abc"), ParseErrorRange(0, 1));
  // "." alone => no digits, '.' is at offset 0 => [0,1).
  EXPECT_THAT(NumberParser::Parse("."), ParseErrorRange(0, 1));
}

TEST(NumberParser, RangeSignOnly) {
  // "+" or "-" with nothing after => sign is consumed, error points at sign position.
  EXPECT_THAT(NumberParser::Parse("+"), ParseErrorRange(0, 1));
  EXPECT_THAT(NumberParser::Parse("-"), ParseErrorRange(0, 1));
}

TEST(NumberParser, RangeInvalidSign) {
  // "+-" and "-+" => Invalid sign, range covers both sign chars [0,2).
  EXPECT_THAT(NumberParser::Parse("+-0"), ParseErrorRange(0, 2));
  EXPECT_THAT(NumberParser::Parse("-+0"), ParseErrorRange(0, 2));
  EXPECT_THAT(NumberParser::Parse("+-"), ParseErrorRange(0, 2));
  EXPECT_THAT(NumberParser::Parse("-+"), ParseErrorRange(0, 2));
}

TEST(NumberParser, RangeNotFinite) {
  // "Inf" => range covers the three letters [0,3).
  EXPECT_THAT(NumberParser::Parse("Inf"), ParseErrorRange(0, 3));
  // "+Inf" => sign is 1 char, so inf range is [1,4).
  EXPECT_THAT(NumberParser::Parse("+Inf"), ParseErrorRange(1, 4));
  EXPECT_THAT(NumberParser::Parse("-Inf"), ParseErrorRange(1, 4));
  // NaN variants.
  EXPECT_THAT(NumberParser::Parse("NaN"), ParseErrorRange(0, 3));
  EXPECT_THAT(NumberParser::Parse("+NaN"), ParseErrorRange(1, 4));
  EXPECT_THAT(NumberParser::Parse("-NaN"), ParseErrorRange(1, 4));
}

TEST(NumberParser, RangeOutOfRange) {
  // Out-of-range number => range covers entire consumed input [0, totalConsumed).
  EXPECT_THAT(NumberParser::Parse("99e999999999999999"), ParseErrorRange(0, 18));
  EXPECT_THAT(NumberParser::Parse("-99e999999999999999"), ParseErrorRange(0, 19));
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

// An integer one past UINT64_MAX keeps its magnitude: the digit that does not fit raises the
// exponent instead of overflowing the mantissa.
TEST(NumberParser, DigitAddOverflow) {
  const std::string toParse = "18446744073709551619";

  const auto maybeResult = NumberParser::Parse(toParse);
  ASSERT_THAT(maybeResult, NoParseError());
  EXPECT_THAT(maybeResult.result().number, WithinOneUlpOf(18446744073709551619.0));
  EXPECT_EQ(maybeResult.result().consumedChars, toParse.size());
}

// Significands with more digits than a 64-bit mantissa holds keep their magnitude: digits past
// that capacity only shift the decimal exponent (integer part) or are dropped (fraction part).
TEST(NumberParser, LongSignificandsKeepTheirMagnitude) {
  const struct {
    std::string_view input;
    double expected;
  } kCases[] = {
      {"18.500000000000000000", 18.5},
      {"-18.500000000000000000", -18.5},
      {"1.9999999999999999999", 2.0},
      {"184467.488870955161800", 184467.488870955161800},
      {"0.123456789012345678901234567890", 0.123456789012345678901234567890},
      {"123456789012345.678901234567", 123456789012345.678901234567},
      {"9999999999999999999.99", 1e19},
      {"1844674407370955161.5", 1844674407370955161.5},
      {"18446744073709551615.5", 18446744073709551615.5},
      {"99999999999999999999.9", 1e20},
      {"18.500000000000000000e2", 1850.0},
      {"1.9999999999999999999e-3", 0.002},
      // Leading zeros do not use mantissa capacity.
      {"0000000000000000000000018.500000000000000000", 18.5},
      {"-000.00000000000000000000001234567890123456789012", -1.234567890123456789012e-23},
      {"00000000000000000000001.9999999999999999999e-3", 0.002},
  };
  for (const auto& testCase : kCases) {
    SCOPED_TRACE(testing::Message() << "Parsing: " << testCase.input);
    const auto maybeResult = NumberParser::Parse(testCase.input);
    ASSERT_THAT(maybeResult, NoParseError());
    EXPECT_THAT(maybeResult.result().number, WithinOneUlpOf(testCase.expected));
    EXPECT_EQ(maybeResult.result().consumedChars, testCase.input.size());
  }
}

// Digits that do not fit in the mantissa are still consumed, so parsing resumes after the whole
// number rather than inside it.
TEST(NumberParser, DroppedDigitsAreConsumed) {
  const struct {
    std::string_view input;
    double expected;
    size_t consumedChars;
  } kCases[] = {
      {"123456789012345678901234567890px", 123456789012345678901234567890.0, 30},
      {"18.500000000000000000000000000 20", 18.5, 30},
      {"1.99999999999999999999999e2,", 200.0, 27},
  };
  for (const auto& testCase : kCases) {
    SCOPED_TRACE(testing::Message() << "Parsing: " << testCase.input);
    const auto maybeResult = NumberParser::Parse(testCase.input);
    ASSERT_THAT(maybeResult, NoParseError());
    EXPECT_THAT(maybeResult.result().number, WithinOneUlpOf(testCase.expected));
    EXPECT_EQ(maybeResult.result().consumedChars, testCase.consumedChars);
  }
}

}  // namespace donner::parser
