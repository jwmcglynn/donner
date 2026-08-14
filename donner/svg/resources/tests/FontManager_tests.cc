#include "donner/svg/resources/FontManager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include "donner/base/fonts/SfntUtils.h"
#include "embed_resources/PublicSansFont.h"

namespace donner::svg {

struct FontManagerTestAccess {
  static bool ReplaceFontData(FontManager& manager, FontHandle handle,
                              std::span<const uint8_t> data,
                              FontDataTrust trust = FontDataTrust::Untrusted) {
    return manager.loadFontDataIntoEntity(handle.entity(), data, trust);
  }

  static bool HasPersistentBudgetState(const Registry& registry) {
    return registry.ctx().contains<FontManager::FontBudgetContext>();
  }
};

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

size_t RetainedCharge(std::span<const uint8_t> data) {
  auto sfnt = fonts::SfntFont::Validate(data);
  EXPECT_TRUE(sfnt.has_value());
  return data.size() + (sfnt ? sfnt->retainedBytes() : 0);
}

}  // namespace

TEST(FontManagerTest, FallbackFontLoads) {
  Registry registry;
  FontManager mgr(registry);
  FontHandle handle = mgr.fallbackFont();
  EXPECT_TRUE(static_cast<bool>(handle));
  EXPECT_TRUE(mgr.isTrustedFont(handle));

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
  EXPECT_FALSE(mgr.isTrustedFont(handle));

  EXPECT_FALSE(mgr.fontData(handle).empty());
}

TEST(FontManagerTest, EnforcesAggregateLoadedFontBudget) {
  Registry registry;
  std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                            embedded::kPublicSansMediumOtf.end());
  FontManager mgr(registry, RetainedCharge(data));

  EXPECT_TRUE(static_cast<bool>(mgr.loadFontData(data)));
  EXPECT_FALSE(static_cast<bool>(mgr.loadFontData(data)));
}

TEST(FontManagerTest, AccountsExactFontAndCachedIndexBytes) {
  Registry registry;
  const std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                                  embedded::kPublicSansMediumOtf.end());
  const size_t charge = RetainedCharge(data);
  FontManager mgr(registry, charge * 2);

  EXPECT_EQ(mgr.loadedFontBytes(), 0u);
  EXPECT_EQ(mgr.numLoadedFonts(), 0u);
  ASSERT_TRUE(static_cast<bool>(mgr.loadFontData(data)));
  EXPECT_EQ(mgr.loadedFontBytes(), charge);
  EXPECT_EQ(mgr.numLoadedFonts(), 1u);
  ASSERT_TRUE(static_cast<bool>(mgr.loadFontData(data)));
  EXPECT_EQ(mgr.loadedFontBytes(), charge * 2);
  EXPECT_EQ(mgr.numLoadedFonts(), 2u);
}

TEST(FontManagerTest, EnforcesLoadedFontCountWithoutLeakingBudget) {
  Registry registry;
  const std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                                  embedded::kPublicSansMediumOtf.end());
  FontManager mgr(registry, RetainedCharge(data) * 2, 1);

  ASSERT_TRUE(static_cast<bool>(mgr.loadFontData(data)));
  EXPECT_FALSE(static_cast<bool>(mgr.loadFontData(data)));
  EXPECT_EQ(mgr.numLoadedFonts(), 1u);
  EXPECT_EQ(mgr.loadedFontBytes(), RetainedCharge(data));
}

TEST(FontManagerTest, RejectedFontDoesNotConsumeAggregateBudget) {
  Registry registry;
  FontManager mgr(registry);
  const std::vector<uint8_t> invalid = {0x00, 0x01, 0x00, 0x00};

  EXPECT_FALSE(static_cast<bool>(mgr.loadFontData(invalid)));
  EXPECT_EQ(mgr.loadedFontBytes(), 0u);
  EXPECT_EQ(mgr.numLoadedFonts(), 0u);
}

TEST(FontManagerTest, ReplacementUpdatesExactAggregateCharge) {
  Registry registry;
  const std::vector<uint8_t> original(embedded::kPublicSansMediumOtf.begin(),
                                      embedded::kPublicSansMediumOtf.end());
  std::vector<uint8_t> replacement = original;
  replacement.resize(replacement.size() + 17, 0);
  FontManager mgr(registry, RetainedCharge(replacement));

  const FontHandle handle = mgr.loadFontData(original);
  ASSERT_TRUE(static_cast<bool>(handle));
  ASSERT_TRUE(FontManagerTestAccess::ReplaceFontData(mgr, handle, replacement));
  EXPECT_EQ(mgr.fontData(handle).size(), replacement.size());
  EXPECT_EQ(mgr.loadedFontBytes(), RetainedCharge(replacement));
  EXPECT_EQ(mgr.numLoadedFonts(), 1u);
}

TEST(FontManagerTest, RejectedReplacementPreservesOriginalReservation) {
  Registry registry;
  const std::vector<uint8_t> original(embedded::kPublicSansMediumOtf.begin(),
                                      embedded::kPublicSansMediumOtf.end());
  std::vector<uint8_t> oversized = original;
  oversized.resize(oversized.size() + 1, 0);
  const size_t originalCharge = RetainedCharge(original);
  FontManager mgr(registry, originalCharge);

  const FontHandle handle = mgr.loadFontData(original);
  ASSERT_TRUE(static_cast<bool>(handle));
  EXPECT_FALSE(FontManagerTestAccess::ReplaceFontData(mgr, handle, oversized));
  EXPECT_EQ(mgr.fontData(handle).size(), original.size());
  EXPECT_EQ(mgr.loadedFontBytes(), originalCharge);
  EXPECT_EQ(mgr.numLoadedFonts(), 1u);
}

TEST(FontManagerTest, EntityDestructionAndManagerLifecycleReleaseReservations) {
  Registry registry;
  const std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                                  embedded::kPublicSansMediumOtf.end());
  const size_t charge = RetainedCharge(data);
  FontHandle handle;
  {
    FontManager first(registry, charge);
    handle = first.loadFontData(data);
    ASSERT_TRUE(static_cast<bool>(handle));
    EXPECT_EQ(first.loadedFontBytes(), charge);
  }

  FontManager second(registry, charge * 2);
  EXPECT_EQ(second.loadedFontBytes(), charge);
  EXPECT_EQ(second.numLoadedFonts(), 1u);
  registry.destroy(handle.entity());
  EXPECT_EQ(second.loadedFontBytes(), 0u);
  EXPECT_EQ(second.numLoadedFonts(), 0u);
  EXPECT_TRUE(static_cast<bool>(second.loadFontData(data)));
}

TEST(FontManagerTest, ContextOwnedManagerSharesBudgetAcrossManagerLifetimes) {
  Registry registry;
  const std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                                  embedded::kPublicSansMediumOtf.end());
  const size_t charge = RetainedCharge(data);

  FontManager& contextManager = registry.ctx().emplace<FontManager>(registry, charge, 1);
  FontManager peerManager(registry, charge * 2, 2);
  const FontHandle handle = contextManager.loadFontData(data);
  ASSERT_TRUE(static_cast<bool>(handle));
  EXPECT_EQ(contextManager.loadedFontBytes(), charge);
  EXPECT_EQ(contextManager.numLoadedFonts(), 1u);
  EXPECT_EQ(peerManager.loadedFontBytes(), charge);
  EXPECT_EQ(peerManager.numLoadedFonts(), 1u);
  EXPECT_FALSE(static_cast<bool>(peerManager.loadFontData(data)));

  registry.ctx().erase<FontManager>();
  EXPECT_EQ(peerManager.loadedFontBytes(), charge);
  EXPECT_EQ(peerManager.numLoadedFonts(), 1u);

  registry.destroy(handle.entity());
  EXPECT_EQ(peerManager.loadedFontBytes(), 0u);
  EXPECT_EQ(peerManager.numLoadedFonts(), 0u);
  EXPECT_TRUE(static_cast<bool>(peerManager.loadFontData(data)));
}

TEST(FontManagerTest, ConstBudgetQueriesDoNotInstallContextOrSelectCaps) {
  Registry registry;
  const std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                                  embedded::kPublicSansMediumOtf.end());
  const size_t charge = RetainedCharge(data);
  FontManager smallerBudget(registry, charge, 1);
  FontManager largerBudget(registry, charge * 2, 2);
  const FontManager& constSmallerBudget = smallerBudget;
  const FontManager& constLargerBudget = largerBudget;

  EXPECT_FALSE(FontManagerTestAccess::HasPersistentBudgetState(registry));
  EXPECT_EQ(constSmallerBudget.loadedFontBytes(), 0u);
  EXPECT_EQ(constSmallerBudget.numLoadedFonts(), 0u);
  EXPECT_EQ(constLargerBudget.loadedFontBytes(), 0u);
  EXPECT_EQ(constLargerBudget.numLoadedFonts(), 0u);
  EXPECT_FALSE(FontManagerTestAccess::HasPersistentBudgetState(registry));

  ASSERT_TRUE(static_cast<bool>(largerBudget.loadFontData(data)));
  ASSERT_TRUE(static_cast<bool>(largerBudget.loadFontData(data)));
  EXPECT_FALSE(static_cast<bool>(smallerBudget.loadFontData(data)));
  EXPECT_EQ(smallerBudget.loadedFontBytes(), charge * 2);
  EXPECT_EQ(smallerBudget.numLoadedFonts(), 2u);
}

TEST(FontManagerTest, ConcurrentBudgetQueriesAreReadOnly) {
  Registry registry;
  const std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                                  embedded::kPublicSansMediumOtf.end());
  const size_t charge = RetainedCharge(data);
  FontManager manager(registry, charge, 1);
  FontManager peerManager(registry, charge * 2, 2);
  ASSERT_TRUE(static_cast<bool>(manager.loadFontData(data)));
  const FontManager& constManager = manager;
  const FontManager& constPeerManager = peerManager;

  constexpr int kThreadCount = 4;
  constexpr int kReadsPerThread = 1000;
  std::atomic<bool> start{false};
  std::atomic<bool> sawMismatch{false};
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {}
      for (int read = 0; read < kReadsPerThread; ++read) {
        if (constManager.loadedFontBytes() != charge || constManager.numLoadedFonts() != 1u ||
            constPeerManager.loadedFontBytes() != charge ||
            constPeerManager.numLoadedFonts() != 1u) {
          sawMismatch.store(true, std::memory_order_relaxed);
        }
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_FALSE(sawMismatch.load(std::memory_order_relaxed));
}

TEST(FontManagerTest, LoadWoff1Data) {
  Registry registry;
  FontManager mgr(registry);

  std::vector<uint8_t> woffData = readFile("donner/base/fonts/testdata/valid-001.woff");
  ASSERT_FALSE(woffData.empty()) << "Could not read WOFF test file";

  FontHandle handle = mgr.loadFontData(woffData);
  EXPECT_TRUE(static_cast<bool>(handle));
  EXPECT_FALSE(mgr.isTrustedFont(handle));
  EXPECT_FALSE(mgr.fontData(handle).empty());

  FontHandle trustedHandle = mgr.loadFontData(woffData, FontDataTrust::Trusted);
  EXPECT_TRUE(static_cast<bool>(trustedHandle));
  EXPECT_TRUE(mgr.isTrustedFont(trustedHandle));
}

TEST(FontManagerTest, ReplacementUsesNewTrustAndRejectedReplacementPreservesOldTrust) {
  Registry registry;
  const std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                                  embedded::kPublicSansMediumOtf.end());
  FontManager mgr(registry);

  const FontHandle handle = mgr.loadFontData(data, FontDataTrust::Trusted);
  ASSERT_TRUE(static_cast<bool>(handle));
  ASSERT_TRUE(mgr.isTrustedFont(handle));

  ASSERT_TRUE(FontManagerTestAccess::ReplaceFontData(mgr, handle, data, FontDataTrust::Untrusted));
  EXPECT_FALSE(mgr.isTrustedFont(handle));

  ASSERT_TRUE(FontManagerTestAccess::ReplaceFontData(mgr, handle, data, FontDataTrust::Trusted));
  EXPECT_TRUE(mgr.isTrustedFont(handle));

  const std::vector<uint8_t> invalid = {0x00, 0x01, 0x00, 0x00};
  EXPECT_FALSE(
      FontManagerTestAccess::ReplaceFontData(mgr, handle, invalid, FontDataTrust::Untrusted));
  EXPECT_TRUE(mgr.isTrustedFont(handle));
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
  EXPECT_FALSE(mgr.isTrustedFont(invalid));
}

TEST(FontManagerTest, FontDataReturnsNonEmpty) {
  Registry registry;
  FontManager mgr(registry);
  FontHandle handle = mgr.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(handle));

  auto data = mgr.fontData(handle);
  EXPECT_FALSE(data.empty());
}

TEST(FontManagerTest, RejectsTruncatedRawSfnt) {
  Registry registry;
  FontManager mgr(registry);
  const std::vector<uint8_t> truncatedSfnt = {0x00, 0x01, 0x00, 0x00};

  EXPECT_FALSE(static_cast<bool>(mgr.loadFontData(truncatedSfnt)));
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
  EXPECT_FALSE(mgr.isTrustedFont(handle));

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
  EXPECT_TRUE(mgr.isTrustedFont(handle));
  // Resolved through the provider, not the Public Sans fallback entity.
  EXPECT_NE(handle, mgr.fallbackFont());
  EXPECT_EQ(provider.loadCalls, 1);
  EXPECT_FALSE(mgr.fontData(handle).empty());
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
