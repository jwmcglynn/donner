#pragma once
/// @file
/// Bounded integer arithmetic for WGSL numeric literals and constant evaluation.

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string_view>

namespace donner::gpu::shader::wgsl::number {

/// Failure class for bounded numeric conversion or arithmetic.
enum class Error : uint8_t { None, Syntax, Range, Capacity, DivisionByZero };

/// Unsigned magnitude, bounded for binary64 arithmetic and WGSL literal conversion.
struct UInt {
  static constexpr uint32_t kWords = 80;  //!< Capacity for finite binary64 intermediate values.
  std::array<uint32_t, kWords> words{};
  uint32_t used = 0;
  bool valid = true;

  /// Constructs a bounded magnitude. @param value Initial unsigned value.
  constexpr explicit UInt(uint64_t value = 0) {
    words[0] = uint32_t(value);
    words[1] = uint32_t(value >> 32);
    used = words[1] ? 2 : words[0] ? 1 : 0;
  }
  /// Removes high zero limbs from the logical magnitude.
  constexpr void trim() {
    while (used && words[used - 1] == 0) --used;
  }
  /// Returns the number of significant bits.
  constexpr uint32_t bits() const {
    if (!used) return 0;
    uint32_t value = words[used - 1], count = (used - 1) * 32;
    while (value) {
      ++count;
      value >>= 1;
    }
    return count;
  }
  /// Returns the low 64 bits; callers check the logical size before narrowing.
  constexpr uint64_t small() const { return uint64_t(words[0]) | (uint64_t(words[1]) << 32); }
  /// Orders magnitudes. @param other Magnitude to compare.
  constexpr int compare(const UInt& other) const {
    if (used != other.used) return used < other.used ? -1 : 1;
    for (uint32_t i = used; i-- > 0;)
      if (words[i] != other.words[i]) return words[i] < other.words[i] ? -1 : 1;
    return 0;
  }
  /// Multiplies by a power of two. @param amount Number of bits to shift.
  constexpr void shiftLeft(uint32_t amount) {
    if (!used || !amount || !valid) return;
    const uint32_t oldBits = bits();
    if (amount > kWords * 32 - oldBits) {
      valid = false;
      return;
    }
    const uint32_t whole = amount / 32, part = amount % 32;
    const uint32_t newUsed = (oldBits + amount + 31) / 32;
    for (uint32_t i = newUsed; i-- > 0;) {
      uint32_t value = 0;
      if (i >= whole) {
        const uint32_t from = i - whole;
        if (from < used) value = words[from] << part;
        if (part && from && from - 1 < used) value |= words[from - 1] >> (32 - part);
      }
      words[i] = value;
    }
    used = newUsed;
  }
  /// Divides by two, rounding down.
  constexpr void shiftRightOne() {
    uint32_t carry = 0;
    for (uint32_t i = used; i-- > 0;) {
      const uint32_t next = words[i] & 1u;
      words[i] = (words[i] >> 1) | (carry << 31);
      carry = next;
    }
    trim();
  }
  /// Adds a single limb. @param value Unsigned addend.
  constexpr void addSmall(uint32_t value) {
    uint64_t carry = value;
    for (uint32_t i = 0; carry; ++i) {
      if (i == kWords) {
        valid = false;
        return;
      }
      if (i == used) {
        words[used++] = 0;
      }
      carry += words[i];
      words[i] = uint32_t(carry);
      carry >>= 32;
    }
  }
  /// Multiplies by a single limb. @param factor Unsigned multiplier.
  constexpr void multiplySmall(uint32_t factor) {
    uint64_t carry = 0;
    for (uint32_t i = 0; i < used; ++i) {
      const uint64_t value = uint64_t(words[i]) * factor + carry;
      words[i] = uint32_t(value);
      carry = value >> 32;
    }
    if (carry) {
      if (used == kWords) {
        valid = false;
        return;
      }
      words[used++] = uint32_t(carry);
    }
    trim();
  }
  /// Adds a bounded magnitude. @param other Unsigned addend.
  constexpr void add(const UInt& other) {
    const uint32_t count = used > other.used ? used : other.used;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint64_t value =
          uint64_t(i < used ? words[i] : 0) + (i < other.used ? other.words[i] : 0) + carry;
      words[i] = uint32_t(value);
      carry = value >> 32;
    }
    used = count;
    if (carry) {
      if (used == kWords) {
        valid = false;
        return;
      }
      words[used++] = uint32_t(carry);
    }
    valid &= other.valid;
  }
  /// Subtracts or reports underflow. @param other Unsigned subtrahend.
  constexpr void subtract(const UInt& other) {
    if (compare(other) < 0) {
      valid = false;
      return;
    }
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < used; ++i) {
      const uint64_t sub = uint64_t(i < other.used ? other.words[i] : 0) + borrow;
      const uint64_t value = words[i];
      words[i] = uint32_t(value - sub);
      borrow = value < sub;
    }
    valid &= other.valid && borrow == 0;
    trim();
  }
  /// Returns the product or capacity failure. @param other Unsigned multiplier.
  constexpr UInt multiply(const UInt& other) const {
    UInt result;
    if (used + other.used > kWords) {
      result.valid = false;
      return result;
    }
    for (uint32_t i = 0; i < used; ++i) {
      uint64_t carry = 0;
      for (uint32_t j = 0; j < other.used; ++j) {
        const uint64_t value = uint64_t(words[i]) * other.words[j] + result.words[i + j] + carry;
        result.words[i + j] = uint32_t(value);
        carry = value >> 32;
      }
      result.words[i + other.used] = uint32_t(carry);
    }
    result.used = used + other.used;
    result.trim();
    result.valid = valid && other.valid;
    return result;
  }
};

/// Integer quotient and nearest-even rounded quotient of a positive rational.
struct Quotient {
  uint64_t floor = 0;
  uint64_t rounded = 0;
  bool exact = false;
  Error error = Error::None;
};

constexpr Quotient Divide(UInt numerator, const UInt& denominator) {
  if (!numerator.valid || !denominator.valid) return {0, 0, false, Error::Capacity};
  if (!denominator.used) return {0, 0, false, Error::DivisionByZero};
  if (numerator.used <= 2 && denominator.used <= 2) {
    const uint64_t n = numerator.small(), d = denominator.small();
    const uint64_t q = n / d, r = n % d;
    const bool up = r > d - r || (r == d - r && (q & 1));
    if (up && q == UINT64_MAX) return {0, 0, false, Error::Capacity};
    return {q, q + uint64_t(up), r == 0, Error::None};
  }
  uint64_t quotient = 0;
  const int32_t difference = int32_t(numerator.bits()) - int32_t(denominator.bits());
  if (difference >= 64) return {0, 0, false, Error::Capacity};
  if (difference >= 0) {
    UInt divisor = denominator;
    divisor.shiftLeft(uint32_t(difference));
    for (int32_t bit = difference; bit >= 0; --bit) {
      if (numerator.compare(divisor) >= 0) {
        numerator.subtract(divisor);
        quotient |= uint64_t(1) << bit;
      }
      divisor.shiftRightOne();
    }
  }
  const bool exact = numerator.used == 0;
  numerator.shiftLeft(1);
  const int midpoint = numerator.valid ? numerator.compare(denominator) : 1;
  const bool up = midpoint > 0 || (midpoint == 0 && (quotient & 1));
  if (up && quotient == UINT64_MAX) return {0, 0, false, Error::Capacity};
  return {quotient, quotient + uint64_t(up), exact, Error::None};
}

/// Finite IEEE encoding, exactness and failure of an arithmetic operation.
struct FloatResult {
  uint64_t bits = 0;
  bool exact = false;
  Error error = Error::None;
};

/// Converts an exact rational times a power of two to finite binary32 or binary64.
constexpr FloatResult Round(const UInt& n, const UInt& d, int32_t power, uint32_t precision,
                            bool negative = false) {
  if (precision != 24 && precision != 53) return {0, false, Error::Capacity};
  const int32_t bias = precision == 24 ? 127 : 1023;
  const uint32_t totalBits = precision == 24 ? 32 : 64;
  const uint64_t sign = negative ? uint64_t(1) << (totalBits - 1) : 0;
  if (!n.valid || !d.valid) return {0, false, Error::Capacity};
  if (!d.used) return {0, false, Error::DivisionByZero};
  if (!n.used) return {sign, true, Error::None};
  int32_t exponent = int32_t(n.bits()) - int32_t(d.bits());
  UInt comparison = exponent >= 0 ? d : n;
  comparison.shiftLeft(uint32_t(exponent >= 0 ? exponent : -exponent));
  if ((exponent >= 0 && n.compare(comparison) < 0) || (exponent < 0 && comparison.compare(d) < 0))
    --exponent;
  exponent += power;
  if (exponent > bias) return {0, false, Error::Range};
  const int32_t minimum = 1 - bias;
  const int32_t smallest = minimum - int32_t(precision - 1);
  if (exponent < smallest - 1) return {sign, false, Error::None};
  const int32_t quantum = exponent < minimum ? smallest : exponent - int32_t(precision - 1);
  const int32_t shift = power - quantum;
  UInt numerator = n, denominator = d;
  if (shift >= 0)
    numerator.shiftLeft(uint32_t(shift));
  else
    denominator.shiftLeft(uint32_t(-shift));
  const Quotient quotient = Divide(numerator, denominator);
  if (quotient.error != Error::None) return {0, false, quotient.error};
  const uint64_t hidden = uint64_t(1) << (precision - 1);
  const uint64_t maximum = (hidden << 1) - 1;
  if (exponent == bias &&
      (quotient.floor > maximum || (quotient.floor == maximum && !quotient.exact)))
    return {0, false, Error::Range};
  uint64_t significand = quotient.rounded;
  if (exponent < minimum) {
    if (significand < hidden) return {sign | significand, quotient.exact, Error::None};
    exponent = minimum;
  } else if (significand >= hidden << 1) {
    significand >>= 1;
    ++exponent;
  }
  if (exponent > bias) return {0, false, Error::Range};
  return {sign | (uint64_t(exponent + bias) << (precision - 1)) | (significand - hidden),
          quotient.exact, Error::None};
}

/// Decoded finite IEEE significand, binary scale and sign.
struct Parts {
  uint64_t significand = 0;
  int32_t power = 0;
  bool negative = false;
  bool valid = true;
};

constexpr Parts Decode(uint64_t bits, uint32_t precision) {
  if ((precision != 24 && precision != 53) || (precision == 24 && bits > UINT32_MAX))
    return {0, 0, false, false};
  const uint32_t exponentBits = precision == 24 ? 8 : 11;
  const int32_t bias = precision == 24 ? 127 : 1023;
  const uint64_t hidden = uint64_t(1) << (precision - 1);
  const uint32_t exponent = uint32_t((bits >> (precision - 1)) & ((1u << exponentBits) - 1));
  if (exponent == (1u << exponentBits) - 1) return {0, 0, false, false};
  return {(bits & (hidden - 1)) | (exponent ? hidden : 0),
          (exponent ? int32_t(exponent) - bias : 1 - bias) - int32_t(precision - 1),
          (bits >> (precision + exponentBits - 1)) != 0};
}

/// Converts finite IEEE precision with nearest-even rounding.
/// @param bits Source encoding. @param fromPrecision Source precision.
/// @param toPrecision Destination precision; precision is 24 or 53 significant bits.
constexpr FloatResult Convert(uint64_t bits, uint32_t fromPrecision, uint32_t toPrecision) {
  const Parts value = Decode(bits, fromPrecision);
  if (!value.valid) return {0, false, Error::Range};
  return Round(UInt(value.significand), UInt(1), value.power, toPrecision, value.negative);
}

/// Arithmetic supported during shader creation.
enum class Op : uint8_t { Add, Subtract, Multiply, Divide };

/// Evaluates finite IEEE arithmetic without host floating-point operations.
/// @param op Operation. @param leftBits Left encoding. @param rightBits Right encoding.
/// @param precision IEEE precision, 24 or 53 significant bits.
constexpr FloatResult Evaluate(Op op, uint64_t leftBits, uint64_t rightBits, uint32_t precision) {
  const Parts left = Decode(leftBits, precision), right = Decode(rightBits, precision);
  if (!left.valid || !right.valid) return {0, false, Error::Range};
  if (op == Op::Multiply)
    return Round(UInt(left.significand).multiply(UInt(right.significand)), UInt(1),
                 left.power + right.power, precision, left.negative != right.negative);
  if (op == Op::Divide)
    return Round(UInt(left.significand), UInt(right.significand), left.power - right.power,
                 precision, left.negative != right.negative);
  const int32_t power = left.power < right.power ? left.power : right.power;
  UInt a(left.significand), b(right.significand);
  a.shiftLeft(uint32_t(left.power - power));
  b.shiftLeft(uint32_t(right.power - power));
  const bool rightNegative = right.negative != (op == Op::Subtract);
  bool negative = left.negative;
  if (left.negative == rightNegative)
    a.add(b);
  else if (a.compare(b) >= 0)
    a.subtract(b);
  else {
    b.subtract(a);
    a = b;
    negative = rightNegative;
  }
  return Round(a, UInt(1), power, precision, negative);
}

constexpr int Digit(char ch, bool hex) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (hex && ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (hex && ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

/// WGSL scalar type of a numeric literal.
enum class Kind : uint8_t { AbstractInt, AbstractFloat, I32, U32, F32 };
/// Numeric literal result; abstract integers use signed 64-bit two's-complement bits.
struct Value {
  Kind kind = Kind::AbstractInt;
  uint64_t bits = 0;
  Error error = Error::None;
};

constexpr Value Integer(std::string_view text, bool hex, char suffix) {
  uint64_t result = 0;
  if (text.empty() || (!hex && text.size() > 1 && text[0] == '0'))
    return {Kind::AbstractInt, 0, Error::Syntax};
  const uint32_t base = hex ? 16 : 10;
  for (char ch : text) {
    const int digit = Digit(ch, hex);
    if (digit < 0) return {Kind::AbstractInt, 0, Error::Syntax};
    if (result > (UINT64_MAX - uint32_t(digit)) / base) return {Kind::AbstractInt, 0, Error::Range};
    result = result * base + uint32_t(digit);
  }
  const Kind kind = suffix == 'u' ? Kind::U32 : suffix == 'i' ? Kind::I32 : Kind::AbstractInt;
  const uint64_t maximum = kind == Kind::U32   ? UINT32_MAX
                           : kind == Kind::I32 ? INT32_MAX
                                               : INT64_MAX;
  return {kind, result, result > maximum ? Error::Range : Error::None};
}

/// Parses an unsigned WGSL literal with bounded decimal/hexadecimal conversion.
/// Retains at most 20 decimal or 16 hexadecimal significant digits. Unary signs are parsed
/// separately; explicit hexadecimal f32 literals must be exactly representable.
/// @param text At most 256 ASCII bytes, including any type suffix.
constexpr Value Parse(std::string_view text) {
  if (text.empty()) return {Kind::AbstractInt, 0, Error::Syntax};
  if (text.size() > 256) return {Kind::AbstractInt, 0, Error::Capacity};
  const bool hex = text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  if (hex) text.remove_prefix(2);
  bool point = false, exponent = false;
  for (char ch : text) {
    point |= ch == '.';
    exponent |= hex ? ch == 'p' || ch == 'P' : ch == 'e' || ch == 'E';
  }
  char suffix = 0;
  if (!text.empty()) {
    const char last = text.back();
    if (last == 'i' || last == 'u' || ((!hex || exponent) && (last == 'f' || last == 'h'))) {
      suffix = last;
      text.remove_suffix(1);
    }
  }
  const bool floating = point || exponent || suffix == 'f' || suffix == 'h';
  if (!floating) return Integer(text, hex, suffix);
  if (suffix == 'h' || suffix == 'i' || suffix == 'u' || text.empty())
    return {Kind::AbstractFloat, 0, Error::Syntax};
  if (!hex && !point && !exponent && text.size() > 1 && text[0] == '0')
    return {Kind::AbstractFloat, 0, Error::Syntax};
  UInt significand;
  uint32_t cursor = 0, digits = 0, significant = 0, dropped = 0, fractional = 0;
  bool afterPoint = false;
  const uint32_t retained = hex ? 16 : 20;
  while (cursor < text.size()) {
    const char ch = text[cursor];
    if (ch == '.') {
      if (afterPoint) return {Kind::AbstractFloat, 0, Error::Syntax};
      afterPoint = true;
      ++cursor;
      continue;
    }
    const int digit = Digit(ch, hex);
    if (digit < 0) break;
    ++cursor;
    ++digits;
    if (afterPoint) ++fractional;
    if (digit != 0 || significant != 0) {
      ++significant;
      if (significant <= retained) {
        significand.multiplySmall(hex ? 16 : 10);
        significand.addSmall(uint32_t(digit));
      } else
        ++dropped;
    }
  }
  if (!digits) return {Kind::AbstractFloat, 0, Error::Syntax};
  int32_t explicitPower = 0;
  if (cursor < text.size()) {
    const char marker = text[cursor++];
    if (!(hex ? marker == 'p' || marker == 'P' : marker == 'e' || marker == 'E'))
      return {Kind::AbstractFloat, 0, Error::Syntax};
    bool negative = false;
    if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-'))
      negative = text[cursor++] == '-';
    const uint32_t first = cursor;
    while (cursor < text.size()) {
      const int digit = Digit(text[cursor++], false);
      if (digit < 0) return {Kind::AbstractFloat, 0, Error::Syntax};
      if (explicitPower > 1000) return {Kind::AbstractFloat, 0, Error::Capacity};
      explicitPower = explicitPower * 10 + digit;
    }
    if (cursor == first) return {Kind::AbstractFloat, 0, Error::Syntax};
    if (negative) explicitPower = -explicitPower;
  }
  const uint32_t precision = suffix == 'f' ? 24 : 53;
  const Kind kind = suffix == 'f' ? Kind::F32 : Kind::AbstractFloat;
  int32_t power = explicitPower + (int32_t(dropped) - int32_t(fractional)) * (hex ? 4 : 1);
  if (!significand.used) return {kind, 0, Error::None};
  UInt denominator(1);
  int32_t binaryPower = hex ? power : 0;
  if (!hex) {
    const int32_t decimalExponent = int32_t(significant - dropped) + power - 1;
    if (decimalExponent > (precision == 24 ? 38 : 308)) return {kind, 0, Error::Range};
    if (decimalExponent < (precision == 24 ? -46 : -324)) return {kind, 0, Error::None};
    if (power >= 0)
      for (int32_t i = 0; i < power; ++i) significand.multiplySmall(10);
    else
      for (int32_t i = 0; i < -power; ++i) denominator.multiplySmall(10);
  }
  const FloatResult result = Round(significand, denominator, binaryPower, precision);
  if (hex && suffix == 'f' && !result.exact) return {kind, 0, Error::Range};
  return {kind, result.bits, result.error};
}

}  // namespace donner::gpu::shader::wgsl::number
