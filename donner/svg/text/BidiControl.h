#pragma once
/// @file

namespace donner::svg {

/// Unicode Bidi_Control characters affect ordering but consume no SVG character position.
constexpr bool IsBidiControlCodepoint(char32_t cp) {
  return cp == 0x061c || cp == 0x200e || cp == 0x200f || (cp >= 0x202a && cp <= 0x202e) ||
         (cp >= 0x2066 && cp <= 0x2069);
}

}  // namespace donner::svg
