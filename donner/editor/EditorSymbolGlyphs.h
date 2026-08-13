#pragma once
/// @file
///
/// Non-ASCII glyphs the editor chrome draws as text, in one place.
///
/// ImGui only rasterizes the codepoints a font is asked for, and only those the
/// font actually has: a codepoint outside the embedded fonts' `cmap` is silently
/// dropped from the atlas and drawn as the font's fallback character, a literal
/// `?`. Keeping the glyphs and the atlas ranges next to each other lets one test
/// assert the whole set is really present in the fonts the editor ships.

#include <array>
#include <cstdint>

namespace donner::editor {

/// Marker on a source-reference chip that carries no match count - the chip a
/// focus-reference rope hangs off. A small lozenge reads as "this is the styling
/// this rope points at" without competing with the numeric chips beside it.
inline constexpr char32_t kSourceReferenceChipCodepoint = 0x25CA;  // LOZENGE

/// UTF-8 encoding of \ref kSourceReferenceChipCodepoint, as a C string for
/// ImGui's text API.
inline constexpr const char* kSourceReferenceChipIcon = "◊";

/// Marker drawn beside a chip whose fanout was too large to expand into ropes.
inline constexpr char32_t kSourceChipOverflowCodepoint = 0x2026;  // HORIZONTAL ELLIPSIS

/// UTF-8 encoding of \ref kSourceChipOverflowCodepoint, as a C string for
/// ImGui's text API.
inline constexpr const char* kSourceChipOverflowMarker = "…";

/// Every non-ASCII codepoint the editor chrome draws, ascending. Both the font
/// atlas ranges and the embedded-font coverage test are built from this.
inline constexpr std::array<char32_t, 2> kEditorSymbolCodepoints = {
    kSourceChipOverflowCodepoint,
    kSourceReferenceChipCodepoint,
};

}  // namespace donner::editor
