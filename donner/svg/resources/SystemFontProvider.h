#pragma once
/// @file

#include <mutex>
#include <optional>

#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::svg {

/**
 * \ref FontFamilyProvider backed by the host operating system's installed fonts.
 *
 * On macOS this enumerates families via CoreText (`CTFontManagerCopyAvailableFontFamilyNames`) and
 * materializes a flat sfnt byte stream on demand by reconstructing the font's tables through
 * CoreText (which works even for `.ttc`/`.dfont` members). All families report \ref
 * FontSource::System.
 *
 * On non-Apple platforms this is a stub: it enumerates nothing and loads nothing, so the catalog
 * still compiles and behaves (embedded fonts only).
 */
class SystemFontProvider : public FontFamilyProvider {
public:
  SystemFontProvider() = default;

  std::vector<FontFamilyInfo> families() const override;
  bool hasFamily(std::string_view family) const override;
  std::vector<uint8_t> loadFamilyData(std::string_view family) const override;

  /// Returns true if this build enumerates real system fonts (i.e. macOS), false for the stub.
  static bool isSupported();

private:
  /// Lazily enumerate + cache the system family names (canonical, display-cased).
  const std::vector<std::string>& enumeratedFamilies() const;

  mutable std::once_flag enumeratedOnce_;
  mutable std::vector<std::string> familyNames_;
};

}  // namespace donner::svg
