#include "donner/svg/renderer/TypefaceResolver.h"

#include <cmath>
#include <limits>
#include <unordered_set>

#include "embed_resources/PublicSansFont.h"
#include "include/core/SkData.h"
#include "include/core/SkString.h"

namespace donner::svg {

namespace {

sk_sp<SkTypeface> CreatePublicSansTypeface(SkFontMgr& fontManager) {
  auto data = SkData::MakeWithoutCopy(embedded::kPublicSansMediumOtf.data(),
                                      embedded::kPublicSansMediumOtf.size());
  return fontManager.makeFromData(std::move(data));
}
}  // namespace

sk_sp<SkTypeface> ResolveTypeface(
    const SmallVector<RcString, 1>& families, const SkFontStyle& fontStyle,
    const std::map<std::string, std::vector<sk_sp<SkTypeface>>>& typefaces, SkFontMgr& fontManager,
    sk_sp<SkTypeface> fallbackTypeface) {
  for (const RcString& family : families) {
    const auto it = typefaces.find(family.str());
    if (it != typefaces.end()) {
      const auto& typefaceList = it->second;
      if (!typefaceList.empty()) {
        sk_sp<SkTypeface> bestMatch = typefaceList.front();
        int bestScore = std::numeric_limits<int>::max();

        for (const auto& typeface : typefaceList) {
          if (!typeface) {
            continue;
          }

          const SkFontStyle style = typeface->fontStyle();
          const int score = std::abs(style.weight() - fontStyle.weight()) +
                            (style.width() == fontStyle.width() ? 0 : 1000) +
                            (style.slant() == fontStyle.slant() ? 0 : 1000);

          if (score < bestScore) {
            bestScore = score;
            bestMatch = typeface;
          }
        }

        return bestMatch;
      }
    }
  }

  std::unordered_set<std::string> availableFamilies;
  const int familyCount = fontManager.countFamilies();
  availableFamilies.reserve(static_cast<size_t>(familyCount + typefaces.size()));
  for (int i = 0; i < familyCount; ++i) {
    SkString familyName;
    fontManager.getFamilyName(i, &familyName);
    availableFamilies.emplace(familyName.c_str());
  }
  for (const auto& [family, fonts] : typefaces) {
    if (!fonts.empty()) {
      availableFamilies.emplace(family);
    }
  }

  for (const RcString& family : families) {
    if (availableFamilies.find(family.str()) == availableFamilies.end()) {
      continue;
    }

    if (auto matched = fontManager.matchFamilyStyle(family.str().c_str(), fontStyle); matched) {
      return matched;
    }
  }

  if (fallbackTypeface) {
    return fallbackTypeface;
  }

  return fontManager.matchFamilyStyle(nullptr, fontStyle);
}

sk_sp<SkTypeface> CreateEmbeddedFallbackTypeface(SkFontMgr& fontManager) {
  if (auto typeface = CreatePublicSansTypeface(fontManager); typeface) {
    return typeface;
  }

  return fontManager.matchFamilyStyle(nullptr, SkFontStyle());
}

void AddEmbeddedFonts(std::map<std::string, std::vector<sk_sp<SkTypeface>>>& typefaces,
                      SkFontMgr& fontManager) {
  if (auto typeface = CreatePublicSansTypeface(fontManager)) {
    typefaces["Public Sans"].push_back(std::move(typeface));
  }
}
}  // namespace donner::svg
