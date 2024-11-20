#pragma once

#include <cassert>
#include <string_view>
#include <tuple>

namespace donner {

/// Utility class for working with UTF-8 encoded strings.
class Utf8 {
public:
  /// U+FFFD REPLACEMENT CHARACTER (�)
  static constexpr char32_t kUnicodeReplacementCharacter = 0xFFFD;

  /// The greatest codepoint defined by Unicode, per
  /// https://www.w3.org/TR/css-syntax-3/#maximum-allowed-code-point
  static constexpr char32_t kUnicodeMaximumAllowedCodepoint = 0x10FFFF;

  /// Returns true if the codepoint is a surrogate, per
  /// https://infra.spec.whatwg.org/#surrogate
  static inline bool IsSurrogateCodepoint(char32_t ch) { return ch >= 0xD800 && ch <= 0xDFFF; }

  /// Returns true if the codepoint is a valid UTF-8 codepoint.
  [[maybe_unused]] static inline bool IsValidCodepoint(char32_t ch) {
    return (ch <= kUnicodeMaximumAllowedCodepoint && !IsSurrogateCodepoint(ch));
  }

  /// Determines the length in bytes of a UTF-8 encoded character based on its leading byte.
  /// @param leadingCh The leading byte of the UTF-8 character.
  /// @return The number of bytes in the UTF-8 character, or 0 if invalid.
  static inline int SequenceLength(char leadingCh) {
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

  /**
   * Decodes the next UTF-8 codepoint from the input string, without validating if it is valid.
   * If the string is empty or contains insufficient bytes, returns a replacement codepoint.
   *
   * @param str The input string_view from which to read the codepoint.
   * @return A tuple containing the decoded Unicode codepoint and the number of bytes consumed.
   */
  [[maybe_unused]] static inline std::tuple<char32_t, int> NextCodepointLenient(
      std::string_view str) {
    if (str.empty()) {
      return {kUnicodeReplacementCharacter, 0};
    }

    const int codepointSize = SequenceLength(str[0]);
    if (codepointSize == 0 || codepointSize > static_cast<int>(str.size())) {
      return {kUnicodeReplacementCharacter, 1};
    }

    char32_t codepoint = 0;
    switch (codepointSize) {
      case 1: codepoint = str[0]; break;
      case 2: codepoint = ((str[0] & 0b00011111) << 6) | (str[1] & 0b00111111); break;
      case 3:
        codepoint =
            ((str[0] & 0b00001111) << 12) | ((str[1] & 0b00111111) << 6) | (str[2] & 0b00111111);
        break;
      case 4:
        codepoint = ((str[0] & 0b00000111) << 18) | ((str[1] & 0b00111111) << 12) |
                    ((str[2] & 0b00111111) << 6) | (str[3] & 0b00111111);
        break;
      default: return {kUnicodeReplacementCharacter, 1};
    }

    return {codepoint, codepointSize};
  }

  /**
   * Decodes the next UTF-8 codepoint from the input string, while strictly validating continuation
   * bytes and sequence lengths. If an invalid codepoint is encountered, the function returns
   * the Unicode replacement character (\xFFFD) and consumes the invalid codepoint.
   *
   * @param str The input string_view from which to read the codepoint.
   * @return A tuple containing the decoded Unicode codepoint and the number of bytes consumed.
   */
  [[maybe_unused]] static inline std::tuple<char32_t, int> NextCodepoint(std::string_view str) {
    if (str.empty()) {
      return {kUnicodeReplacementCharacter, 0};
    }

    const int codepointSize = SequenceLength(str[0]);
    if (codepointSize == 0 || codepointSize > static_cast<int>(str.size())) {
      return {kUnicodeReplacementCharacter, 1};
    }

    // Validate continuation bytes
    for (int i = 1; i < codepointSize; ++i) {
      if ((str[i] & 0b11000000) != 0b10000000) {
        return {kUnicodeReplacementCharacter, 1};
      }
    }

    char32_t codepoint = 0;

    switch (codepointSize) {
      case 1: codepoint = str[0]; break;
      case 2: codepoint = ((str[0] & 0b00011111) << 6) | (str[1] & 0b00111111); break;
      case 3:
        codepoint =
            ((str[0] & 0b00001111) << 12) | ((str[1] & 0b00111111) << 6) | (str[2] & 0b00111111);
        break;
      case 4:
        codepoint = ((str[0] & 0b00000111) << 18) | ((str[1] & 0b00111111) << 12) |
                    ((str[2] & 0b00111111) << 6) | (str[3] & 0b00111111);
        break;
      default: return {kUnicodeReplacementCharacter, 1};
    }

    // Check for overlong sequences
    if ((codepointSize == 2 && codepoint < 0x80) || (codepointSize == 3 && codepoint < 0x800) ||
        (codepointSize == 4 && codepoint < 0x10000)) {
      return {kUnicodeReplacementCharacter, 1};
    }

    // Check for invalid codepoints
    if (!IsValidCodepoint(codepoint)) {
      return {kUnicodeReplacementCharacter, 1};
    }

    return {codepoint, codepointSize};
  }

  /// Appends the UTF-8 encoding of the given Unicode codepoint to the output iterator.
  /// @tparam OutputIterator An output iterator that accepts `char` elements.
  /// @param ch The Unicode codepoint to encode and append.
  /// @param it The output iterator to which the encoded bytes are appended.
  /// @return An iterator pointing to the element past the last inserted element.
  template <std::output_iterator<char> OutputIterator>
  static inline OutputIterator Append(char32_t ch, OutputIterator it) {
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
};

}  // namespace donner
