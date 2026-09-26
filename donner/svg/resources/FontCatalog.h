#pragma once
/// @file

#include <memory>
#include <optional>

#include "donner/svg/resources/CatalogEncodedFontStore.h"
#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::svg {

/**
 * Aggregate font catalog: the single surface the editor's font picker and \ref FontManager
 * resolution consume. The default catalog also resolves the CSS generic `sans-serif` family
 * through bundled regular, bold, and italic faces without listing it as a named picker family.
 *
 * A default-constructed catalog contains the generic sans-serif faces, an embedded provider
 * (curated Google Fonts), then a system provider (CoreText on macOS; a no-op stub elsewhere).
 * Providers are consulted in order, so
 * resolution and `loadFace()` prefer **Bundled** families over **System** families, and
 * `families()` lists the Bundled group before the System group.
 *
 * The catalog implements \ref FontFamilyProvider, so it can be installed directly on a \ref
 * FontManager (via `setFontProvider()` or `SetDefaultFontProvider()`).
 *
 * ### Picker contract
 * - `families()` / `familiesBySource()` enumerate available families, grouped and sorted, each
 *   tagged with its \ref FontSource (Bundled vs System) and best-effort \ref FontCategory.
 * - `hasFamily()` reports whether a typed family resolves through the catalog.
 * - `loadFace()` returns encoded font-file bytes for a family, suitable for the full-text
 * `FontManager` used by catalog previews. These bytes are not raw TTF for ImGui.
 */
class FontCatalog : public FontFamilyProvider {
public:
  /// Construct with the default providers: embedded fonts, then platform system fonts.
  FontCatalog();

  /// Use a session-owned deferred catalog store followed by the platform system provider.
  explicit FontCatalog(std::shared_ptr<CatalogEncodedFontStore> store);

  /// The default bundled provider's delivery/admission store; null for an explicit custom list.
  std::shared_ptr<CatalogEncodedFontStore> encodedStore() const { return store_; }

  /// Test/advanced constructor: supply the ordered provider list explicitly (first wins on ties).
  /// Empty-byte fallback between legacy synchronous providers is supported. An immutable asset
  /// provider must precede synchronous claimants for the same family; it cannot be their fallback
  /// because doing so would separate its bytes from the selected metadata/admission contract.
  explicit FontCatalog(std::vector<std::unique_ptr<FontFamilyProvider>> providers);

  ~FontCatalog() override;

  FontCatalog(const FontCatalog&) = delete;
  FontCatalog& operator=(const FontCatalog&) = delete;

  // FontFamilyProvider:

  /**
   * All available families, Bundled group first then System group, each group sorted by name. If
   * the same family name appears in both groups the Bundled one wins and the System duplicate is
   * dropped (case-insensitive).
   */
  std::vector<FontFamilyInfo> families() const override;

  /// True if any provider supplies \p family (case-insensitive).
  bool hasFamily(std::string_view family) const override;

  /**
   * Resolve \p family (case-insensitive) to its catalog entry, or `std::nullopt` if no provider
   * supplies it (in which case a document naming this family renders through the Public Sans
   * fallback). The returned \ref FontFamilyInfo::source tells the caller whether it resolved to an
   * Bundled or System font, mirroring `findFont()` precedence.
   */
  std::optional<FontFamilyInfo> find(std::string_view family) const;

  /// Encoded font-file bytes for \p family from the first provider that has it (Bundled before
  /// System).
  std::vector<uint8_t> loadFamilyData(std::string_view family,
                                      const FontFaceRequest& request) const override;

  FontFaceAvailability availability(std::string_view family,
                                    const FontFaceRequest& request) const override;
  FontFaceAdmission tryAcquireFace(std::string_view family,
                                   const FontFaceRequest& request) const override;

  // Picker conveniences:

  /// Families from a single source (Bundled or System), sorted by name.
  std::vector<FontFamilyInfo> familiesBySource(FontSource source) const;

  /// Alias for `loadFamilyData()`, named for the picker's preview use. Previews show the family's
  /// default face, so this asks for the regular upright one.
  std::vector<uint8_t> loadFace(std::string_view family) const {
    return loadFamilyData(family, FontFaceRequest{});
  }

private:
  std::vector<std::unique_ptr<FontFamilyProvider>> providers_;
  std::shared_ptr<CatalogEncodedFontStore> store_;
};

}  // namespace donner::svg
