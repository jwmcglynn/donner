#pragma once
/// @file

#include <cstdint>
#include <string>
#include <string_view>

#include "donner/base/Utf8.h"

namespace donner {

/// Escape untrusted bytes and terminal format controls while preserving printable UTF-8.
inline std::string EscapeTerminalText(std::string_view text, bool preserveNewlines = false) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(text.size());
  const auto appendHexEscape = [&](std::string_view prefix, std::size_t digits,
                                   std::uint32_t value) {
    result.append(prefix);
    for (std::size_t index = digits; index > 0; --index) {
      result.push_back(kHex[(value >> ((index - 1) * 4)) & 0x0f]);
    }
  };
  const auto isFormatControl = [](char32_t codepoint) {
    return codepoint == 0x061c || codepoint == 0x180e ||
           (codepoint >= 0x200b && codepoint <= 0x200f) ||
           (codepoint >= 0x2028 && codepoint <= 0x202e) ||
           (codepoint >= 0x2060 && codepoint <= 0x206f) || codepoint == 0xfeff;
  };

  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::string_view remaining = text.substr(offset);
    const auto [codepoint, length] = Utf8::NextCodepoint(remaining);
    const bool invalidEncoding = length <= 0 ||
                                 static_cast<std::size_t>(length) > remaining.size() ||
                                 (codepoint == Utf8::kUnicodeReplacementCharacter && length == 1);
    if (invalidEncoding) {
      appendHexEscape("\\x", 2, static_cast<unsigned char>(remaining.front()));
      ++offset;
      continue;
    }
    if (codepoint == '\n' && preserveNewlines) {
      result.push_back('\n');
    } else if (codepoint <= 0x1f || (codepoint >= 0x7f && codepoint <= 0x9f)) {
      appendHexEscape(codepoint <= 0x7f ? "\\x" : "\\u", codepoint <= 0x7f ? 2 : 4,
                      static_cast<std::uint32_t>(codepoint));
    } else if (isFormatControl(codepoint)) {
      appendHexEscape(codepoint <= 0xffff ? "\\u" : "\\U", codepoint <= 0xffff ? 4 : 8,
                      static_cast<std::uint32_t>(codepoint));
    } else {
      result.append(remaining.substr(0, static_cast<std::size_t>(length)));
    }
    offset += static_cast<std::size_t>(length);
  }
  return result;
}

}  // namespace donner
