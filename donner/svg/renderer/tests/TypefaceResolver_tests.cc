#include "donner/svg/renderer/TypefaceResolver.h"

#include <gtest/gtest.h>

#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"

namespace donner::svg {

TEST(TypefaceResolverTests, PicksCachedTypefaceBeforeFallback) {
  sk_sp<SkFontMgr> fontManager = SkFontMgr::RefDefault();
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
  sk_sp<SkFontMgr> fontManager = SkFontMgr::RefDefault();
  auto fallback = CreateEmbeddedFallbackTypeface(*fontManager);

  std::map<std::string, std::vector<sk_sp<SkTypeface>>> typefaces;
  SmallVector<RcString, 1> families;

  auto resolved = ResolveTypeface(families, SkFontStyle(), typefaces, *fontManager, fallback);

  EXPECT_EQ(resolved.get(), fallback.get());
}

TEST(TypefaceResolverTests, PicksSystemFamilyWhenAvailable) {
  sk_sp<SkFontMgr> fontManager = SkFontMgr::RefDefault();
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
