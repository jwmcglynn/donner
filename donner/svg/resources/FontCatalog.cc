#include "donner/svg/resources/FontCatalog.h"

#include <algorithm>
#include <unordered_set>

#include "donner/base/StringUtils.h"
#include "donner/svg/resources/EmbeddedFontProvider.h"
#include "donner/svg/resources/SystemFontProvider.h"

namespace donner::svg {

namespace {

/// Lowercase copy for case-insensitive dedup keys.
std::string toLower(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

}  // namespace

FontCatalog::FontCatalog() {
  // Order defines precedence: Embedded before System.
  providers_.push_back(std::make_unique<EmbeddedFontProvider>());
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

std::vector<uint8_t> FontCatalog::loadFamilyData(std::string_view family) const {
  for (const auto& provider : providers_) {
    if (provider->hasFamily(family)) {
      std::vector<uint8_t> data = provider->loadFamilyData(family);
      if (!data.empty()) {
        return data;
      }
    }
  }
  return {};
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
