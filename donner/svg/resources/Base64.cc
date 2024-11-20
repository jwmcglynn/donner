#include "donner/svg/resources/Base64.h"

namespace donner::svg {

namespace {

constexpr uint8_t kInvalidChar = 255;

constexpr std::array<uint8_t, 256> CreateBase64LookupTable() {
  constexpr std::string_view base64Chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::array<uint8_t, 256> lookupTable = {kInvalidChar};
  lookupTable.fill(kInvalidChar);

  for (uint8_t i = 0; i < static_cast<uint8_t>(base64Chars.size()); ++i) {
    // NOLINTNEXTLINE: Array index is not constant but is known at compile time.
    lookupTable[static_cast<uint8_t>(base64Chars[i])] = i;
  }

  return lookupTable;
}

constexpr std::array<uint8_t, 256> kBase64LookupTable = CreateBase64LookupTable();

bool IsWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\f' || ch == '\r' || ch == '\n';
}

}  // namespace

ParseResult<std::vector<uint8_t>> DecodeBase64Data(std::string_view base64String) {
  std::vector<uint8_t> decodedData;
  decodedData.reserve(base64String.size() * 3 / 4);

  int accumulator = 0;
  int bitCount = -8;

  for (size_t i = 0; i < base64String.size(); ++i) {
    const char ch = base64String[i];

    // Skip newlines and padding '=' characters.
    if (IsWhitespace(ch) || ch == '=') {
      continue;
    }

    // NOLINTNEXTLINE: Array index is guaranteed to be in bounds.
    const uint8_t base64Value = kBase64LookupTable[static_cast<uint8_t>(ch)];
    if (base64Value == kInvalidChar) {
      ParseError err;
      err.reason = "Invalid base64 char '" + std::string(1, ch) + "'";
      err.location = FileOffset::Offset(i);
      return err;
    }

    accumulator = (accumulator << 6) + base64Value;
    bitCount += 6;
    if (bitCount >= 0) {
      decodedData.push_back((accumulator >> bitCount) & 0xFF);
      bitCount -= 8;
    }
  }

  return decodedData;
}

}  // namespace donner::svg
