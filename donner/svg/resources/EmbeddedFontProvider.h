#pragma once
/// @file

#include <memory>

#include "donner/svg/resources/CatalogEncodedFontStore.h"
#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::svg {

/**
 * Curated bundled fonts, sharing one generated WOFF2 catalog on native and Wasm.
 *
 * Native uses compiled compressed spans and stays self-contained. Wasm links only the manifest
 * and obtains verified encoded bytes from the session store. Enumeration never requests bytes.
 * Each family retains its existing single variable/static file and face-selection behavior.
 */
class EmbeddedFontProvider : public FontFamilyProvider {
public:
  EmbeddedFontProvider();

  /// Explicit deferred delivery, also usable by native applications and deterministic tests.
  explicit EmbeddedFontProvider(std::shared_ptr<CatalogEncodedFontStore> store);

  std::vector<FontFamilyInfo> families() const override;
  bool hasFamily(std::string_view family) const override;
  FontFaceAvailability availability(std::string_view family,
                                    const FontFaceRequest& request) const override;
  FontFaceAdmission tryAcquireFace(std::string_view family,
                                   const FontFaceRequest& request) const override;
  std::vector<uint8_t> loadFamilyData(std::string_view family,
                                      const FontFaceRequest& request) const override;

  /// Session-owned delivery/admission state. Native needs no copied encoded cache.
  std::shared_ptr<CatalogEncodedFontStore> encodedStore() const { return store_; }

private:
  std::shared_ptr<CatalogEncodedFontStore> store_;
  bool deferred_ = false;
};

}  // namespace donner::svg
