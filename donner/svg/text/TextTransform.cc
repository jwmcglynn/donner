#include "donner/svg/text/TextTransform.h"

#include <algorithm>
#include <iterator>
#include <limits>

#include "donner/base/Utf8.h"
#include "donner/svg/text/FullWidthMapping.h"

namespace donner::svg::text_transform {

char32_t FullWidthCodepoint(char32_t codepoint, bool preserveSpaces) {
  if (codepoint == U' ' && !preserveSpaces) {
    return codepoint;
  }
  const auto found = std::lower_bound(
      detail::kFullWidthMapping.begin(), detail::kFullWidthMapping.end(), codepoint,
      [](const auto& entry, char32_t value) { return entry.first < value; });
  return found != detail::kFullWidthMapping.end() && found->first == codepoint ? found->second
                                                                               : codepoint;
}

bool NeedsFullWidth(std::string_view text, bool preserveSpaces) {
  while (!text.empty()) {
    const auto [codepoint, bytes] = Utf8::NextCodepoint(text);
    if (FullWidthCodepoint(codepoint, preserveSpaces) != codepoint) {
      return true;
    }
    text.remove_prefix(static_cast<size_t>(bytes));
  }
  return false;
}

std::optional<FullWidthText> MakeFullWidth(std::string_view text, bool preserveSpaces,
                                           size_t maximumBytes) {
  if (text.size() > std::numeric_limits<uint32_t>::max()) {
    return std::nullopt;
  }
  size_t encodedBytes = 0;
  size_t codepoints = 0;
  for (size_t offset = 0; offset < text.size();) {
    const auto [codepoint, bytes] = Utf8::NextCodepoint(text.substr(offset));
    const char32_t mapped = FullWidthCodepoint(codepoint, preserveSpaces);
    const size_t width = mapped < 0x80 ? 1 : mapped < 0x800 ? 2 : mapped < 0x10000 ? 3 : 4;
    if (encodedBytes > maximumBytes || width > maximumBytes - encodedBytes) {
      return std::nullopt;
    }
    encodedBytes += width;
    ++codepoints;
    offset += static_cast<size_t>(bytes);
  }
  if (encodedBytes > std::numeric_limits<uint32_t>::max() ||
      codepoints > (maximumBytes - encodedBytes) / sizeof(uint32_t)) {
    return std::nullopt;
  }
  FullWidthText result;
  result.text.reserve(encodedBytes);
  result.sourceByteOffsets.reserve(codepoints);
  for (size_t offset = 0; offset < text.size();) {
    const auto [codepoint, bytes] = Utf8::NextCodepoint(text.substr(offset));
    result.sourceByteOffsets.push_back(static_cast<uint32_t>(offset));
    Utf8::Append(FullWidthCodepoint(codepoint, preserveSpaces), std::back_inserter(result.text));
    offset += static_cast<size_t>(bytes);
  }
  return result;
}

}  // namespace donner::svg::text_transform
