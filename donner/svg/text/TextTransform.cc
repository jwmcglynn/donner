#include "donner/svg/text/TextTransform.h"

#include <algorithm>
#include <iterator>
#include <limits>

#include "donner/base/Utf8.h"
#include "donner/svg/text/FullWidthMapping.h"

namespace donner::svg::text_transform {

char32_t FullWidthCodepoint(char32_t codepoint, bool preserveSpaces) {
  if (codepoint == U' ' && !preserveSpaces) return codepoint;
  const auto found = std::lower_bound(detail::kFullWidthMapping.begin(),
                                       detail::kFullWidthMapping.end(), codepoint,
                                       [](const auto& entry, char32_t value) {
                                         return entry.first < value;
                                       });
  return found != detail::kFullWidthMapping.end() && found->first == codepoint
             ? found->second : codepoint;
}

bool NeedsFullWidth(std::string_view text, bool preserveSpaces) {
  while (!text.empty()) {
    const auto [codepoint, bytes] = Utf8::NextCodepoint(text);
    if (FullWidthCodepoint(codepoint, preserveSpaces) != codepoint) return true;
    text.remove_prefix(static_cast<size_t>(bytes));
  }
  return false;
}

std::optional<FullWidthText> MakeFullWidth(std::string_view text, bool preserveSpaces) {
  if (text.size() > std::numeric_limits<uint32_t>::max() / 3u) return std::nullopt;
  FullWidthText result;
  result.text.reserve(text.size() * 3u);
  result.sourceByteOffsets.reserve(text.size());
  for (size_t offset = 0; offset < text.size();) {
    const auto [codepoint, bytes] = Utf8::NextCodepoint(text.substr(offset));
    result.sourceByteOffsets.push_back(static_cast<uint32_t>(offset));
    Utf8::Append(FullWidthCodepoint(codepoint, preserveSpaces), std::back_inserter(result.text));
    offset += static_cast<size_t>(bytes);
  }
  return result;
}

}  // namespace donner::svg::text_transform
