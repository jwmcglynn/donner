#pragma once
/// @file

#include <cstddef>
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

/// Maximum transient UTF-8 and source-index payload retained for one transformed span.
inline constexpr size_t kMaximumFullWidthBytes = 64u * 1024u * 1024u;

/// Transform one source span, preserving codepoint count and original byte indices.
/// @param text Original normalized text, which remains unchanged.
/// @param preserveSpaces Whether U+0020 survived whitespace normalization as preserved space.
/// @param maximumBytes Maximum combined UTF-8 and source-index payload, checked before allocation.
/// @return No result when the payload exceeds the bound or 32-bit shaping cluster indices.
std::optional<FullWidthText> MakeFullWidth(std::string_view text, bool preserveSpaces,
                                           size_t maximumBytes = kMaximumFullWidthBytes);

}  // namespace donner::svg::text_transform
