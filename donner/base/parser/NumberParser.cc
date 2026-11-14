#include "donner/base/parser/NumberParser.h"

#include <algorithm>
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
  uint64_t value = 0;        ///< Parsed numeric value.
  size_t consumedChars = 0;  ///< The number of characters consumed from the input.
  bool saturated = false;    ///< Further digits exceeded the stored magnitude.
};

/**
 * Helper to parse multiple digits. Saturates if it overflows 64-bit.
 *
 * @param str The string to parse.
 * @return Result structure containing the parsed value and number of characters consumed.
 */
ParseDigitsResult ParseDigitsSaturating(std::string_view str) {
  ParseDigitsResult result;

  // The largest safe place to multiply by 10 without overflow is `UINT64_MAX / 10`.
  static constexpr uint64_t kMaxBeforeMultiply = std::numeric_limits<uint64_t>::max() / 10ULL;
  bool saturated = false;

  for (size_t i = 0; i < str.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(str[i]);
    if (!std::isdigit(static_cast<unsigned char>(c))) {
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
    }

    ++result.consumedChars;
  }

  result.saturated = saturated;
  return result;
}

/**
 * Decimal significand digits gathered into a 64-bit mantissa. Once a digit does not fit, it and
 * every later digit are dropped: a dropped integer digit multiplies the value by ten, and a dropped
 * fraction digit only loses precision, so the magnitude is preserved.
 */
struct Significand {
  uint64_t mantissa = 0;            ///< The digits kept so far.
  bool full = false;                ///< A digit did not fit, so later digits are dropped.
  size_t droppedIntegerDigits = 0;  ///< Integer digits past capacity, each a factor of ten.
  size_t keptFractionDigits = 0;    ///< Fraction digits in the mantissa, each a factor of 1/10.
};

/**
 * Appends the run of decimal digits at the start of \p str to \p significand.
 *
 * @param str The string to parse.
 * @param fraction Whether the digits follow the decimal point.
 * @param significand The significand to extend.
 * @return The number of characters consumed.
 */
size_t AppendDigits(std::string_view str, bool fraction, Significand& significand) {
  static constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  size_t consumed = 0;
  while (consumed < str.size() && std::isdigit(static_cast<unsigned char>(str[consumed]))) {
    const uint64_t digit = static_cast<uint64_t>(str[consumed] - '0');
    if (!significand.full && significand.mantissa <= (kMax - digit) / 10ULL) {
      significand.mantissa = significand.mantissa * 10ULL + digit;
      if (fraction) {
        ++significand.keptFractionDigits;
      }
    } else {
      significand.full = true;
      if (!fraction) {
        ++significand.droppedIntegerDigits;
      }
    }
    ++consumed;
  }
  return consumed;
}

/// Combines decimal scales before clamping, retaining cancellation for long integer inputs.
int AdjustExponent(uint64_t magnitude, bool negative, bool overflow, size_t integerDigits,
                   size_t fractionDigits) {
  constexpr uint64_t kLimit = 20000;
  // An exponent beyond uint64_t dominates the digit count of an addressable input string.
  if (overflow) {
    return negative ? -static_cast<int>(kLimit) : static_cast<int>(kLimit);
  }
  const bool adjustmentNegative = fractionDigits > integerDigits;
  const uint64_t adjustment =
      adjustmentNegative ? fractionDigits - integerDigits : integerDigits - fractionDigits;
  if (negative != adjustmentNegative) {
    if (magnitude < adjustment) {
      magnitude = adjustment - magnitude;
      negative = adjustmentNegative;
    } else {
      magnitude -= adjustment;
    }
  } else if (adjustment > std::numeric_limits<uint64_t>::max() - magnitude) {
    magnitude = kLimit;
  } else {
    magnitude += adjustment;
  }
  const int bounded = static_cast<int>(std::min(magnitude, kLimit));
  return negative ? -bounded : bounded;
}

}  // namespace

ParseResult<NumberParser::Result> NumberParser::Parse(std::string_view str, Options options) {
  // Keep an original copy for offset calculations or error messages.
  const std::string_view originalStr = str;

  // If empty => "Unexpected end of string".
  if (str.empty()) {
    return ParseDiagnostic::Error("Failed to parse number: Unexpected end of string",
                                  FileOffset::EndOfString());
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

  // If we consumed a sign, but there's nothing left => "Unexpected character".
  if (str.empty()) {
    const size_t signPos = originalStr.size() - 1;
    return ParseDiagnostic::Error(
        "Failed to parse number: Unexpected character",
        SourceRange{FileOffset::Offset(signPos), FileOffset::Offset(signPos + 1)});
  }

  // If we removed one sign and now see another => "Invalid sign", e.g. "+-0" or "-+0".
  {
    const bool secondPlus = (str.front() == '+');
    const bool secondMinus = (str.front() == '-');
    if (foundPlus && secondMinus) {
      return ParseDiagnostic::Error("Failed to parse number: Invalid sign",
                                    SourceRange{FileOffset::Offset(0), FileOffset::Offset(2)});
    }
    if (!foundPlus && negative && (secondPlus || secondMinus)) {
      return ParseDiagnostic::Error("Failed to parse number: Invalid sign",
                                    SourceRange{FileOffset::Offset(0), FileOffset::Offset(2)});
    }
  }

  // Check for Inf or NaN (case-insensitive).
  {
    if (StringUtils::StartsWith<StringComparison::IgnoreCase>(str, std::string_view("inf"))) {
      return ParseDiagnostic::Error(
          "Failed to parse number: Not finite",
          SourceRange{FileOffset::Offset(signChars), FileOffset::Offset(signChars + 3)});
    }

    if (StringUtils::StartsWith<StringComparison::IgnoreCase>(str, std::string_view("nan"))) {
      return ParseDiagnostic::Error(
          "Failed to parse number: Not finite",
          SourceRange{FileOffset::Offset(signChars), FileOffset::Offset(signChars + 3)});
    }
  }

  // Parse the integer part
  size_t totalConsumed = signChars;

  // Digits before decimal
  Significand significand;
  const size_t integerChars = AppendDigits(str, /*fraction=*/false, significand);
  str.remove_prefix(integerChars);
  totalConsumed += integerChars;

  bool anyDigits = (integerChars > 0);

  // Fraction part, starting with a '.'
  if (!str.empty() && str.front() == '.') {
    // Only consume '.' if there's at least 1 digit after it
    if (str.size() > 1 && std::isdigit(static_cast<unsigned char>(str[1]))) {
      str.remove_prefix(1);
      totalConsumed++;

      const size_t fractionChars = AppendDigits(str, /*fraction=*/true, significand);
      str.remove_prefix(fractionChars);
      totalConsumed += fractionChars;

      assert(fractionChars > 0 && "Fraction parse should consume at least 1 digit");
      anyDigits = true;
    } else {
      // Partial parse => don't consume '.'
    }
  }

  // If no digits at all => error. (Matches tests expecting "Unexpected character" for "+", "-")
  if (!anyDigits) {
    return ParseDiagnostic::Error(
        "Failed to parse number: Unexpected character",
        SourceRange{FileOffset::Offset(signChars), FileOffset::Offset(signChars + 1)});
  }

  // 3) Optionally parse exponent
  // If next char is 'e' or 'E', check if there's a digit after optional sign.
  uint64_t exponentMagnitude = 0;
  bool exponentNegative = false;
  bool exponentOverflow = false;

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

        exponentMagnitude = expResult.value;
        exponentNegative = expNeg;
        exponentOverflow = expResult.saturated;
      }
    }
    // else "1e" => partial parse => ignore
  }

  const int exponentVal =
      AdjustExponent(exponentMagnitude, exponentNegative, exponentOverflow,
                     significand.droppedIntegerDigits, significand.keptFractionDigits);

  // Do the final double computation
  const double finalVal = ConvertMantissaAndExponent(significand.mantissa, exponentVal, negative);

  assert(!std::isnan(finalVal) && "Final value should not be NaN");

  // If infinite => "Out of range"
  if (!std::isfinite(finalVal)) {
    if (options.forbidOutOfRange) {
      return ParseDiagnostic::Error(
          "Failed to parse number: Out of range",
          SourceRange{FileOffset::Offset(0), FileOffset::Offset(totalConsumed)});
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
