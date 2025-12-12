#include "donner/svg/renderer/TypefaceResolver.h"

#include <gtest/gtest.h>

#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"

#ifdef DONNER_USE_CORETEXT
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(DONNER_USE_FREETYPE)
#include "include/ports/SkFontMgr_empty.h"
#elif defined(DONNER_USE_FREETYPE_WITH_FONTCONFIG)
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#else
#error \
    "Neither DONNER_USE_CORETEXT, DONNER_USE_FREETYPE, nor DONNER_USE_FREETYPE_WITH_FONTCONFIG is defined"
#endif

namespace donner::svg {

namespace {

sk_sp<SkFontMgr> CreateFontManager() {
#ifdef DONNER_USE_CORETEXT
  return SkFontMgr_New_CoreText(nullptr);
#elif defined(DONNER_USE_FREETYPE)
  return SkFontMgr_New_Custom_Empty();
#elif defined(DONNER_USE_FREETYPE_WITH_FONTCONFIG)
  return SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
}

}  // namespace

TEST(TypefaceResolverTests, PicksCachedTypefaceBeforeFallback) {
  sk_sp<SkFontMgr> fontManager = CreateFontManager();
  auto cachedTypeface = CreateEmbeddedFallbackTypeface(*fontManager);
  auto fallback = fontManager->matchFamilyStyle(nullptr, SkFontStyle());
  if (!fallback) {
    fallback = cachedTypeface;
  }

  std::map<std::string, std::vector<sk_sp<SkTypeface>>> typefaces;
  typefaces["Example"].push_back(cachedTypeface);

  SmallVector<RcString, 1> families{RcString("Missing"), RcString("Example")};

  auto resolved = ResolveTypeface(families, SkFontStyle(), typefaces, *fontManager, fallback);

  EXPECT_EQ(resolved.get(), cachedTypeface.get());
}

TEST(TypefaceResolverTests, FallsBackWhenNoFamilyMatches) {
  sk_sp<SkFontMgr> fontManager = CreateFontManager();
  auto fallback = CreateEmbeddedFallbackTypeface(*fontManager);

  std::map<std::string, std::vector<sk_sp<SkTypeface>>> typefaces;
  SmallVector<RcString, 1> families;

  auto resolved = ResolveTypeface(families, SkFontStyle(), typefaces, *fontManager, fallback);

  EXPECT_EQ(resolved.get(), fallback.get());
}

TEST(TypefaceResolverTests, PicksSystemFamilyWhenAvailable) {
  sk_sp<SkFontMgr> fontManager = CreateFontManager();
  ASSERT_GT(fontManager->countFamilies(), 0);

  SkString systemFamily;
  fontManager->getFamilyName(0, &systemFamily);

  SmallVector<RcString, 1> families{RcString("Missing"), RcString(systemFamily.c_str())};
  auto fallback = CreateEmbeddedFallbackTypeface(*fontManager);

  auto resolved =
      ResolveTypeface(families, SkFontStyle(), /*typefaces=*/{}, *fontManager, fallback);

  ASSERT_TRUE(resolved);

  SkString resolvedFamily;
  resolved->getFamilyName(&resolvedFamily);

  EXPECT_STREQ(resolvedFamily.c_str(), systemFamily.c_str());
}

}  // namespace donner::svg
