#include "donner/svg/resources/FontManager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "donner/base/fonts/SfntUtils.h"
#include "donner/svg/core/FontStretch.h"
#include "donner/svg/core/FontStyle.h"
#include "embed_resources/PublicSansFont.h"

using testing::Eq;

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

  static size_t NumValidationRejectedSources(const FontManager& manager) {
    return manager.numValidationRejectedSources();
  }

  static size_t CompressedFontDecompressionAttempts(const FontManager& manager) {
    return manager.compressedFontDecompressionAttempts();
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

std::vector<uint8_t> WithCompressedFlavor(std::vector<uint8_t> data, uint32_t flavor) {
  EXPECT_GE(data.size(), 8u);
  if (data.size() < 8) return {};
  data[4] = static_cast<uint8_t>(flavor >> 24);
  data[5] = static_cast<uint8_t>(flavor >> 16);
  data[6] = static_cast<uint8_t>(flavor >> 8);
  data[7] = static_cast<uint8_t>(flavor);
  return data;
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

  std::vector<uint8_t> loadFamilyData(std::string_view family,
                                      const FontFaceRequest& request) const override {
    ++loadCalls;
    lastRequest = request;
    if (!hasFamily(family)) {
      return {};
    }
    return std::vector<uint8_t>(embedded::kPublicSansMediumOtf.begin(),
                                embedded::kPublicSansMediumOtf.end());
  }

  mutable int loadCalls = 0;
  mutable FontFaceRequest lastRequest;

private:
  std::vector<std::string> families_;
};

/// A provider that genuinely serves different bytes per requested face, which is what the
/// interface allows: a family is a set of faces, and asking for bold is a different question from
/// asking for regular. Every answer is Public Sans with a distinct amount of trailing padding, so
/// the loaded byte count says which face was served.
class VariantFontProvider : public FontFamilyProvider {
public:
  explicit VariantFontProvider(std::string family) : family_(std::move(family)) {}

  /// Trailing padding, and so the loaded size, that @p request is answered with.
  static size_t PaddingFor(const FontFaceRequest& request) {
    return 1u + static_cast<size_t>(request.weight) +
           1000u * static_cast<size_t>(static_cast<int>(request.style)) +
           100000u * static_cast<size_t>(static_cast<int>(request.stretch));
  }

  /// The exact loaded byte count this provider answers @p request with.
  static size_t SizeFor(const FontFaceRequest& request) {
    return embedded::kPublicSansMediumOtf.size() + PaddingFor(request);
  }

  std::vector<FontFamilyInfo> families() const override {
    return {FontFamilyInfo{family_, FontSource::Embedded, FontCategory::SansSerif}};
  }

  bool hasFamily(std::string_view family) const override { return family == family_; }

  std::vector<uint8_t> loadFamilyData(std::string_view family,
                                      const FontFaceRequest& request) const override {
    ++loadCalls;
    if (!hasFamily(family)) {
      return {};
    }
    // Padding after the last table leaves a valid sfnt whose size identifies the face.
    std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                              embedded::kPublicSansMediumOtf.end());
    data.resize(SizeFor(request), 0);
    return data;
  }

  mutable int loadCalls = 0;

private:
  std::string family_;
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
  const size_t validationWorkAfterAdmission = mgr.fontValidationWork();
  EXPECT_GT(validationWorkAfterAdmission, 0u);
  EXPECT_FALSE(static_cast<bool>(mgr.loadFontData(data)));
  EXPECT_EQ(mgr.fontValidationWork(), validationWorkAfterAdmission);
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

TEST(FontManagerTest, CffValidationWorkIsAggregateAcrossManagersAndAttempts) {
  Registry registry;
  const std::vector<uint8_t> cff(embedded::kPublicSansMediumOtf.begin(),
                                 embedded::kPublicSansMediumOtf.end());
  FontManager first(registry, FontManager::kDefaultMaximumLoadedFontBytes,
                    FontManager::kDefaultMaximumLoadedFonts, 1);

  EXPECT_FALSE(static_cast<bool>(first.loadFontData(cff)));
  EXPECT_EQ(first.fontValidationWork(), 1u);

  FontManager peer(registry, FontManager::kDefaultMaximumLoadedFontBytes,
                   FontManager::kDefaultMaximumLoadedFonts,
                   FontManager::kDefaultMaximumFontValidationWork);
  EXPECT_EQ(peer.fontValidationWork(), 1u);
  EXPECT_FALSE(static_cast<bool>(peer.loadFontData(cff)));
  EXPECT_EQ(peer.fontValidationWork(), 1u);
}

TEST(FontManagerTest, ExhaustedCffWorkBudgetPreservesReplacementAndAllowsTrueType) {
  Registry registry;
  const std::vector<uint8_t> trueType = readFile("third_party/roboto/Roboto-Regular.ttf");
  const std::vector<uint8_t> cff(embedded::kPublicSansMediumOtf.begin(),
                                 embedded::kPublicSansMediumOtf.end());
  ASSERT_FALSE(trueType.empty());
  FontManager manager(registry, FontManager::kDefaultMaximumLoadedFontBytes,
                      FontManager::kDefaultMaximumLoadedFonts, 0);

  const FontHandle handle = manager.loadFontData(trueType);
  ASSERT_TRUE(static_cast<bool>(handle));
  const std::vector<uint8_t> original(manager.fontData(handle).begin(),
                                      manager.fontData(handle).end());
  EXPECT_FALSE(FontManagerTestAccess::ReplaceFontData(manager, handle, cff));
  ASSERT_EQ(manager.fontData(handle).size(), original.size());
  EXPECT_THAT(manager.fontData(handle), testing::ElementsAreArray(original));
  EXPECT_EQ(manager.fontValidationWork(), 0u);

  EXPECT_TRUE(FontManagerTestAccess::ReplaceFontData(manager, handle, trueType));
  EXPECT_TRUE(static_cast<bool>(manager.glyphOutlineComplexity(handle, 1)));
}

TEST(FontManagerTest, SuccessfulCffLoadChargesMeasuredValidationWork) {
  Registry registry;
  FontManager manager(registry);
  const std::vector<uint8_t> cff(embedded::kPublicSansMediumOtf.begin(),
                                 embedded::kPublicSansMediumOtf.end());

  ASSERT_TRUE(static_cast<bool>(manager.loadFontData(cff)));
  EXPECT_GT(manager.fontValidationWork(), 0u);
}

TEST(FontManagerTest, PermanentlyRejectedFontFaceSourceIsNotRevalidated) {
  Registry registry;
  FontManager manager(registry, FontManager::kDefaultMaximumLoadedFontBytes,
                      FontManager::kDefaultMaximumLoadedFonts, 1);
  css::FontFace face;
  face.familyName = RcString("RejectedCff");
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = std::make_shared<const std::vector<uint8_t>>(
      embedded::kPublicSansMediumOtf.begin(), embedded::kPublicSansMediumOtf.end());
  face.sources.push_back(std::move(source));
  manager.addFontFace(face);

  const FontHandle first = manager.findFont("RejectedCff");
  ASSERT_TRUE(static_cast<bool>(first));
  EXPECT_EQ(manager.fontValidationWork(), 1u);
  EXPECT_EQ(FontManagerTestAccess::NumValidationRejectedSources(manager), 1u);

  EXPECT_EQ(manager.findFont("RejectedCff"), first);
  EXPECT_EQ(manager.fontValidationWork(), 1u);
  EXPECT_EQ(FontManagerTestAccess::NumValidationRejectedSources(manager), 1u);
}

TEST(FontManagerTest, ExhaustedCffWorkDoesNotGrowRejectionMetadataForDistinctSources) {
  Registry registry;
  FontManager manager(registry, FontManager::kDefaultMaximumLoadedFontBytes, 256, 1);
  FontHandle fallback;

  for (int sourceIndex = 0; sourceIndex < 128; ++sourceIndex) {
    css::FontFace face;
    face.familyName = RcString("RejectedCff" + std::to_string(sourceIndex));
    css::FontFaceSource source;
    source.kind = css::FontFaceSource::Kind::Data;
    source.payload = std::make_shared<const std::vector<uint8_t>>(
        embedded::kPublicSansMediumOtf.begin(), embedded::kPublicSansMediumOtf.end());
    face.sources.push_back(std::move(source));
    manager.addFontFace(face);

    const FontHandle resolved = manager.findFont(face.familyName);
    ASSERT_TRUE(static_cast<bool>(resolved));
    if (sourceIndex == 0) {
      fallback = resolved;
    } else {
      EXPECT_EQ(resolved, fallback);
    }
  }

  EXPECT_EQ(manager.fontValidationWork(), 1u);
  EXPECT_EQ(FontManagerTestAccess::NumValidationRejectedSources(manager), 1u);
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
  const auto complexity = mgr.glyphOutlineComplexity(handle, 1);
  ASSERT_TRUE(complexity.has_value());

  FontHandle trustedHandle = mgr.loadFontData(woffData, FontDataTrust::Trusted);
  EXPECT_TRUE(static_cast<bool>(trustedHandle));
  EXPECT_TRUE(mgr.isTrustedFont(trustedHandle));
}

TEST(FontManagerTest, CffWorkExhaustionRejectsMislabeledCompressedFontsBeforeDecompression) {
  Registry registry;
  FontManager manager(registry, FontManager::kDefaultMaximumLoadedFontBytes,
                      FontManager::kDefaultMaximumLoadedFonts, 1);
  const std::vector<uint8_t> cff(embedded::kPublicSansMediumOtf.begin(),
                                 embedded::kPublicSansMediumOtf.end());
  EXPECT_FALSE(static_cast<bool>(manager.loadFontData(cff)));
  ASSERT_EQ(manager.fontValidationWork(), 1u);

  const std::vector<uint8_t> mislabeledWoff =
      WithCompressedFlavor(readFile("donner/base/fonts/testdata/valid-001.woff"), 0x00010000);
  ASSERT_FALSE(mislabeledWoff.empty());
  EXPECT_FALSE(static_cast<bool>(manager.loadFontData(mislabeledWoff)));
  EXPECT_FALSE(static_cast<bool>(manager.loadFontData(std::vector<uint8_t>(mislabeledWoff))));

#ifdef DONNER_TEXT_WOFF2_ENABLED
  const std::vector<uint8_t> mislabeledWoff2 =
      WithCompressedFlavor(readFile("donner/base/fonts/testdata/valid-001.woff2"), 0x00010000);
  ASSERT_FALSE(mislabeledWoff2.empty());
  EXPECT_FALSE(static_cast<bool>(manager.loadFontData(mislabeledWoff2)));
#endif

  EXPECT_EQ(FontManagerTestAccess::CompressedFontDecompressionAttempts(manager), 0u);

  const std::vector<uint8_t> trueType = readFile("third_party/roboto/Roboto-Regular.ttf");
  ASSERT_FALSE(trueType.empty());
  EXPECT_TRUE(static_cast<bool>(manager.loadFontData(trueType)));
  EXPECT_EQ(FontManagerTestAccess::CompressedFontDecompressionAttempts(manager), 0u);
}

TEST(FontManagerTest, MislabeledCompressedCffStillLoadsBeforeValidationBudgetExhaustion) {
  Registry registry;
  FontManager manager(registry);
  const std::vector<uint8_t> mislabeledWoff =
      WithCompressedFlavor(readFile("donner/base/fonts/testdata/valid-001.woff"), 0x00010000);
  ASSERT_FALSE(mislabeledWoff.empty());

  const FontHandle handle = manager.loadFontData(mislabeledWoff);
  ASSERT_TRUE(static_cast<bool>(handle));
  EXPECT_GT(manager.fontValidationWork(), 0u);
  EXPECT_EQ(FontManagerTestAccess::CompressedFontDecompressionAttempts(manager), 1u);
  EXPECT_TRUE(FontManagerTestAccess::ReplaceFontData(manager, handle, mislabeledWoff));
  EXPECT_EQ(FontManagerTestAccess::CompressedFontDecompressionAttempts(manager), 2u);
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

/// A style recompute re-announces the document's whole `@font-face` set. Minting a fresh entity
/// for each unchanged declaration would give every text run a new font identity on every unrelated
/// document mutation, invalidating every cache keyed on the font (glyph outlines, backend faces)
/// and stranding a second copy of the bytes on the abandoned entity.
TEST(FontManagerTest, ReAnnouncingUnchangedFontFacesKeepsOneFontIdentity) {
  Registry registry;
  FontManager mgr(registry);

  css::FontFace face;
  face.familyName = RcString("TestFont");
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = std::make_shared<const std::vector<uint8_t>>(
      embedded::kPublicSansMediumOtf.begin(), embedded::kPublicSansMediumOtf.end());
  face.sources.push_back(std::move(source));

  mgr.addFontFace(face);
  const FontHandle first = mgr.findFont("TestFont");
  ASSERT_TRUE(static_cast<bool>(first));
  const size_t bytesAfterFirstLoad = mgr.loadedFontBytes();
  EXPECT_EQ(mgr.numLoadedFonts(), 1u);

  for (int recompute = 0; recompute < 20; ++recompute) {
    mgr.addFontFace(face);
    EXPECT_EQ(mgr.findFont("TestFont"), first)
        << "Font identity changed at recompute " << recompute;
    EXPECT_EQ(mgr.numLoadedFonts(), 1u) << "Duplicate font load at recompute " << recompute;
    EXPECT_EQ(mgr.loadedFontBytes(), bytesAfterFirstLoad)
        << "Duplicate font bytes charged at recompute " << recompute;
  }
}

/// Identity reuse is keyed on the declaration, so a rule that names the same family but points at
/// different bytes is a different declaration: it takes a new entity, and the bytes behind the
/// original handle are left exactly as they were. Callers cache parsed faces and rendered outlines
/// against a handle, so swapping bytes underneath a live handle would serve one font's cached
/// geometry for another's.
TEST(FontManagerTest, ReplacingFontFaceDataTakesANewIdentityAndLeavesTheOldBytesIntact) {
  Registry registry;
  FontManager mgr(registry);

  css::FontFace face;
  face.familyName = RcString("TestFont");
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = std::make_shared<const std::vector<uint8_t>>(
      embedded::kPublicSansMediumOtf.begin(), embedded::kPublicSansMediumOtf.end());
  face.sources.push_back(std::move(source));
  mgr.addFontFace(face);

  const FontHandle original = mgr.findFont("TestFont");
  ASSERT_TRUE(static_cast<bool>(original));
  const size_t originalSize = mgr.fontData(original).size();
  ASSERT_GT(originalSize, 0u);

  // Same descriptors, different payload: the WOFF form of the same font decompresses to different
  // sfnt bytes than the OTF, so a stale reuse would be visible as an unchanged byte count.
  const std::vector<uint8_t> woff = readFile("donner/base/fonts/testdata/valid-001.woff");
  ASSERT_FALSE(woff.empty()) << "Could not read WOFF test file";
  css::FontFace replacement;
  replacement.familyName = RcString("TestFont");
  css::FontFaceSource replacementSource;
  replacementSource.kind = css::FontFaceSource::Kind::Data;
  replacementSource.payload =
      std::make_shared<const std::vector<uint8_t>>(woff.begin(), woff.end());
  replacement.sources.push_back(std::move(replacementSource));
  mgr.addFontFace(replacement);

  const FontHandle updated = mgr.findFont("TestFont");
  ASSERT_TRUE(static_cast<bool>(updated));
  EXPECT_NE(updated, original) << "New font data must resolve to a new identity.";
  EXPECT_EQ(mgr.fontData(original).size(), originalSize)
      << "The original handle's bytes were swapped underneath it.";
  EXPECT_NE(mgr.fontData(updated).size(), originalSize);
}

/// A declaration that differs only in a matching descriptor selects a different face, so it must
/// take its own entity rather than colliding with the family's existing one.
TEST(FontManagerTest, FontFacesDifferingOnlyByWeightTakeSeparateIdentities) {
  Registry registry;
  FontManager mgr(registry);

  auto sharedPayload = std::make_shared<const std::vector<uint8_t>>(
      embedded::kPublicSansMediumOtf.begin(), embedded::kPublicSansMediumOtf.end());

  css::FontFace regular;
  regular.familyName = RcString("TestFont");
  css::FontFaceSource regularSource;
  regularSource.kind = css::FontFaceSource::Kind::Data;
  regularSource.payload = sharedPayload;
  regular.sources.push_back(regularSource);
  mgr.addFontFace(regular);

  css::FontFace bold = regular;
  bold.fontWeight = 700;
  mgr.addFontFace(bold);

  const FontHandle regularHandle = mgr.findFont("TestFont", 400);
  const FontHandle boldHandle = mgr.findFont("TestFont", 700);
  ASSERT_TRUE(static_cast<bool>(regularHandle));
  ASSERT_TRUE(static_cast<bool>(boldHandle));
  EXPECT_NE(regularHandle, boldHandle);
}

/// A provider answers per requested face, so the memo that keeps provider fonts on one identity is
/// keyed on the whole request. The same request has to keep resolving to the same entity across
/// recomputes and across a dropped resolution cache, or every downstream cache keyed on the font
/// handle is thrown away on an unrelated document change. A DIFFERENT request is a different
/// question and may resolve elsewhere; what it must never do is receive another face's bytes.
TEST(FontManagerTest, ProviderRequestKeepsOneIdentityAcrossStylesAndCacheDrops) {
  Registry registry;
  FontManager mgr(registry);

  VariantFontProvider provider("ProviderFamily");
  mgr.setFontProvider(&provider);

  const FontHandle regular = mgr.findFont("ProviderFamily");
  ASSERT_TRUE(static_cast<bool>(regular));
  EXPECT_EQ(provider.loadCalls, 1);
  EXPECT_EQ(mgr.fontData(regular).size(), VariantFontProvider::SizeFor(FontFaceRequest{}));

  // The same request, repeated: one identity, no second load.
  EXPECT_EQ(mgr.findFont("ProviderFamily"), regular);
  EXPECT_EQ(provider.loadCalls, 1);

  // Different faces of the same family are different questions. They may take their own entities,
  // and each must carry the bytes the provider returned for that face rather than the first
  // face's, which is what a family-keyed memo would hand back.
  const FontFaceRequest boldRequest{.weight = 700};
  const FontHandle bold = mgr.findFont("ProviderFamily", boldRequest.weight);
  ASSERT_TRUE(static_cast<bool>(bold));
  EXPECT_EQ(mgr.fontData(bold).size(), VariantFontProvider::SizeFor(boldRequest))
      << "The bold request was served another face's bytes.";

  const FontFaceRequest italicRequest{.weight = 400, .style = FontStyle::Italic};
  const FontHandle italic =
      mgr.findFont("ProviderFamily", italicRequest.weight, static_cast<int>(italicRequest.style),
                   static_cast<int>(italicRequest.stretch));
  ASSERT_TRUE(static_cast<bool>(italic));
  EXPECT_EQ(mgr.fontData(italic).size(), VariantFontProvider::SizeFor(italicRequest))
      << "The italic request was served another face's bytes.";

  // Registering an unrelated declaration drops the resolution cache, because a new rule can
  // outrank an earlier answer. Every request's identity must survive that unchanged.
  css::FontFace unrelated;
  unrelated.familyName = RcString("SomeOtherFamily");
  mgr.addFontFace(unrelated);

  const int loadsBeforeCacheDrop = provider.loadCalls;
  EXPECT_EQ(mgr.findFont("ProviderFamily"), regular);
  EXPECT_EQ(mgr.findFont("ProviderFamily", boldRequest.weight), bold);
  EXPECT_EQ(
      mgr.findFont("ProviderFamily", italicRequest.weight, static_cast<int>(italicRequest.style),
                   static_cast<int>(italicRequest.stretch)),
      italic);
  EXPECT_EQ(provider.loadCalls, loadsBeforeCacheDrop)
      << "A dropped resolution cache re-loaded fonts the provider memo already held.";
}

/// Two providers may answer the same request with different bytes, so attaching a different one
/// abandons everything resolved through the old one, including the repeat of an identical query.
TEST(FontManagerTest, SwappingProviderAbandonsWhatThePreviousOneResolved) {
  Registry registry;
  FontManager mgr(registry);

  FakeFontProvider first({"ProviderFamily"});
  mgr.setFontProvider(&first);
  const FontHandle fromFirst = mgr.findFont("ProviderFamily");
  ASSERT_TRUE(static_cast<bool>(fromFirst));
  EXPECT_EQ(first.loadCalls, 1);

  FakeFontProvider second({"ProviderFamily"});
  mgr.setFontProvider(&second);

  const FontHandle fromSecond = mgr.findFont("ProviderFamily");
  ASSERT_TRUE(static_cast<bool>(fromSecond));
  EXPECT_NE(fromSecond, fromFirst) << "The new provider's family kept the old provider's font.";
  EXPECT_EQ(second.loadCalls, 1);

  // Re-attaching the same provider is not a change and must not throw away its resolutions.
  mgr.setFontProvider(&second);
  EXPECT_EQ(mgr.findFont("ProviderFamily"), fromSecond);
  EXPECT_EQ(second.loadCalls, 1);
}

/// A face that matches the family but cannot be loaded resolves to the fallback for now. That
/// answer must not stick: the failure can be a full font budget, which a later release undoes, and
/// a stuck answer would leave the document rendering in the wrong font forever.
TEST(FontManagerTest, FallbackForAnUnloadableFaceIsRetriedRatherThanCached) {
  Registry registry;
  const std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                                  embedded::kPublicSansMediumOtf.end());
  const size_t charge = RetainedCharge(data);
  // Room for two fonts: the fallback, and one filler that is released later to make room. The
  // fallback has to survive that release, or the cached answer would be dropped for being stale
  // rather than for being provisional, and this would not be testing the retry at all.
  FontManager mgr(registry, charge * 2);

  css::FontFace face;
  face.familyName = RcString("TestFont");
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = std::make_shared<const std::vector<uint8_t>>(data.begin(), data.end());
  face.sources.push_back(std::move(source));
  mgr.addFontFace(face);

  const FontHandle fallback = mgr.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(fallback));
  const FontHandle filler = mgr.loadFontData(data);
  ASSERT_TRUE(static_cast<bool>(filler));
  ASSERT_EQ(mgr.numLoadedFonts(), 2u) << "The budget is not full, so the load below would succeed.";

  EXPECT_EQ(mgr.findFont("TestFont"), fallback) << "The budget was full, so the face cannot load.";

  // Free the budget, leaving the fallback entity alive, and ask again. The face must now load
  // rather than replay the answer that stood in for it.
  registry.destroy(filler.entity());
  ASSERT_TRUE(registry.valid(fallback.entity()));
  const FontHandle loaded = mgr.findFont("TestFont");
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_NE(loaded, fallback);
  EXPECT_EQ(mgr.numLoadedFonts(), 2u);
}

/// Equally good faces resolve to the one declared last (CSS Fonts: a later `@font-face` wins).
/// The two declarations here carry different bytes, so the assertion names which one won rather
/// than only that the answer held still. Deduplication must not disturb the order either, so a
/// re-announced early declaration stays early rather than jumping ahead of later ones.
TEST(FontManagerTest, TiedFacesResolveToTheLastDeclared) {
  Registry registry;
  FontManager mgr(registry);

  const std::vector<uint8_t> earlyBytes(embedded::kPublicSansMediumOtf.begin(),
                                        embedded::kPublicSansMediumOtf.end());
  // Trailing padding after the last table leaves a valid sfnt with a distinguishable byte count,
  // so the loaded font says which declaration produced it.
  std::vector<uint8_t> lateBytes = earlyBytes;
  lateBytes.resize(lateBytes.size() + 17, 0);
  ASSERT_NE(earlyBytes.size(), lateBytes.size());

  // Two declarations for one family that score identically for a plain lookup: same family,
  // weight, style, and stretch, differing only in the bytes they carry.
  css::FontFace early;
  early.familyName = RcString("TestFont");
  css::FontFaceSource earlySource;
  earlySource.kind = css::FontFaceSource::Kind::Data;
  earlySource.payload =
      std::make_shared<const std::vector<uint8_t>>(earlyBytes.begin(), earlyBytes.end());
  early.sources.push_back(std::move(earlySource));

  css::FontFace late;
  late.familyName = RcString("TestFont");
  css::FontFaceSource lateSource;
  lateSource.kind = css::FontFaceSource::Kind::Data;
  lateSource.payload =
      std::make_shared<const std::vector<uint8_t>>(lateBytes.begin(), lateBytes.end());
  late.sources.push_back(std::move(lateSource));

  mgr.addFontFace(early);
  mgr.addFontFace(late);

  const FontHandle winner = mgr.findFont("TestFont");
  ASSERT_TRUE(static_cast<bool>(winner));
  EXPECT_EQ(mgr.fontData(winner).size(), lateBytes.size())
      << "The earlier declaration won the tie; CSS resolves a tie to the last one declared.";

  // Re-announce the whole set the way a style recompute does, repeatedly. Each round also brings
  // one genuinely new declaration, which drops the resolution cache and forces the tie to be
  // decided again rather than replayed.
  for (int recompute = 0; recompute < 5; ++recompute) {
    mgr.addFontFace(early);
    mgr.addFontFace(late);

    css::FontFace unrelated;
    unrelated.familyName = RcString("Unrelated" + std::to_string(recompute));
    mgr.addFontFace(unrelated);

    EXPECT_EQ(mgr.findFont("TestFont"), winner)
        << "The tie between two equally good faces moved at recompute " << recompute;
    EXPECT_EQ(mgr.fontData(mgr.findFont("TestFont")).size(), lateBytes.size())
        << "The tie stopped resolving to the last declaration at recompute " << recompute;
  }
}

/// The provider path needs the same retry the document-face path gets. A provider face whose bytes
/// cannot be stored right now (a full aggregate budget is enough on its own) resolves to the
/// fallback for the moment, and caching that would leave that request on the wrong font for the
/// rest of the manager's life even after the budget frees up.
TEST(FontManagerTest, FallbackForAnUnstorableProviderFaceIsRetriedRatherThanCached) {
  Registry registry;
  const std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                                  embedded::kPublicSansMediumOtf.end());
  const size_t charge = RetainedCharge(data);
  // Room for two fonts, as above: the fallback must outlive the release that makes room, or the
  // second lookup would miss the cache because its entry went stale rather than because the answer
  // was provisional.
  FontManager mgr(registry, charge * 2);

  FakeFontProvider provider({"ProviderFamily"});
  mgr.setFontProvider(&provider);

  const FontHandle fallback = mgr.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(fallback));
  const FontHandle filler = mgr.loadFontData(data);
  ASSERT_TRUE(static_cast<bool>(filler));
  ASSERT_EQ(mgr.numLoadedFonts(), 2u);

  EXPECT_EQ(mgr.findFont("ProviderFamily"), fallback)
      << "The budget was full, so the provider's bytes cannot be stored.";

  // Free the budget, leaving the fallback entity alive, and ask again. The provider's font must
  // now load rather than replay the answer that stood in for it.
  registry.destroy(filler.entity());
  ASSERT_TRUE(registry.valid(fallback.entity()));
  const FontHandle loaded = mgr.findFont("ProviderFamily");
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_NE(loaded, fallback);
  EXPECT_EQ(mgr.numLoadedFonts(), 2u);
  EXPECT_FALSE(mgr.fontData(loaded).empty());
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

// Regression: a family is a set of faces, so the provider needs the requested weight/style/stretch
// to pick one. Dropping them here is what makes `font-weight: bold` render as regular for every
// catalog-resolved family.
TEST(FontManagerTest, ProviderReceivesTheRequestedFace) {
  Registry registry;
  FontManager mgr(registry);
  FakeFontProvider provider({"ProviderFamily"});
  mgr.setFontProvider(&provider);

  (void)mgr.findFont("ProviderFamily", 700, static_cast<int>(FontStyle::Italic),
                     static_cast<int>(FontStretch::Condensed));

  EXPECT_THAT(provider.lastRequest,
              Eq(FontFaceRequest{
                  .weight = 700, .style = FontStyle::Italic, .stretch = FontStretch::Condensed}));
}

// Faces are cached per requested face, not per family, so a later bold lookup is not served the
// regular face out of the cache.
TEST(FontManagerTest, ProviderIsConsultedOncePerRequestedFace) {
  Registry registry;
  FontManager mgr(registry);
  FakeFontProvider provider({"ProviderFamily"});
  mgr.setFontProvider(&provider);

  (void)mgr.findFont("ProviderFamily", 400);
  (void)mgr.findFont("ProviderFamily", 400);
  EXPECT_EQ(provider.loadCalls, 1);

  (void)mgr.findFont("ProviderFamily", 700);
  EXPECT_EQ(provider.loadCalls, 2);
  EXPECT_THAT(provider.lastRequest.weight, Eq(700));
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
