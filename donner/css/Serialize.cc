#include "donner/css/Serialize.h"

#include <cstdint>
#include <format>

namespace donner::css {

namespace {

constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";

/// Appends a CSS hex escape for @p byte, followed by the space that terminates it.
void AppendHexEscape(std::string& out, unsigned char byte) {
  out += std::format("\\{:x} ", byte);
}

bool IsControl(unsigned char c) {
  return (c >= 0x01 && c <= 0x1F) || c == 0x7F;
}

bool IsAsciiDigit(unsigned char c) {
  return c >= '0' && c <= '9';
}

bool IsPlainIdentifierByte(unsigned char c) {
  return c >= 0x80 || c == '-' || c == '_' || IsAsciiDigit(c) || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z');
}

/// Whether the byte at @p index of an identifier needs a hex escape because it is a digit in a
/// position where it would start a number.
bool IsLeadingDigit(std::string_view value, std::size_t index) {
  const auto c = static_cast<unsigned char>(value[index]);
  return IsAsciiDigit(c) && (index == 0 || (index == 1 && value[0] == '-'));
}

}  // namespace

std::string SerializeIdentifier(std::string_view value) {
  std::string out;
  if (value == "-") {
    return "\\-";
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto c = static_cast<unsigned char>(value[i]);
    if (c == 0) {
      out += kReplacementCharacter;
    } else if (IsControl(c) || IsLeadingDigit(value, i)) {
      AppendHexEscape(out, c);
    } else if (IsPlainIdentifierByte(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

std::string SerializeStringContents(std::string_view value, char quote) {
  std::string out;
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == 0) {
      out += kReplacementCharacter;
    } else if (IsControl(c)) {
      AppendHexEscape(out, c);
    } else if (ch == quote || ch == '\\') {
      out.push_back('\\');
      out.push_back(ch);
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::string SerializeUrlTokenValue(std::string_view value) {
  std::string out;
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == 0) {
      out += kReplacementCharacter;
    } else if (IsControl(c) || c == ' ' || ch == '"' || ch == '\'' || ch == '(' || ch == ')' ||
               ch == '\\') {
      AppendHexEscape(out, c);
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

}  // namespace donner::css
