#pragma once
/// @file

#include <memory>

#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::svg {

/**
 * Aggregate font catalog: the single surface the editor's font picker and \ref FontManager
 * resolution consume.
 *
 * A default-constructed catalog contains an embedded provider (curated Google Fonts) followed by a
 * system provider (CoreText on macOS; a no-op stub elsewhere). Providers are consulted in order, so
 * resolution and `loadFace()` prefer **Embedded** families over **System** families, and `families()`
 * lists the Embedded group before the System group.
 *
 * The catalog implements \ref FontFamilyProvider, so it can be installed directly on a \ref
 * FontManager (via `setFontProvider()` or `SetDefaultFontProvider()`).
 *
 * ### Picker contract (Design 0013 W2 consumes this)
 * - `families()` / `familiesBySource()` enumerate available families, grouped and sorted, each
 *   tagged with its \ref FontSource (Embedded vs System) and best-effort \ref FontCategory.
 * - `hasFamily()` reports whether a typed family resolves through the catalog.
 * - `loadFace()` returns raw sfnt bytes for a family, suitable for building an ImGui/preview font
 *   or for `FontManager::loadFontData()`.
 */
class FontCatalog : public FontFamilyProvider {
public:
  /// Construct with the default providers: embedded fonts, then platform system fonts.
  FontCatalog();

  /// Test/advanced constructor: supply the ordered provider list explicitly (first wins on ties).
  explicit FontCatalog(std::vector<std::unique_ptr<FontFamilyProvider>> providers);

  ~FontCatalog() override;

  FontCatalog(const FontCatalog&) = delete;
  FontCatalog& operator=(const FontCatalog&) = delete;

  // FontFamilyProvider:

  /**
   * All available families, Embedded group first then System group, each group sorted by name. If
   * the same family name appears in both groups the Embedded one wins and the System duplicate is
   * dropped (case-insensitive).
   */
  std::vector<FontFamilyInfo> families() const override;

  /// True if any provider supplies \p family (case-insensitive).
  bool hasFamily(std::string_view family) const override;

  /// Raw sfnt bytes for \p family from the first provider that has it (Embedded before System).
  std::vector<uint8_t> loadFamilyData(std::string_view family) const override;

  // Picker conveniences:

  /// Families from a single source (Embedded or System), sorted by name.
  std::vector<FontFamilyInfo> familiesBySource(FontSource source) const;

  /// Alias for `loadFamilyData()`, named for the picker's preview use.
  std::vector<uint8_t> loadFace(std::string_view family) const { return loadFamilyData(family); }

private:
  std::vector<std::unique_ptr<FontFamilyProvider>> providers_;
};

}  // namespace donner::svg
