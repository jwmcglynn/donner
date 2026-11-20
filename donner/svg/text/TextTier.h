#pragma once
/// @file

#include <cstdint>
#include <iosfwd>
#include <string>

namespace donner::svg {

/**
 * Text support compiled into the linked Donner libraries, selected at build time by the
 * `//donner/svg/renderer:text` and `//donner/svg/renderer:text_full` Bazel settings or the
 * matching CMake options.
 *
 * The build settings reach only the translation units that implement text, so code outside them
 * queries \ref BuiltTextTier at run time instead of testing a preprocessor macro. Every header
 * therefore has the same declarations in every text configuration.
 *
 * WOFF2 font loading is reported separately by \ref IsWoff2BuiltIn: Bazel builds it exactly with
 * \ref TextTier::Full, but CMake controls it with its own option, on by default, so a CMake
 * build of either text tier may have it or not.
 */
enum class TextTier : uint8_t {
  None,   ///< Text rendering is compiled out; `<text>` content draws nothing.
  Basic,  ///< stb_truetype glyph outlines with kern-table kerning; TTF, OTF and WOFF1 fonts.
  Full,   ///< FreeType outlines and HarfBuzz shaping.
};

/// Ostream output operator for \ref TextTier, outputs the enum name.
std::ostream& operator<<(std::ostream& os, TextTier tier);

/**
 * Returns the text support the linked Donner libraries were built with.
 *
 * Every library in one binary is built from the same configuration, so the result describes the
 * whole binary.
 */
TextTier BuiltTextTier();

/// Returns true if the linked libraries render text (\ref TextTier::Basic or \ref TextTier::Full).
inline bool IsTextBuiltIn() {
  return BuiltTextTier() != TextTier::None;
}

/// Returns true if the linked libraries include FreeType outlines and HarfBuzz shaping.
inline bool IsFullTextBuiltIn() {
  return BuiltTextTier() == TextTier::Full;
}

/// Returns true if the linked font manager loads WOFF2 fonts; see \ref TextTier.
bool IsWoff2BuiltIn();

/**
 * Prepares the packaged font resources that text rendering needs, as \ref
 * donner::PreparePackagedResources does, when text is built in. A build without text needs none
 * and succeeds without touching the packaged resource session.
 *
 * @param error Receives the reason on failure; unchanged on success.
 * @return True if the resources are ready or no text is built in.
 */
bool PrepareTextResources(std::string* error);

}  // namespace donner::svg
