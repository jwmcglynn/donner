#include "donner/svg/resources/FontCatalog.h"

#include <algorithm>
#include <unordered_set>

#include "donner/base/StringUtils.h"
#include "donner/svg/resources/EmbeddedFontProvider.h"
#include "donner/svg/resources/SystemFontProvider.h"
#ifdef __EMSCRIPTEN__
#include "embed_resources/PublicSansFont.h"
#else
#include "embed_resources/NotoSansGenericFaces.h"
#endif

namespace donner::svg {

namespace {

/// Lowercase copy for case-insensitive dedup keys.
std::string toLower(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

/// Editor-only generic family with consistent upright, bold, and italic metrics.
class GenericSansProvider final : public FontFamilyProvider {
public:
  std::vector<FontFamilyInfo> families() const override {
    return {{"sans-serif", FontSource::Bundled, FontCategory::SansSerif}};
  }

  bool hasFamily(std::string_view family) const override {
    return StringUtils::Equals<StringComparison::IgnoreCase>(family,
                                                             std::string_view("sans-serif"));
  }

  std::vector<uint8_t> loadFamilyData(std::string_view family,
                                      const FontFaceRequest& request) const override {
    if (!hasFamily(family)) {
      return {};
    }
#ifdef __EMSCRIPTEN__
    (void)request;
    // Reuse the renderer's existing fallback face. The full text backend synthesizes missing
    // bold and oblique on this upright font without adding another web font payload.
    const std::span<const unsigned char> bytes = embedded::kPublicSansMediumOtf;
#else
    const std::span<const unsigned char> bytes = request.weight >= 600 ? embedded::kNotoSansBoldTtf
                                                 : request.style != FontStyle::Normal
                                                     ? embedded::kNotoSansItalicTtf
                                                     : embedded::kNotoSansRegularTtf;
#endif
    return {bytes.begin(), bytes.end()};
  }
};

}  // namespace

FontCatalog::FontCatalog() {
  providers_.push_back(std::make_unique<GenericSansProvider>());
  auto bundled = std::make_unique<EmbeddedFontProvider>();
  store_ = bundled->encodedStore();
  providers_.push_back(std::move(bundled));
  providers_.push_back(std::make_unique<SystemFontProvider>());
}

FontCatalog::FontCatalog(std::shared_ptr<CatalogEncodedFontStore> store)
    : store_(std::move(store)) {
  providers_.push_back(std::make_unique<GenericSansProvider>());
  providers_.push_back(std::make_unique<EmbeddedFontProvider>(store_));
  providers_.push_back(std::make_unique<SystemFontProvider>());
}

FontCatalog::FontCatalog(std::vector<std::unique_ptr<FontFamilyProvider>> providers)
    : providers_(std::move(providers)) {}

FontCatalog::~FontCatalog() = default;

std::vector<FontFamilyInfo> FontCatalog::families() const {
  std::vector<FontFamilyInfo> result;
  std::unordered_set<std::string> seen;
  for (const auto& provider : providers_) {
    std::vector<FontFamilyInfo> group = provider->families();
    std::sort(group.begin(), group.end(),
              [](const FontFamilyInfo& a, const FontFamilyInfo& b) { return a.family < b.family; });
    for (FontFamilyInfo& info : group) {
      // CSS generic families resolve through find(), but are not named faces in the picker.
      if (StringUtils::Equals<StringComparison::IgnoreCase>(info.family,
                                                            std::string_view("sans-serif"))) {
        continue;
      }
      if (seen.insert(toLower(info.family)).second) {
        result.push_back(std::move(info));
      }
    }
  }
  return result;
}

bool FontCatalog::hasFamily(std::string_view family) const {
  for (const auto& provider : providers_) {
    if (provider->hasFamily(family)) {
      return true;
    }
  }
  return false;
}

std::vector<uint8_t> FontCatalog::loadFamilyData(std::string_view family,
                                                 const FontFaceRequest& request) const {
  bool skippedSynchronousProvider = false;
  for (const auto& provider : providers_) {
    if (provider->hasFamily(family)) {
      const bool immutable = !provider->availability(family, request).contentId.empty();
      // Metadata and admission selected the first claimant. Legacy empty-byte fallback may only
      // reach another synchronous provider; crossing into immutable assets would bypass their
      // identity, readiness, decode reservation, and stricter resource limits.
      if (skippedSynchronousProvider && immutable) {
        return {};
      }
      // Readiness must not change bundled source precedence. Preserve the historic empty-byte
      // fallback for synchronous custom providers, whose metadata has no immutable content ID.
      auto data = provider->loadFamilyData(family, request);
      if (!data.empty() || immutable) {
        return data;
      }
      skippedSynchronousProvider = true;
    }
  }
  return {};
}

FontFaceAvailability FontCatalog::availability(std::string_view family,
                                               const FontFaceRequest& request) const {
  for (const auto& provider : providers_) {
    if (provider->hasFamily(family)) {
      return provider->availability(family, request);
    }
  }
  return {};
}

FontFaceAdmission FontCatalog::tryAcquireFace(std::string_view family,
                                              const FontFaceRequest& request) const {
  for (const auto& provider : providers_) {
    if (provider->hasFamily(family)) {
      return provider->tryAcquireFace(family, request);
    }
  }
  return {.state = FontFaceLoadState::Failed};
}

std::optional<FontFamilyInfo> FontCatalog::find(std::string_view family) const {
  for (const auto& provider : providers_) {
    if (!provider->hasFamily(family)) {
      continue;
    }
    for (FontFamilyInfo& info : provider->families()) {
      if (StringUtils::Equals<StringComparison::IgnoreCase>(info.family, family)) {
        return info;
      }
    }
  }
  return std::nullopt;
}

std::vector<FontFamilyInfo> FontCatalog::familiesBySource(FontSource source) const {
  std::vector<FontFamilyInfo> result;
  for (const auto& provider : providers_) {
    for (FontFamilyInfo& info : provider->families()) {
      if (StringUtils::Equals<StringComparison::IgnoreCase>(info.family,
                                                            std::string_view("sans-serif"))) {
        continue;
      }
      if (info.source == source) {
        result.push_back(std::move(info));
      }
    }
  }
  std::sort(result.begin(), result.end(),
            [](const FontFamilyInfo& a, const FontFamilyInfo& b) { return a.family < b.family; });
  return result;
}

}  // namespace donner::svg
