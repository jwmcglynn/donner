#include "donner/base/parser/NumberParser.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

#include "donner/base/MathUtils.h"
#include "donner/base/StringUtils.h"

namespace donner::parser {

namespace {

/**
 * Converts a parsed mantissa (64-bit) and decimal exponent into a double with minimal drift.
 *
 * For small |exp10| (<=22), it uses exact multiplication or division by powers of 10
 * that are precisely representable in double, avoiding typical rounding issues like
 * 0.07900000000000001.
 *
 * If the exponent is extremely large, it falls back to std::pow(10.0, exp10) and
 * may produce small rounding differences or ±∞.
 *
 * @param mantissa up to 19 digits of integer (e.g. 79 for ".79")
 * @param exp10 decimal exponent, clamped to an `int`, so large values may produce inf or 0. (e.g.
 * -3 for ".79e-1")
 * @param negative whether the result should be negated
 * @return the double or ±∞ if out of representable range
 */
double ConvertMantissaAndExponent(uint64_t mantissa, int exp10, bool negative) {
  // Table of exact powers of 10 up to 22. Values are exactly representable in a double.
  constexpr double kExactPowersOf10[] = {
      1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
  };
  constexpr int kMaxExactExp = 22;  // Do exact multiply/divide if |exp10| <= 22

  // If no digits, return ±0.0
  if (mantissa == 0) {
    return negative ? -0.0 : 0.0;
  }

  // Convert 64-bit mantissa to double. This is exact if mantissa < 2^53.
  // If mantissa >= 2^53, we might lose a tiny bit of integer precision.
  double d = static_cast<double>(mantissa);

  // For |exp10| <= 22, do an exact multiply or divide by 10^exp10.
  // Otherwise, fallback to std::pow(10.0, exp10).
  if (exp10 >= 0 && exp10 <= kMaxExactExp) {
    d *= kExactPowersOf10[exp10];
  } else if (exp10 < 0 && -exp10 <= kMaxExactExp) {
    d /= kExactPowersOf10[-exp10];
  } else {
    // Large exponent => do standard pow. This should overflow to ±∞ if exp10 is huge.
    d *= std::pow(10.0, static_cast<double>(exp10));
  }

  // If we ended up out-of-range => ±∞
  if (!std::isfinite(d)) {
    return negative ? -std::numeric_limits<double>::infinity()
                    : std::numeric_limits<double>::infinity();
  }

  return negative ? -d : d;
}

/**
 * Holds the result of parsing digits from a string.
 */
struct ParseDigitsResult {
  uint64_t value = 0;         ///< Parsed numeric value.
  size_t valueNumDigits = 0;  ///< The total number of digits contributing to the value (base 10).
  size_t consumedChars = 0;   ///< The number of characters consumed from the input.
};

/**
 * Helper to parse multiple digits. Saturates if it overflows 64-bit.
 *
 * @param str The string to parse.
 * @return Result structure containing the parsed value, number of (base 10) digits in the value,
 * and number of characters consumed.
 */
ParseDigitsResult ParseDigitsSaturating(std::string_view str) {
  ParseDigitsResult result;

  // The largest safe place to multiply by 10 without overflow is `UINT64_MAX / 10`.
  static constexpr uint64_t kMaxBeforeMultiply = std::numeric_limits<uint64_t>::max() / 10ULL;
  bool saturated = false;

  for (size_t i = 0; i < str.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(str[i]);
    if (!std::isdigit(c)) {
      break;
    }

    // If about to overflow, saturate the result value
    if (result.value > kMaxBeforeMultiply) {
      saturated = true;
    }

    if (saturated) {
      ++result.consumedChars;
      continue;
    }

    const uint64_t nextVal = result.value * 10ULL + (c - '0');

    // If we detect overflow on addition, saturate
    if (nextVal < result.value) {
      saturated = true;
    } else {
      result.value = nextVal;
      ++result.valueNumDigits;
    }

    ++result.consumedChars;
  }

  return result;
}

}  // namespace

ParseResult<NumberParser::Result> NumberParser::Parse(std::string_view str, Options options) {
  // Keep an original copy for offset calculations or error messages.
  const std::string_view originalStr = str;

  // If empty => "Unexpected end of string".
  if (str.empty()) {
    ParseError err;
    err.reason = "Failed to parse number: Unexpected end of string";
    err.location = FileOffset::Offset(0);
    return err;
  }

  // Short-circuit for "0x" or "0X".
  if (str.size() >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
    Result r;
    r.number = 0.0;
    r.consumedChars = 1;  // Only the '0' is consumed
    return r;
  }

  // Detect leading plus or minus. We do *not* finalize that sign consumption unless we find digits,
  // but to match the existing code & partial parse behavior, we'll remove it now and handle empty
  // below.
  const bool foundPlus = (str.front() == '+');
  const bool negative = (str.front() == '-');
  size_t signChars = 0;
  if (foundPlus || negative) {
    signChars = 1;
    str.remove_prefix(1);
  }

  // If we consumed a sign, but there's nothing left => "Unexpected character" (vs. "Unexpected end
  // of string").
  if (str.empty()) {
    ParseError err;
    err.reason = "Failed to parse number: Unexpected character";
    // The offset is the position of that sign in the original string (i.e. 0).
    err.location = FileOffset::Offset(static_cast<int64_t>(originalStr.size() - str.size() - 1));
    return err;
  }

  // If we removed one sign and now see another => "Invalid sign", e.g. "+-0" or "-+0".
  // This check matches the existing logic: if there's a second sign, it's invalid.
  {
    const bool secondPlus = (str.front() == '+');
    const bool secondMinus = (str.front() == '-');
    if (foundPlus && secondMinus) {
      // We saw '+' then '-'
      ParseError err;
      err.reason = "Failed to parse number: Invalid sign";
      // That '-' is at offset=1 in the original string.
      err.location = FileOffset::Offset(1);
      return err;
    }
    if (!foundPlus && negative && (secondPlus || secondMinus)) {
      // We saw '-' then another sign
      ParseError err;
      err.reason = "Failed to parse number: Invalid sign";
      // That second sign is offset=1 if the original started with '-'.
      err.location = FileOffset::Offset(1);
      return err;
    }
  }

  // Check for Inf or NaN (case-insensitive).
  {
    if (StringUtils::StartsWith<StringComparison::IgnoreCase>(str, std::string_view("inf"))) {
      ParseError err;
      err.reason = "Failed to parse number: Not finite";
      err.location = FileOffset::Offset(static_cast<int64_t>(signChars));
      return err;
    }

    if (StringUtils::StartsWith<StringComparison::IgnoreCase>(str, std::string_view("nan"))) {
      ParseError err;
      err.reason = "Failed to parse number: Not finite";
      err.location = FileOffset::Offset(static_cast<int64_t>(signChars));
      return err;
    }
  }

  // Parse the integer part
  size_t totalConsumed = signChars;

  // Digits before decimal
  const ParseDigitsResult digitsResult = ParseDigitsSaturating(str);
  str.remove_prefix(digitsResult.consumedChars);
  totalConsumed += digitsResult.consumedChars;

  uint64_t intPart = digitsResult.value;
  bool anyDigits = (digitsResult.consumedChars > 0);

  // Fraction part, starting with a '.'
  uint64_t fracPart = 0;
  size_t fracDigits = 0;

  if (!str.empty() && str.front() == '.') {
    // Only consume '.' if there's at least 1 digit after it
    if (str.size() > 1 && std::isdigit(str[1])) {
      str.remove_prefix(1);
      totalConsumed++;

      const ParseDigitsResult fracResult = ParseDigitsSaturating(str);
      str.remove_prefix(fracResult.consumedChars);
      totalConsumed += fracResult.consumedChars;
      fracDigits = fracResult.valueNumDigits;

      fracPart = fracResult.value;

      assert(fracResult.consumedChars > 0 && "Fraction parse should consume at least 1 digit");
      anyDigits = true;
    } else {
      // Partial parse => don't consume '.'
    }
  }

  // If no digits at all => error. (Matches tests expecting "Unexpected character" for "+", "-")
  if (!anyDigits) {
    ParseError err;
    err.reason = "Failed to parse number: Unexpected character";
    // The offset is the location where we started to parse digits:
    // i.e. signChars if we had a sign, else 0
    err.location = FileOffset::Offset(static_cast<int64_t>(signChars));
    return err;
  }

  // 3) Optionally parse exponent
  // If next char is 'e' or 'E', check if there's a digit after optional sign.
  int64_t exponentVal = 0;

  if (!str.empty() && (str.front() == 'e' || str.front() == 'E')) {
    // We'll only consume exponent if there's a digit after optional sign.
    if (str.size() > 1) {
      bool expNeg = false;
      bool validExponent = false;
      size_t consumeCount = 1;  // At least consume 'e' if valid

      char nextChar = str[1];
      if (nextChar == '+' || nextChar == '-') {
        // Only accept sign if there's a digit after that
        if (str.size() > 2 && std::isdigit(static_cast<unsigned char>(str[2]))) {
          expNeg = (nextChar == '-');
          consumeCount = 2;  // 'e' + sign
          validExponent = true;
        }
      } else if (std::isdigit(static_cast<unsigned char>(nextChar))) {
        validExponent = true;
      }

      if (validExponent) {
        // Consume 'e' and optional sign
        str.remove_prefix(consumeCount);
        totalConsumed += consumeCount;

        // Parse exponent digits
        const ParseDigitsResult expResult = ParseDigitsSaturating(str);
        str.remove_prefix(expResult.consumedChars);
        totalConsumed += expResult.consumedChars;

        exponentVal = static_cast<int64_t>(expNeg ? -expResult.value : expResult.value);
      }
    }
    // else "1e" => partial parse => ignore
  }

  // Combine integer + fraction
  // e.g. 12.34 => intPart=1234, fracDigits=2 => exponentVal-=2
  // e.g. 0.79  => intPart=79,   fracDigits=2 => exponentVal-=2
  // e.g. 59.6078... => parse large fraction
  for (size_t i = 0; i < fracDigits; i++) {
    if (intPart < std::numeric_limits<uint64_t>::max() / 10ULL) {
      intPart = intPart * 10ULL;
    } else {
      // Truncate fracPart to the used digits
      const size_t truncateFrac = fracDigits - i;
      for (size_t j = 0; j < truncateFrac; j++) {
        fracPart /= 10ULL;
      }

      fracDigits = i;
      break;
    }
  }

  uint64_t mantissa = intPart;
  if (mantissa < std::numeric_limits<uint64_t>::max()) {
    // If we didn't already saturate, we can safely add
    // If `intPart` was max, adding `fracPart` won't matter (already saturate)
    if (mantissa + fracPart < mantissa) {
      mantissa = std::numeric_limits<uint64_t>::max();
    } else {
      mantissa += fracPart;
    }
  }

  // Adjust exponent for the number of digits consumed
  exponentVal += static_cast<int64_t>(digitsResult.consumedChars - digitsResult.valueNumDigits);
  exponentVal -= static_cast<int64_t>(fracDigits);

  // For extremely large exponentVal (e.g. 1e999999999999999999), we'd normally do
  // pow(10, huge_double) => inf. But if exponentVal won't fit in an int => it overflows.
  // We'll clamp exponentVal so it doesn't overflow an int, and then rely on pow(10.0, large)
  // to produce ±∞. Then the parse logic below (checking isfinite) can produce a parse error or ±∞.
  //
  // If forbidOutOfRange==true, large exponent => "Out of range".
  // else => ±∞. We'll detect that after computing the final value.

  // Bound exponentVal to a safe range for a subsequent cast to int, small enough to yield 0 and
  // large enough to yield inf.
  exponentVal = Clamp<int64_t>(exponentVal, -20000, 20000);

  // Do the final double computation
  const double finalVal =
      ConvertMantissaAndExponent(mantissa, static_cast<int>(exponentVal), negative);

  assert(!std::isnan(finalVal) && "Final value should not be NaN");

  // If infinite => "Out of range"
  if (!std::isfinite(finalVal)) {
    if (options.forbidOutOfRange) {
      ParseError err;
      err.reason = "Failed to parse number: Out of range";
      err.location = FileOffset::Offset(static_cast<int64_t>(totalConsumed));
      return err;
    } else {
      Result r;
      r.number = (finalVal < 0.0) ? -std::numeric_limits<double>::infinity()
                                  : std::numeric_limits<double>::infinity();
      r.consumedChars = totalConsumed;
      return r;
    }
  }

  // Return success
  Result out;
  out.number = finalVal;
  out.consumedChars = totalConsumed;
  return out;
}

}  // namespace donner::parser
