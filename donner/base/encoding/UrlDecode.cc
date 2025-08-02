#include "donner/base/encoding/UrlDecode.h"

namespace donner {

namespace {

/// Check if a character is a valid hexadecimal digit.
bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

/// Convert a hexadecimal digit to its numeric value.
uint8_t HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<uint8_t>(c - '0');
  } else if (c >= 'A' && c <= 'F') {
    return static_cast<uint8_t>(c - 'A' + 10);
  } else {  // c >= 'a' && c <= 'f'
    return static_cast<uint8_t>(c - 'a' + 10);
  }
}

}  // namespace

std::vector<uint8_t> UrlDecode(std::string_view urlEncodedString) {
  std::vector<uint8_t> decodedData;
  decodedData.reserve(urlEncodedString.size());

  // Process each byte in the input.
  for (size_t i = 0; i < urlEncodedString.size(); ++i) {
    const char byte = urlEncodedString[i];

    if (byte != '%') {
      // Append non-'%' bytes directly.
      decodedData.push_back(static_cast<uint8_t>(byte));
    } else {
      // Check if there are at least two more bytes.
      if (i + 2 < urlEncodedString.size() && IsHexDigit(urlEncodedString[i + 1]) &&
          IsHexDigit(urlEncodedString[i + 2])) {
        // Decode the two hexadecimal digits.
        const uint8_t highNibble = HexValue(urlEncodedString[i + 1]);
        const uint8_t lowNibble = HexValue(urlEncodedString[i + 2]);
        const uint8_t decodedByte = static_cast<uint8_t>((highNibble << 4) | lowNibble);
        decodedData.push_back(decodedByte);
        i += 2;  // Skip the next two bytes.
      } else {
        // Either there are not two bytes remaining, or the bytes are not valid hex digits.
        // Append '%' as literal.
        decodedData.push_back(static_cast<uint8_t>('%'));
      }
    }
  }

  return decodedData;
}

}  // namespace donner
