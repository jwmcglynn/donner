#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace donner::svg::text_transform {

/// Transformed shaping text with the original byte offset of each Unicode codepoint.
struct FullWidthText {
  std::string text;
  std::vector<uint32_t> sourceByteOffsets;
};

/// Return the Unicode full-width mapping, retaining a collapsed ASCII space unchanged.
char32_t FullWidthCodepoint(char32_t codepoint, bool preserveSpaces);

/// Whether \p text contains a character whose full-width form differs.
bool NeedsFullWidth(std::string_view text, bool preserveSpaces);

/// Transform one source span, preserving codepoint count and original byte indices.
/// Returns no result when the expanded string cannot be addressed by 32-bit shaping clusters.
std::optional<FullWidthText> MakeFullWidth(std::string_view text, bool preserveSpaces);

}  // namespace donner::svg::text_transform
