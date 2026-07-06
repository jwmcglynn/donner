#include "donner/svg/resources/FontCatalog.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

#include "donner/svg/resources/EmbeddedFontProvider.h"
#include "donner/svg/resources/SystemFontProvider.h"

namespace donner::svg {

namespace {

using ::testing::Contains;
using ::testing::IsSupersetOf;

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

  std::vector<uint8_t> loadFamilyData(std::string_view family) const override {
    if (!hasFamily(family)) {
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

  // A representative sample from each style bucket must be present.
  EXPECT_THAT(names, IsSupersetOf({"Inter", "Bitter", "JetBrains Mono", "Oswald", "Pacifico"}));
  EXPECT_GE(names.size(), 8u);
  EXPECT_LE(names.size(), 12u);
}

TEST(EmbeddedFontProviderTest, AllFamiliesReportEmbeddedSource) {
  EmbeddedFontProvider provider;
  for (const FontFamilyInfo& info : provider.families()) {
    EXPECT_EQ(info.source, FontSource::Embedded) << info.family;
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

TEST(EmbeddedFontProviderTest, LoadsValidSfntBytes) {
  EmbeddedFontProvider provider;
  const std::vector<uint8_t> data = provider.loadFamilyData("Inter");
  ASSERT_GE(data.size(), 4u);
  // TrueType sfnt magic 0x00010000.
  EXPECT_EQ(data[0], 0x00);
  EXPECT_EQ(data[1], 0x01);
  EXPECT_EQ(data[2], 0x00);
  EXPECT_EQ(data[3], 0x00);
}

TEST(EmbeddedFontProviderTest, LookupIsCaseInsensitive) {
  EmbeddedFontProvider provider;
  EXPECT_TRUE(provider.hasFamily("inter"));
  EXPECT_TRUE(provider.hasFamily("JETBRAINS MONO"));
  EXPECT_FALSE(provider.hasFamily("Definitely Not A Font"));
  EXPECT_TRUE(provider.loadFamilyData("Definitely Not A Font").empty());
}

// --- FontCatalog aggregation + precedence -----------------------------------------------------

TEST(FontCatalogTest, GroupsEmbeddedBeforeSystem) {
  std::vector<std::unique_ptr<FontFamilyProvider>> providers;
  providers.push_back(
      std::make_unique<MarkerProvider>(std::vector<std::string>{"EmbeddedOnly"},
                                       FontSource::Embedded, 0x11));
  providers.push_back(std::make_unique<MarkerProvider>(std::vector<std::string>{"SystemOnly"},
                                                       FontSource::System, 0x22));
  FontCatalog catalog(std::move(providers));

  const std::vector<FontFamilyInfo> all = catalog.families();
  ASSERT_EQ(all.size(), 2u);
  // Embedded group is emitted first.
  EXPECT_EQ(all[0].source, FontSource::Embedded);
  EXPECT_EQ(all[0].family, "EmbeddedOnly");
  EXPECT_EQ(all[1].source, FontSource::System);
  EXPECT_EQ(all[1].family, "SystemOnly");
}

TEST(FontCatalogTest, EmbeddedShadowsSystemForDuplicateFamily) {
  std::vector<std::unique_ptr<FontFamilyProvider>> providers;
  providers.push_back(std::make_unique<MarkerProvider>(std::vector<std::string>{"Shared"},
                                                       FontSource::Embedded, 0xAB));
  providers.push_back(std::make_unique<MarkerProvider>(std::vector<std::string>{"Shared"},
                                                       FontSource::System, 0xCD));
  FontCatalog catalog(std::move(providers));

  // Only one "Shared" survives, tagged Embedded.
  const std::vector<FontFamilyInfo> all = catalog.families();
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].source, FontSource::Embedded);

  // And loadFace resolves to the embedded provider's bytes.
  const std::vector<uint8_t> data = catalog.loadFace("Shared");
  ASSERT_EQ(data.size(), 1u);
  EXPECT_EQ(data[0], 0xAB);
}

TEST(FontCatalogTest, FamiliesBySourceFilters) {
  std::vector<std::unique_ptr<FontFamilyProvider>> providers;
  providers.push_back(std::make_unique<MarkerProvider>(
      std::vector<std::string>{"E1", "E2"}, FontSource::Embedded, 0x01));
  providers.push_back(std::make_unique<MarkerProvider>(std::vector<std::string>{"S1"},
                                                       FontSource::System, 0x02));
  FontCatalog catalog(std::move(providers));

  EXPECT_THAT(familyNames(catalog.familiesBySource(FontSource::Embedded)),
              ::testing::ElementsAre("E1", "E2"));
  EXPECT_THAT(familyNames(catalog.familiesBySource(FontSource::System)),
              ::testing::ElementsAre("S1"));
}

TEST(FontCatalogTest, DefaultCatalogContainsEmbeddedFamilies) {
  FontCatalog catalog;
  EXPECT_TRUE(catalog.hasFamily("Inter"));
  const std::vector<FontFamilyInfo> embedded = catalog.familiesBySource(FontSource::Embedded);
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

TEST(SystemFontProviderTest, LoadsSfntForKnownSystemFamily) {
  SystemFontProvider provider;
  // Helvetica ships on every macOS; if enumeration found it, its bytes must reconstruct.
  if (!provider.hasFamily("Helvetica")) {
    GTEST_SKIP() << "Helvetica not available on this host";
  }
  const std::vector<uint8_t> data = provider.loadFamilyData("Helvetica");
  ASSERT_GE(data.size(), 4u);
  const uint32_t magic = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
                         (uint32_t(data[2]) << 8) | uint32_t(data[3]);
  EXPECT_TRUE(magic == 0x00010000u || magic == 0x4F54544Fu /*OTTO*/ || magic == 0x74727565u /*true*/)
      << "unexpected sfnt magic: " << std::hex << magic;
}

TEST(SystemFontProviderTest, UnknownFamilyReturnsEmpty) {
  SystemFontProvider provider;
  EXPECT_FALSE(provider.hasFamily("Definitely Not Installed Font XYZ"));
  EXPECT_TRUE(provider.loadFamilyData("Definitely Not Installed Font XYZ").empty());
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
