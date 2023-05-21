#pragma once

#include <cassert>
#include <cctype>
#include <string_view>
#include <tuple>

namespace donner::css::details {

/// U+FFFD REPLACEMENT CHARACTER (�)
static constexpr char32_t kUnicodeReplacementCharacter = 0xFFFD;

/// The greatest codepoint defined by unicode, per
/// https://www.w3.org/TR/css-syntax-3/#maximum-allowed-code-point
static constexpr char32_t kUnicodeMaximumAllowedCodepoint = 0x10FFFF;

/// Returns true if the codepoint is a surrogate, per
/// https://infra.spec.whatwg.org/#surrogate
static inline bool IsSurrogateCodepoint(char32_t ch) {
  return ch >= 0xD800 && ch <= 0xDFFF;
}

/// Returns true if the codepoint is a valid UTF-8 codepoint.
[[maybe_unused]] static inline bool IsValidCodepoint(char32_t ch) {
  return (ch <= kUnicodeMaximumAllowedCodepoint && !IsSurrogateCodepoint(ch));
}

static inline int Utf8SequenceLength(char leadingCh) {
  const uint8_t maskedCh = static_cast<uint8_t>(leadingCh);
  if (maskedCh < 0b10000000) {
    return 1;
  } else if ((maskedCh & 0b11100000) == 0b11000000) {
    return 2;
  } else if ((maskedCh & 0b11110000) == 0b11100000) {
    return 3;
  } else if ((maskedCh & 0b11111000) == 0b11110000) {
    return 4;
  } else {
    return 0;
  }
}

[[maybe_unused]] static inline std::tuple<char32_t, int> Utf8NextCodepoint(std::string_view str) {
  const int codepointSize = Utf8SequenceLength(str[0]);
  if (codepointSize > str.size()) {
    return {kUnicodeReplacementCharacter, str.size()};
  }

  const uint8_t* c = reinterpret_cast<const uint8_t*>(str.data());

  switch (codepointSize) {
    default:
    case 0: return {kUnicodeReplacementCharacter, 1};
    case 1: return {c[0], 1};
    case 2:
      return {((c[0] & 0b00011111) << 6)  //
                  | (c[1] & 0b00111111),
              2};
    case 3:
      return {((c[0] & 0b00001111) << 12)       //
                  | ((c[1] & 0b00111111) << 6)  //
                  | (c[2] & 0b00111111),
              3};
    case 4:
      return {((c[0] & 0b00001111) << 17)        //
                  | ((c[1] & 0b00111111) << 12)  //
                  | ((c[2] & 0b00111111) << 6)   //
                  | (c[3] & 0b00111111),
              3};
  }
}

// TODO: Change this to output_iterator concept once standard library supports it.
template <typename OutputIterator>
static inline OutputIterator Utf8Append(char32_t ch, OutputIterator it) {
  assert(IsValidCodepoint(ch));

  if (ch < 0x80) {
    *(it++) = static_cast<uint8_t>(ch);
  } else if (ch < 0x800) {
    *(it++) = 0b11000000 | static_cast<uint8_t>(ch >> 6);
    *(it++) = 0b10000000 | (static_cast<uint8_t>(ch) & 0b00111111);
  } else if (ch < 0x10000) {
    *(it++) = 0b11100000 | static_cast<uint8_t>(ch >> 12);
    *(it++) = 0b10000000 | (static_cast<uint8_t>(ch >> 6) & 0b00111111);
    *(it++) = 0b10000000 | (static_cast<uint8_t>(ch) & 0b00111111);
  } else {
    *(it++) = 0b11110000 | static_cast<uint8_t>(ch >> 18);
    *(it++) = 0b10000000 | (static_cast<uint8_t>(ch >> 12) & 0b00111111);
    *(it++) = 0b10000000 | (static_cast<uint8_t>(ch >> 6) & 0b00111111);
    *(it++) = 0b10000000 | (static_cast<uint8_t>(ch) & 0b00111111);
  }

  return it;
}

}  // namespace donner::css::details
