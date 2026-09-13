#include "donner/svg/resources/EmbeddedFontProvider.h"

#include <algorithm>
#include <array>
#include <span>

#include "donner/base/StringUtils.h"
#ifndef __EMSCRIPTEN__
#include "embed_resources/GoogleFontsData.h"
#endif

namespace donner::svg {

std::span<const CatalogFontAsset> CatalogFontAssets() {
  static const std::array<CatalogFontAsset, 12> kAssets = {{
#define DONNER_GF_ENTRY(family, category, id, path, encoded, decoded, symbol) \
  CatalogFontAsset{family, FontCategory::category, id, path, encoded, decoded},
#include "embed_resources/GoogleFontsCatalog.inc"
#undef DONNER_GF_ENTRY
  }};
  return kAssets;
}

namespace {

const CatalogFontAsset* FindAsset(std::string_view family) {
  for (const auto& asset : CatalogFontAssets()) {
    if (StringUtils::Equals<StringComparison::IgnoreCase>(asset.family, family)) return &asset;
  }
  return nullptr;
}

#ifndef __EMSCRIPTEN__
std::span<const unsigned char> EmbeddedData(std::string_view family) {
#define DONNER_GF_ENTRY(familyName, category, id, path, encoded, decoded, symbol)                \
  if (StringUtils::Equals<StringComparison::IgnoreCase>(std::string_view(familyName), family)) { \
    return ::donner::embedded::symbol;                                                           \
  }
#include "embed_resources/GoogleFontsCatalog.inc"
#undef DONNER_GF_ENTRY
  return {};
}
#endif

}  // namespace

EmbeddedFontProvider::EmbeddedFontProvider() : store_(std::make_shared<CatalogEncodedFontStore>()) {
#ifdef __EMSCRIPTEN__
  deferred_ = true;
#endif
}

EmbeddedFontProvider::EmbeddedFontProvider(std::shared_ptr<CatalogEncodedFontStore> store)
    : store_(std::move(store)), deferred_(true) {}

std::vector<FontFamilyInfo> EmbeddedFontProvider::families() const {
  std::vector<FontFamilyInfo> result;
  result.reserve(CatalogFontAssets().size());
  for (const auto& asset : CatalogFontAssets()) {
    result.push_back({asset.family, FontSource::Bundled, asset.category});
  }
  std::sort(result.begin(), result.end(),
            [](const FontFamilyInfo& a, const FontFamilyInfo& b) { return a.family < b.family; });
  return result;
}

bool EmbeddedFontProvider::hasFamily(std::string_view family) const {
  return FindAsset(family) != nullptr;
}

FontFaceAvailability EmbeddedFontProvider::availability(std::string_view family,
                                                        const FontFaceRequest&) const {
  const auto* asset = FindAsset(family);
  if (!asset || !store_) return {};
  auto result = store_->availability(asset->contentId);
  if (!deferred_) result.state = FontAssetState::Ready;
  return result;
}

FontFaceAdmission EmbeddedFontProvider::tryAcquireFace(std::string_view family,
                                                       const FontFaceRequest&) const {
  const auto* asset = FindAsset(family);
  return asset && store_ ? store_->tryAcquireDecode(asset->contentId)
                         : FontFaceAdmission{.state = FontFaceLoadState::Failed};
}

std::vector<uint8_t> EmbeddedFontProvider::loadFamilyData(
    std::string_view family, const FontFaceRequest& /*request*/) const {
  const auto* asset = FindAsset(family);
  if (!asset || !store_) return {};
  if (deferred_) {
    // A miss is metadata-only. Only the application coordinator can queue approved visible or
    // explicit-output demand; layout may also inspect fonts belonging to offscreen text.
    const auto bytes = store_->encodedBytes(asset->contentId);
    return bytes ? std::vector<uint8_t>(bytes->begin(), bytes->end()) : std::vector<uint8_t>{};
  }
#ifndef __EMSCRIPTEN__
  const auto data = EmbeddedData(family);
  return {data.begin(), data.end()};
#else
  return {};
#endif
}

}  // namespace donner::svg
