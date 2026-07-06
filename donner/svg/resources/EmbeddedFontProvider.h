#pragma once
/// @file

#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::svg {

/**
 * \ref FontFamilyProvider backed by the curated Google Fonts set embedded into the build.
 *
 * The family table is generated from `third_party/google_fonts/fonts.bzl` (see the
 * `DONNER_GF_ENTRY` x-macro include), so it always matches the fetched-and-embedded bytes. All
 * families report \ref FontSource::Embedded.
 */
class EmbeddedFontProvider : public FontFamilyProvider {
public:
  EmbeddedFontProvider() = default;

  std::vector<FontFamilyInfo> families() const override;
  bool hasFamily(std::string_view family) const override;
  std::vector<uint8_t> loadFamilyData(std::string_view family) const override;
};

}  // namespace donner::svg
