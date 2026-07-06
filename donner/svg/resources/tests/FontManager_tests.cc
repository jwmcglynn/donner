#include "donner/svg/resources/FontManager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <vector>

#include "embed_resources/PublicSansFont.h"

namespace donner::svg {

namespace {

struct TestCacheComponent {
  int value = 0;
};

/// Read a file from disk into a byte vector.
std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
}

/// A FontFamilyProvider that serves a fixed set of family names (Public Sans bytes for all) and
/// counts calls, so tests can prove exactly when the provider is consulted.
class FakeFontProvider : public FontFamilyProvider {
public:
  explicit FakeFontProvider(std::vector<std::string> families) : families_(std::move(families)) {}

  std::vector<FontFamilyInfo> families() const override {
    std::vector<FontFamilyInfo> out;
    for (const std::string& name : families_) {
      out.push_back(FontFamilyInfo{name, FontSource::Embedded, FontCategory::SansSerif});
    }
    return out;
  }

  bool hasFamily(std::string_view family) const override {
    for (const std::string& name : families_) {
      if (name == family) {
        return true;
      }
    }
    return false;
  }

  std::vector<uint8_t> loadFamilyData(std::string_view family) const override {
    ++loadCalls;
    if (!hasFamily(family)) {
      return {};
    }
    return std::vector<uint8_t>(embedded::kPublicSansMediumOtf.begin(),
                                embedded::kPublicSansMediumOtf.end());
  }

  mutable int loadCalls = 0;

private:
  std::vector<std::string> families_;
};

}  // namespace

TEST(FontManagerTest, FallbackFontLoads) {
  Registry registry;
  FontManager mgr(registry);
  FontHandle handle = mgr.fallbackFont();
  EXPECT_TRUE(static_cast<bool>(handle));

  auto data = mgr.fontData(handle);
  EXPECT_FALSE(data.empty());
}

TEST(FontManagerTest, FallbackFontIsCached) {
  Registry registry;
  FontManager mgr(registry);
  FontHandle h1 = mgr.fallbackFont();
  FontHandle h2 = mgr.fallbackFont();
  EXPECT_EQ(h1, h2);
}

TEST(FontManagerTest, LoadRawOtfData) {
  Registry registry;
  FontManager mgr(registry);

  // Load the embedded Public Sans font data directly (raw OTF).
  std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                            embedded::kPublicSansMediumOtf.end());
  FontHandle handle = mgr.loadFontData(data);
  EXPECT_TRUE(static_cast<bool>(handle));

  EXPECT_FALSE(mgr.fontData(handle).empty());
}

TEST(FontManagerTest, LoadWoff1Data) {
  Registry registry;
  FontManager mgr(registry);

  std::vector<uint8_t> woffData = readFile("donner/base/fonts/testdata/valid-001.woff");
  ASSERT_FALSE(woffData.empty()) << "Could not read WOFF test file";

  FontHandle handle = mgr.loadFontData(woffData);
  EXPECT_TRUE(static_cast<bool>(handle));
  EXPECT_FALSE(mgr.fontData(handle).empty());
}

TEST(FontManagerTest, InvalidDataReturnsInvalidHandle) {
  Registry registry;
  FontManager mgr(registry);

  // Empty data.
  FontHandle h1 = mgr.loadFontData({});
  EXPECT_FALSE(static_cast<bool>(h1));

  // Garbage data.
  std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
  FontHandle h2 = mgr.loadFontData(garbage);
  EXPECT_FALSE(static_cast<bool>(h2));
}

TEST(FontManagerTest, InvalidHandleReturnsEmptyMeasurements) {
  Registry registry;
  FontManager mgr(registry);
  FontHandle invalid;
  EXPECT_TRUE(mgr.fontData(invalid).empty());
}

TEST(FontManagerTest, FontDataReturnsNonEmpty) {
  Registry registry;
  FontManager mgr(registry);
  FontHandle handle = mgr.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(handle));

  auto data = mgr.fontData(handle);
  EXPECT_FALSE(data.empty());
}

TEST(FontManagerTest, FindFontFallsBackToPublicSans) {
  Registry registry;
  FontManager mgr(registry);

  // No @font-face registered, so any family name should fall back.
  FontHandle handle = mgr.findFont("NonExistentFont");
  EXPECT_TRUE(static_cast<bool>(handle));

  // Should be the same as the fallback.
  FontHandle fallback = mgr.fallbackFont();
  EXPECT_EQ(handle, fallback);
}

TEST(FontManagerTest, FindFontCachesResult) {
  Registry registry;
  FontManager mgr(registry);

  FontHandle h1 = mgr.findFont("Anything");
  FontHandle h2 = mgr.findFont("Anything");
  EXPECT_EQ(h1, h2);
}

TEST(FontManagerTest, AddFontFaceInvalidatesLookupCache) {
  Registry registry;
  FontManager mgr(registry);

  const FontHandle fallbackBefore = mgr.findFont("TestFont");
  ASSERT_TRUE(static_cast<bool>(fallbackBefore));
  EXPECT_EQ(fallbackBefore, mgr.fallbackFont());

  css::FontFace face;
  face.familyName = RcString("TestFont");

  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = std::make_shared<const std::vector<uint8_t>>(
      embedded::kPublicSansMediumOtf.begin(), embedded::kPublicSansMediumOtf.end());
  face.sources.push_back(std::move(source));

  mgr.addFontFace(face);

  const FontHandle resolvedAfter = mgr.findFont("TestFont");
  EXPECT_TRUE(static_cast<bool>(resolvedAfter));
  EXPECT_NE(resolvedAfter, fallbackBefore);
}

TEST(FontManagerTest, AddFontFaceWithDataSource) {
  Registry registry;
  FontManager mgr(registry);

  // Create a @font-face with inline data from the embedded Public Sans font.
  css::FontFace face;
  face.familyName = RcString("TestFont");

  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = std::make_shared<const std::vector<uint8_t>>(
      embedded::kPublicSansMediumOtf.begin(), embedded::kPublicSansMediumOtf.end());
  face.sources.push_back(std::move(source));

  mgr.addFontFace(face);

  FontHandle handle = mgr.findFont("TestFont");
  FontHandle secondHandle = mgr.findFont("TestFont");
  EXPECT_TRUE(static_cast<bool>(handle));
  EXPECT_EQ(secondHandle, handle);

  // The loaded font should be different from the fallback (separate allocation).
  FontHandle fallback = mgr.fallbackFont();
  EXPECT_NE(handle, fallback);

  EXPECT_FALSE(mgr.fontData(handle).empty());
}

TEST(FontManagerTest, AllowsAttachingAndRemovingCustomCacheComponents) {
  Registry registry;
  FontManager mgr(registry);

  const FontHandle handle = mgr.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(handle));
  EXPECT_EQ(registry.try_get<TestCacheComponent>(handle.entity()), nullptr);

  auto& component = registry.emplace<TestCacheComponent>(handle.entity());
  component.value = 42;

  const auto* cached = registry.try_get<TestCacheComponent>(handle.entity());
  ASSERT_NE(cached, nullptr);
  EXPECT_EQ(cached->value, 42);

  EXPECT_EQ(registry.remove<TestCacheComponent>(handle.entity()), 1u);
  EXPECT_EQ(registry.try_get<TestCacheComponent>(handle.entity()), nullptr);
}

TEST(FontManagerTest, ProviderResolvesFamilyMissingFromFontFaces) {
  Registry registry;
  FontManager mgr(registry);

  FakeFontProvider provider({"ProviderFamily"});
  mgr.setFontProvider(&provider);

  const FontHandle handle = mgr.findFont("ProviderFamily");
  ASSERT_TRUE(static_cast<bool>(handle));
  // Resolved through the provider, not the Public Sans fallback entity.
  EXPECT_NE(handle, mgr.fallbackFont());
  EXPECT_EQ(provider.loadCalls, 1);
  EXPECT_FALSE(mgr.fontData(handle).empty());
}

// QA-F23 layer 3 (font resolution) plumbing: the provider serves one font file
// per family regardless of weight/style, so findFont must record the requested
// variable-font instance on non-default lookups. The text backend consumes this
// to instantiate the `wght` axis; without it a bold request rendered the default
// (regular) instance and looked identical to normal. A default 400/normal lookup
// records nothing, so regular-weight rendering stays byte-for-byte unchanged.
TEST(FontManagerTest, RecordsVariationRequestForNonDefaultProviderLookup) {
  Registry registry;
  FontManager mgr(registry);

  FakeFontProvider provider({"ProviderFamily"});
  mgr.setFontProvider(&provider);

  const FontHandle regular = mgr.findFont("ProviderFamily", 400);
  ASSERT_TRUE(static_cast<bool>(regular));
  EXPECT_FALSE(mgr.requestedVariation(regular).has_value())
      << "A default 400/normal lookup must not request a variation instance.";

  const FontHandle bold = mgr.findFont("ProviderFamily", 700);
  ASSERT_TRUE(static_cast<bool>(bold));
  // Distinct weights resolve to distinct font entities, so the bold instance can
  // carry its own axis coordinates without disturbing the regular one.
  EXPECT_NE(regular, bold);
  const auto boldRequest = mgr.requestedVariation(bold);
  ASSERT_TRUE(boldRequest.has_value());
  EXPECT_EQ(boldRequest->weight, 700);
  EXPECT_EQ(boldRequest->style, 0);

  const FontHandle italic = mgr.findFont("ProviderFamily", 400, /*style=*/1, /*stretch=*/5);
  const auto italicRequest = mgr.requestedVariation(italic);
  ASSERT_TRUE(italicRequest.has_value());
  EXPECT_EQ(italicRequest->weight, 400);
  EXPECT_EQ(italicRequest->style, 1);
}

TEST(FontManagerTest, FontFaceTakesPrecedenceOverProvider) {
  Registry registry;
  FontManager mgr(registry);

  // Both a document @font-face and the provider claim "Shared".
  css::FontFace face;
  face.familyName = RcString("Shared");
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = std::make_shared<const std::vector<uint8_t>>(
      embedded::kPublicSansMediumOtf.begin(), embedded::kPublicSansMediumOtf.end());
  face.sources.push_back(std::move(source));
  mgr.addFontFace(face);

  FakeFontProvider provider({"Shared"});
  mgr.setFontProvider(&provider);

  const FontHandle handle = mgr.findFont("Shared");
  ASSERT_TRUE(static_cast<bool>(handle));
  EXPECT_NE(handle, mgr.fallbackFont());
  // The @font-face satisfied the lookup, so the provider was never consulted.
  EXPECT_EQ(provider.loadCalls, 0);
}

TEST(FontManagerTest, FallsBackToPublicSansWhenProviderLacksFamily) {
  Registry registry;
  FontManager mgr(registry);

  FakeFontProvider provider({"ProviderFamily"});
  mgr.setFontProvider(&provider);

  const FontHandle handle = mgr.findFont("SomethingElse");
  EXPECT_TRUE(static_cast<bool>(handle));
  EXPECT_EQ(handle, mgr.fallbackFont());
  EXPECT_EQ(provider.loadCalls, 0);
}

TEST(FontManagerTest, DefaultProviderAdoptedByNewInstances) {
  FakeFontProvider provider({"ProviderFamily"});
  FontManager::SetDefaultFontProvider(&provider);

  {
    Registry registry;
    FontManager mgr(registry);
    const FontHandle handle = mgr.findFont("ProviderFamily");
    EXPECT_TRUE(static_cast<bool>(handle));
    EXPECT_NE(handle, mgr.fallbackFont());
  }

  // Reset so the global does not leak into other tests.
  FontManager::SetDefaultFontProvider(nullptr);
  EXPECT_EQ(FontManager::DefaultFontProvider(), nullptr);
}

#ifdef DONNER_TEXT_WOFF2_ENABLED
TEST(FontManagerTest, LoadWoff2Data) {
  Registry registry;
  FontManager mgr(registry);

  std::vector<uint8_t> woff2Data = readFile("donner/base/fonts/testdata/valid-001.woff2");
  ASSERT_FALSE(woff2Data.empty()) << "Could not read WOFF2 test file";

  FontHandle handle = mgr.loadFontData(woff2Data);
  EXPECT_TRUE(static_cast<bool>(handle));
  EXPECT_FALSE(mgr.fontData(handle).empty());
}
#endif

}  // namespace donner::svg
