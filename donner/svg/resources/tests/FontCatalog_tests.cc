#include "donner/svg/resources/FontCatalog.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <ranges>
#include <string>

#include "donner/svg/core/FontStyle.h"
#include "donner/svg/resources/EmbeddedFontProvider.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/resources/SystemFontProvider.h"

namespace donner::svg {

namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;

/// A provider that reports a fixed family list and returns a single sentinel byte per family so
/// tests can tell which provider served a given family.
class MarkerProvider : public FontFamilyProvider {
public:
  MarkerProvider(std::vector<std::string> families, FontSource source, uint8_t marker)
      : families_(std::move(families)), source_(source), marker_(marker) {}

  std::vector<FontFamilyInfo> families() const override {
    std::vector<FontFamilyInfo> out;
    for (const std::string& name : families_) {
      out.push_back(FontFamilyInfo{name, source_, FontCategory::Unknown});
    }
    return out;
  }

  bool hasFamily(std::string_view family) const override {
    return std::any_of(families_.begin(), families_.end(),
                       [&](const std::string& name) { return name == family; });
  }

  std::vector<uint8_t> loadFamilyData(std::string_view family,
                                      const FontFaceRequest& /*request*/) const override {
    if (!hasFamily(family) || marker_ == 0) {
      return {};
    }
    return std::vector<uint8_t>{marker_};
  }

private:
  std::vector<std::string> families_;
  FontSource source_;
  uint8_t marker_;
};

std::vector<std::string> familyNames(const std::vector<FontFamilyInfo>& infos) {
  std::vector<std::string> names;
  names.reserve(infos.size());
  for (const FontFamilyInfo& info : infos) {
    names.push_back(info.family);
  }
  return names;
}

}  // namespace

// --- EmbeddedFontProvider (curated Google Fonts) ---------------------------------------------

TEST(EmbeddedFontProviderTest, EnumeratesCuratedSet) {
  EmbeddedFontProvider provider;
  const std::vector<std::string> names = familyNames(provider.families());

  EXPECT_THAT(names, ElementsAre("Bebas Neue", "Bitter", "Inter", "JetBrains Mono", "Lato", "Lora",
                                 "Montserrat", "Open Sans", "Oswald", "Pacifico",
                                 "Playfair Display", "Roboto Mono"));
}

TEST(EmbeddedFontProviderTest, AllFamiliesReportBundledSource) {
  EmbeddedFontProvider provider;
  for (const FontFamilyInfo& info : provider.families()) {
    EXPECT_EQ(info.source, FontSource::Bundled) << info.family;
  }
}

TEST(EmbeddedFontProviderTest, SetSpansMultipleCategories) {
  EmbeddedFontProvider provider;
  std::vector<FontCategory> categories;
  for (const FontFamilyInfo& info : provider.families()) {
    categories.push_back(info.category);
  }
  EXPECT_THAT(categories, Contains(FontCategory::SansSerif));
  EXPECT_THAT(categories, Contains(FontCategory::Serif));
  EXPECT_THAT(categories, Contains(FontCategory::Monospace));
  EXPECT_THAT(categories, Contains(FontCategory::Display));
}

TEST(EmbeddedFontProviderTest, EveryAdvertisedFamilySuppliesEncodedWoff2Bytes) {
  EmbeddedFontProvider provider;
  for (const FontFamilyInfo& info : provider.families()) {
    const std::vector<uint8_t> data = provider.loadFamilyData(info.family, FontFaceRequest{});
    ASSERT_GE(data.size(), 4u) << info.family;
    EXPECT_THAT(std::span(data).first(4), ElementsAre('w', 'O', 'F', '2')) << info.family;
    const auto availability = provider.availability(info.family, {});
    EXPECT_EQ(availability.state, FontAssetState::Ready);
    EXPECT_EQ(availability.format, FontFileFormat::Woff2);
    EXPECT_EQ(availability.encodedBytes, data.size());
  }
}

TEST(EmbeddedFontProviderTest, LookupIsCaseInsensitive) {
  EmbeddedFontProvider provider;
  EXPECT_TRUE(provider.hasFamily("inter"));
  EXPECT_TRUE(provider.hasFamily("JETBRAINS MONO"));
  EXPECT_FALSE(provider.hasFamily("Definitely Not A Font"));
  EXPECT_TRUE(provider.loadFamilyData("Definitely Not A Font", FontFaceRequest{}).empty());
}

// --- FontCatalog aggregation + precedence -----------------------------------------------------

TEST(FontCatalogTest, PendingBundledFamilyDoesNotResolveToSameNamedSystemFont) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  std::vector<std::unique_ptr<FontFamilyProvider>> providers;
  providers.push_back(std::make_unique<EmbeddedFontProvider>(store));
  providers.push_back(std::make_unique<MarkerProvider>(std::vector<std::string>{"Inter"},
                                                       FontSource::System, 0x22));
  FontCatalog catalog(std::move(providers));
  EXPECT_EQ(catalog.availability("Inter", {}).state, FontAssetState::Absent);
  EXPECT_THAT(catalog.loadFamilyData("Inter", {}), IsEmpty());
}

TEST(FontCatalogTest, SynchronousCustomProviderRetainsEmptyByteFallback) {
  std::vector<std::unique_ptr<FontFamilyProvider>> providers;
  providers.push_back(
      std::make_unique<MarkerProvider>(std::vector<std::string>{"Shared"}, FontSource::Bundled, 0));
  providers.push_back(std::make_unique<MarkerProvider>(std::vector<std::string>{"Shared"},
                                                       FontSource::System, 0x22));
  FontCatalog catalog(std::move(providers));
  EXPECT_THAT(catalog.loadFamilyData("Shared", {}), ElementsAre(0x22));
}

TEST(FontCatalogTest, LegacyFallbackCannotBypassImmutableAssetAdmission) {
  class CountingBundledProvider : public EmbeddedFontProvider {
  public:
    std::vector<uint8_t> loadFamilyData(std::string_view family,
                                        const FontFaceRequest& request) const override {
      ++loads;
      return EmbeddedFontProvider::loadFamilyData(family, request);
    }
    mutable size_t loads = 0;
  };
  std::vector<std::unique_ptr<FontFamilyProvider>> providers;
  providers.push_back(
      std::make_unique<MarkerProvider>(std::vector<std::string>{"Inter"}, FontSource::System, 0));
  auto bundled = std::make_unique<CountingBundledProvider>();
  const auto* probe = bundled.get();
  providers.push_back(std::move(bundled));
  FontCatalog catalog(std::move(providers));
  ASSERT_THAT(catalog.availability("Inter", {}).contentId, IsEmpty());
  EXPECT_THAT(catalog.loadFamilyData("Inter", {}), IsEmpty());
  Registry registry;
  FontManager manager(registry);
  manager.setFontProvider(&catalog);
  EXPECT_EQ(manager.findFont("Inter"), manager.fallbackFont());
  EXPECT_EQ(probe->loads, 0u);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 0u);
}

TEST(FontCatalogTest, GroupsEmbeddedBeforeSystem) {
  std::vector<std::unique_ptr<FontFamilyProvider>> providers;
  providers.push_back(std::make_unique<MarkerProvider>(std::vector<std::string>{"EmbeddedOnly"},
                                                       FontSource::Bundled, 0x11));
  providers.push_back(std::make_unique<MarkerProvider>(std::vector<std::string>{"SystemOnly"},
                                                       FontSource::System, 0x22));
  FontCatalog catalog(std::move(providers));

  const std::vector<FontFamilyInfo> all = catalog.families();
  ASSERT_EQ(all.size(), 2u);
  // Embedded group is emitted first.
  EXPECT_EQ(all[0].source, FontSource::Bundled);
  EXPECT_EQ(all[0].family, "EmbeddedOnly");
  EXPECT_EQ(all[1].source, FontSource::System);
  EXPECT_EQ(all[1].family, "SystemOnly");
}

TEST(FontCatalogTest, EmbeddedShadowsSystemForDuplicateFamily) {
  std::vector<std::unique_ptr<FontFamilyProvider>> providers;
  providers.push_back(std::make_unique<MarkerProvider>(std::vector<std::string>{"Shared"},
                                                       FontSource::Bundled, 0xAB));
  providers.push_back(std::make_unique<MarkerProvider>(std::vector<std::string>{"Shared"},
                                                       FontSource::System, 0xCD));
  FontCatalog catalog(std::move(providers));

  // Only one "Shared" survives, tagged Embedded.
  const std::vector<FontFamilyInfo> all = catalog.families();
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].source, FontSource::Bundled);

  // And loadFace resolves to the embedded provider's bytes.
  const std::vector<uint8_t> data = catalog.loadFace("Shared");
  ASSERT_EQ(data.size(), 1u);
  EXPECT_EQ(data[0], 0xAB);
}

TEST(FontCatalogTest, FamiliesBySourceFilters) {
  std::vector<std::unique_ptr<FontFamilyProvider>> providers;
  providers.push_back(std::make_unique<MarkerProvider>(std::vector<std::string>{"E1", "E2"},
                                                       FontSource::Bundled, 0x01));
  providers.push_back(
      std::make_unique<MarkerProvider>(std::vector<std::string>{"S1"}, FontSource::System, 0x02));
  FontCatalog catalog(std::move(providers));

  EXPECT_THAT(familyNames(catalog.familiesBySource(FontSource::Bundled)),
              ::testing::ElementsAre("E1", "E2"));
  EXPECT_THAT(familyNames(catalog.familiesBySource(FontSource::System)),
              ::testing::ElementsAre("S1"));
}

TEST(FontCatalogTest, DefaultCatalogContainsEmbeddedFamilies) {
  FontCatalog catalog;
  EXPECT_TRUE(catalog.hasFamily("Inter"));
  const std::vector<FontFamilyInfo> embedded = catalog.familiesBySource(FontSource::Bundled);
  EXPECT_GE(embedded.size(), 8u);
}

// --- SystemFontProvider (macOS only) ----------------------------------------------------------

#ifdef __APPLE__
TEST(SystemFontProviderTest, EnumeratesSystemFamilies) {
  SystemFontProvider provider;
  ASSERT_TRUE(SystemFontProvider::isSupported());

  const std::vector<FontFamilyInfo> families = provider.families();
  EXPECT_FALSE(families.empty());
  for (const FontFamilyInfo& info : families) {
    EXPECT_EQ(info.source, FontSource::System);
    // Hidden dot-prefixed system families must be filtered out.
    EXPECT_FALSE(info.family.empty());
    EXPECT_NE(info.family.front(), '.');
  }
}

TEST(SystemFontProviderTest, FamilyLookupMatchesPresentAbsentAndCaseVariants) {
  SystemFontProvider provider;
  ASSERT_TRUE(SystemFontProvider::isSupported());

  const std::vector<FontFamilyInfo> families = provider.families();
  ASSERT_FALSE(families.empty());
  const std::string present = families.front().family;

  EXPECT_TRUE(provider.hasFamily(present));

  // Layout folds family names when it indexes them, so the provider has to agree on casing.
  std::string upper = present;
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  std::string lower = present;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  EXPECT_TRUE(provider.hasFamily(upper));
  EXPECT_TRUE(provider.hasFamily(lower));

  EXPECT_FALSE(provider.hasFamily("Definitely Not An Installed Family"));
  EXPECT_FALSE(provider.hasFamily(""));
}

TEST(SystemFontProviderTest, LoadsSfntForKnownSystemFamily) {
  SystemFontProvider provider;
  // Helvetica ships on every macOS; if enumeration found it, its bytes must reconstruct.
  if (!provider.hasFamily("Helvetica")) {
    GTEST_SKIP() << "Helvetica not available on this host";
  }
  const std::vector<uint8_t> data = provider.loadFamilyData("Helvetica", FontFaceRequest{});
  ASSERT_GE(data.size(), 4u);
  const uint32_t magic = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
                         (uint32_t(data[2]) << 8) | uint32_t(data[3]);
  EXPECT_TRUE(magic == 0x00010000u || magic == 0x4F54544Fu /*OTTO*/ ||
              magic == 0x74727565u /*true*/)
      << "unexpected sfnt magic: " << std::hex << magic;
}

TEST(SystemFontProviderTest, UnknownFamilyReturnsEmpty) {
  SystemFontProvider provider;
  EXPECT_FALSE(provider.hasFamily("Definitely Not Installed Font XYZ"));
  EXPECT_TRUE(
      provider.loadFamilyData("Definitely Not Installed Font XYZ", FontFaceRequest{}).empty());
}

// Regression: a catalog family is a *set* of faces, and `font-weight`/`font-style` pick which one.
// Resolving them all to the family's default face is what makes bold and italic render as regular.
TEST(FontManagerCatalogFaceTest, BoldResolvesToADistinctFaceFromRegular) {
  SystemFontProvider probe;
  if (!probe.hasFamily("Helvetica")) {
    GTEST_SKIP() << "Helvetica not available on this host";
  }

  FontCatalog catalog;
  Registry registry;
  FontManager manager(registry);
  manager.setFontProvider(&catalog);

  const std::span<const uint8_t> regular = manager.fontData(manager.findFont("Helvetica", 400));
  const std::span<const uint8_t> bold = manager.fontData(manager.findFont("Helvetica", 700));

  ASSERT_THAT(regular, Not(IsEmpty()));
  ASSERT_THAT(bold, Not(IsEmpty()));
  EXPECT_FALSE(std::ranges::equal(regular, bold))
      << "font-weight:700 resolved to the same face bytes as font-weight:400";
}

TEST(FontManagerCatalogFaceTest, ItalicResolvesToADistinctFaceFromUpright) {
  SystemFontProvider probe;
  if (!probe.hasFamily("Helvetica")) {
    GTEST_SKIP() << "Helvetica not available on this host";
  }

  FontCatalog catalog;
  Registry registry;
  FontManager manager(registry);
  manager.setFontProvider(&catalog);

  const std::span<const uint8_t> upright =
      manager.fontData(manager.findFont("Helvetica", 400, static_cast<int>(FontStyle::Normal), 5));
  const std::span<const uint8_t> italic =
      manager.fontData(manager.findFont("Helvetica", 400, static_cast<int>(FontStyle::Italic), 5));

  ASSERT_THAT(upright, Not(IsEmpty()));
  ASSERT_THAT(italic, Not(IsEmpty()));
  EXPECT_FALSE(std::ranges::equal(upright, italic))
      << "font-style:italic resolved to the same face bytes as font-style:normal";
}
#else
TEST(SystemFontProviderTest, StubEnumeratesNothing) {
  SystemFontProvider provider;
  EXPECT_FALSE(SystemFontProvider::isSupported());
  EXPECT_TRUE(provider.families().empty());
  EXPECT_FALSE(provider.hasFamily("Helvetica"));
}
#endif

}  // namespace donner::svg
