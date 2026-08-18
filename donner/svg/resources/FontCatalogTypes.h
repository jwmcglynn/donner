#pragma once
/// @file

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/svg/core/FontStretch.h"
#include "donner/svg/core/FontStyle.h"

namespace donner::svg {

/**
 * Where a font family in the \ref FontCatalog originates.
 *
 * The picker groups families by source, and resolution prefers \ref Embedded over \ref System
 * (see \ref FontCatalog and \ref FontManager).
 */
enum class FontSource {
  Embedded,  //!< Bundled with the build (curated Google Fonts, fetched + embedded at build time).
  System,    //!< Discovered from the host OS (CoreText on macOS; none on other platforms).
};

/**
 * Coarse style bucket for a font family, used to give the picker variety and (later) to inform
 * generic-family fallback. Best-effort: \ref Unknown is used when a provider cannot classify a
 * family.
 */
enum class FontCategory {
  SansSerif,
  Serif,
  Monospace,
  Display,
  Handwriting,
  Unknown,
};

/// One entry in the font catalog: a family name plus how it was discovered and classified.
struct FontFamilyInfo {
  std::string family;                             //!< Display name and CSS `font-family` match key.
  FontSource source = FontSource::Embedded;       //!< Origin (embedded vs system).
  FontCategory category = FontCategory::Unknown;  //!< Best-effort style bucket.

  /// Equality (used by tests).
  bool operator==(const FontFamilyInfo& other) const {
    return family == other.family && source == other.source && category == other.category;
  }
};

/**
 * Which face inside a family a `font-family` lookup wants, taken from the element's computed CSS
 * font properties.
 *
 * A family is a *set* of faces, not one file: "Helvetica" covers Regular, Bold, Oblique, and Bold
 * Oblique. Providers must honour this or `font-weight: bold` silently renders as regular.
 *
 * @see https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm
 */
struct FontFaceRequest {
  int weight = 400;                           //!< CSS `font-weight`, 100-900 (700 = bold).
  FontStyle style = FontStyle::Normal;        //!< CSS `font-style`.
  FontStretch stretch = FontStretch::Normal;  //!< CSS `font-stretch`.

  /// Equality comparison.
  bool operator==(const FontFaceRequest& other) const = default;
};

/// Ostream output operator, so test failures print the requested face instead of an opaque struct.
inline std::ostream& operator<<(std::ostream& os, const FontFaceRequest& request) {
  return os << "FontFaceRequest(weight=" << request.weight << ", style=" << request.style
            << ", stretch=" << request.stretch << ")";
}

/**
 * Interface implemented by each font source (embedded, system) and by the aggregate \ref
 * FontCatalog. Lets \ref FontManager resolve a `font-family` name against providers without
 * depending on the (potentially large) embedded font bytes: the core engine sees only this
 * interface, and the editor injects a concrete catalog.
 *
 * All lookups by family name are case-insensitive.
 */
class FontFamilyProvider {
public:
  virtual ~FontFamilyProvider() = default;

  /// List the families this provider can supply.
  virtual std::vector<FontFamilyInfo> families() const = 0;

  /// Returns true if \p family is available (case-insensitive).
  virtual bool hasFamily(std::string_view family) const = 0;

  /**
   * Load the raw sfnt (TTF/OTF) bytes for the face of \p family that best matches \p request, or
   * an empty vector if the family is unavailable.
   *
   * Providers that only hold a single file per family return it for every request, which means
   * bold and italic render as regular for those families - see \ref EmbeddedFontProvider.
   *
   * The returned bytes are suitable for `FontManager::loadFontData()`.
   *
   * @param family Family name to load (case-insensitive).
   * @param request Face within the family to prefer.
   */
  virtual std::vector<uint8_t> loadFamilyData(std::string_view family,
                                              const FontFaceRequest& request) const = 0;
};

}  // namespace donner::svg
