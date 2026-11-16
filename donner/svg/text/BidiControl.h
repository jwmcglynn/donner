#pragma once
/// @file

namespace donner::svg {

/// Explicit formatting characters that CSS `unicode-bidi` values translate to.
namespace bidi_control {
constexpr char32_t kLre = 0x202a;  //!< LEFT-TO-RIGHT EMBEDDING
constexpr char32_t kRle = 0x202b;  //!< RIGHT-TO-LEFT EMBEDDING
constexpr char32_t kPdf = 0x202c;  //!< POP DIRECTIONAL FORMATTING
constexpr char32_t kLro = 0x202d;  //!< LEFT-TO-RIGHT OVERRIDE
constexpr char32_t kRlo = 0x202e;  //!< RIGHT-TO-LEFT OVERRIDE
constexpr char32_t kLri = 0x2066;  //!< LEFT-TO-RIGHT ISOLATE
constexpr char32_t kRli = 0x2067;  //!< RIGHT-TO-LEFT ISOLATE
constexpr char32_t kFsi = 0x2068;  //!< FIRST STRONG ISOLATE
constexpr char32_t kPdi = 0x2069;  //!< POP DIRECTIONAL ISOLATE
}  // namespace bidi_control

/// Unicode Bidi_Control characters affect ordering but consume no SVG character position.
constexpr bool IsBidiControlCodepoint(char32_t cp) {
  return cp == 0x061c || cp == 0x200e || cp == 0x200f || (cp >= 0x202a && cp <= 0x202e) ||
         (cp >= 0x2066 && cp <= 0x2069);
}

}  // namespace donner::svg
