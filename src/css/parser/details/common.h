#pragma once

#include <cctype>
#include <string_view>
#include <tuple>

namespace donner {
namespace css {
namespace details {

/// U+FFFD REPLACEMENT CHARACTER (�)
static constexpr char32_t kUnicodeReplacementCharacter = 0xFFFD;

static inline bool stringLowercaseEq(std::string_view str, std::string_view matcher) {
  if (str.size() != matcher.size()) {
    return false;
  }

  for (size_t i = 0; i < str.size(); ++i) {
    if (std::tolower(str[i]) != matcher[i]) {
      return false;
    }
  }

  return true;
}

static inline int utf8SequenceLength(char leadingCh) {
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

static inline std::tuple<char32_t, int> utf8NextCodepoint(std::string_view str) {
  const int codepointSize = utf8SequenceLength(str[0]);
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

}  // namespace details
}  // namespace css
}  // namespace donner
