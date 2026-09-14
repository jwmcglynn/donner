#include "donner/svg/resources/CatalogEncodedFontStore.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/resources/EmbeddedFontProvider.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::svg {
namespace {

using testing::ElementsAre;
using testing::Field;
using testing::IsEmpty;
using testing::Not;

const CatalogFontAsset& InterAsset() {
  const auto assets = CatalogFontAssets();
  return *std::find_if(assets.begin(), assets.end(),
                       [](const auto& asset) { return asset.family == "Inter"; });
}

std::vector<uint8_t> InterBytes() {
  return EmbeddedFontProvider().loadFamilyData("Inter", {});
}

uint64_t QueueAndStart(CatalogEncodedFontStore& store) {
  EXPECT_EQ(store.queue(InterAsset().contentId), true);
  const uint64_t token = store.beginFetch(InterAsset().contentId);
  EXPECT_NE(token, 0u);
  return token;
}

TEST(CatalogEncodedFontStoreTest, MetadataAndProviderMissDoNotQueueARequest) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  EmbeddedFontProvider provider(store);
  EXPECT_EQ(provider.families().size(), 12u);
  EXPECT_EQ(provider.hasFamily("Inter"), true);
  EXPECT_THAT(provider.loadFamilyData("Inter", {}), IsEmpty());
  EXPECT_EQ(store->availability(InterAsset().contentId).state, FontAssetState::Absent);
  EXPECT_EQ(store->wakeRevision(), 0u);
  EXPECT_EQ(store->beginFetch(InterAsset().contentId), 0u);
  EXPECT_EQ(store->retainedBytes(), 0u);
}

TEST(CatalogEncodedFontStoreTest, RejectsUnknownStaleDuplicateAndOversizedPublication) {
  CatalogEncodedFontStore store;
  const auto& id = InterAsset().contentId;
  EXPECT_EQ(store.queue("not-in-compiled-manifest"), false);
  const uint64_t token = QueueAndStart(store);
  EXPECT_EQ(store.publishVerified("not-in-compiled-manifest", token, InterBytes()), false);
  EXPECT_EQ(store.publishVerified(id, token + 1, InterBytes()), false);
  auto truncated = InterBytes();
  truncated.pop_back();
  EXPECT_EQ(store.publishVerified(id, token, std::move(truncated)), false);
  auto oversizedCapacity = InterBytes();
  oversizedCapacity.reserve(CatalogEncodedFontStore::kMaximumAssetBytes + 1);
  EXPECT_EQ(store.publishVerified(id, token, std::move(oversizedCapacity)), false);
  auto wrongHeader = InterBytes();
  wrongHeader[16] = 0xff;
  EXPECT_EQ(store.publishVerified(id, token, std::move(wrongHeader)), false);
  EXPECT_EQ(store.retainedBytes(), 0u);
  EXPECT_EQ(store.publishVerified(id, token, InterBytes()), true);
  EXPECT_EQ(store.publishVerified(id, token, InterBytes()), false);
  EXPECT_EQ(store.fail(id, token), false);
}

TEST(CatalogEncodedFontStoreTest, AtMostTwoTransportRequestsAreActive) {
  CatalogEncodedFontStore store;
  const auto assets = CatalogFontAssets();
  for (size_t i = 0; i < 3; ++i) EXPECT_EQ(store.queue(assets[i].contentId), true);
  const auto first = store.beginFetch(assets[0].contentId);
  EXPECT_NE(first, 0u);
  EXPECT_NE(store.beginFetch(assets[1].contentId), 0u);
  EXPECT_EQ(store.beginFetch(assets[2].contentId), 0u);
  EXPECT_EQ(store.fail(assets[0].contentId, first), true);
  EXPECT_NE(store.beginFetch(assets[2].contentId), 0u);
}

TEST(CatalogEncodedFontStoreTest, FailureRequiresExplicitRetryAndRejectsOldAttempt) {
  CatalogEncodedFontStore store;
  const auto& id = InterAsset().contentId;
  const uint64_t first = QueueAndStart(store);
  EXPECT_EQ(store.fail(id, first), true);
  EXPECT_EQ(store.availability(id).state, FontAssetState::Failed);
  EXPECT_EQ(store.queue(id), false);
  EXPECT_EQ(store.retry(id), true);
  const uint64_t second = store.beginFetch(id);
  EXPECT_GT(second, first);
  EXPECT_EQ(store.publishVerified(id, first, InterBytes()), false);
  EXPECT_EQ(store.publishVerified(id, second, InterBytes()), true);
}

TEST(CatalogEncodedFontStoreTest, AdoptionWakesTasksCreatedAfterPublication) {
  CatalogEncodedFontStore store;
  const auto& id = InterAsset().contentId;
  ASSERT_EQ(store.publishVerified(id, QueueAndStart(store), InterBytes()), true);
  const auto stagedWake = store.wakeRevision();
  ASSERT_EQ(store.availability(id).state, FontAssetState::Fetching);
  std::vector<FontAssetState> observed;
  store.setWakeCallback([&] { observed.push_back(store.availability(id).state); });

  ASSERT_EQ(store.adoptReadyAssets(), true);
  EXPECT_GT(store.wakeRevision(), stagedWake);
  EXPECT_THAT(observed, ElementsAre(FontAssetState::Ready));
  const auto adoptedWake = store.wakeRevision();
  EXPECT_EQ(store.adoptReadyAssets(), false);
  EXPECT_EQ(store.wakeRevision(), adoptedWake);
  EXPECT_THAT(observed, ElementsAre(FontAssetState::Ready));
  store.setWakeCallback({});
}

TEST(CatalogEncodedFontStoreTest, EvictionCannotInvalidateALeaseAndRefillPreservesIdentity) {
  CatalogEncodedFontStore store;
  const auto& id = InterAsset().contentId;
  EXPECT_EQ(store.publishVerified(id, QueueAndStart(store), InterBytes()), true);
  EXPECT_EQ(store.adoptReadyAssets(), true);
  const auto first = store.availability(id);
  auto lease = store.encodedBytes(id);
  ASSERT_THAT(lease, Not(testing::IsNull()));
  EXPECT_EQ(store.retainedBytes(), lease->capacity());
  EXPECT_EQ(store.evict(id), false);
  lease.reset();
  EXPECT_EQ(store.evict(id), true);
  EXPECT_EQ(store.retainedBytes(), 0u);
  EXPECT_EQ(store.publishVerified(id, QueueAndStart(store), InterBytes()), true);
  EXPECT_EQ(store.availability(id).contentGeneration, first.contentGeneration);
}

TEST(CatalogEncodedFontStoreTest, AdmissionReleaseWakesWithoutARequestOrManagerLifetime) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  auto admitted = store->tryAcquireDecode(InterAsset().contentId);
  EXPECT_EQ(admitted.state, FontFaceLoadState::Resolving);
  EXPECT_EQ(store->tryAcquireDecode(InterAsset().contentId).state,
            FontFaceLoadState::WaitingForAdmission);
  EXPECT_EQ(store->tryAcquireDecode(InterAsset().contentId).waitReason,
            FontFaceWaitReason::SharedDecodeSlot);
  size_t wakes = 0;
  store->setWakeCallback([&] {
    ++wakes;
    // Reentry proves callbacks are outside the store mutex.
    EXPECT_GT(store->wakeRevision(), 0u);
  });
  admitted.reservation.reset();
  EXPECT_EQ(wakes, 1u);
  EXPECT_EQ(store->availability(InterAsset().contentId).state, FontAssetState::Absent);
  EXPECT_EQ(store->tryAcquireDecode(InterAsset().contentId).state, FontFaceLoadState::Resolving);
  store->setWakeCallback({});
}

#ifdef DONNER_TEXT_WOFF2_ENABLED
class AlteredCatalogProvider final : public FontFamilyProvider {
public:
  std::vector<FontFamilyInfo> families() const override { return native_.families(); }
  bool hasFamily(std::string_view family) const override { return native_.hasFamily(family); }
  FontFaceAvailability availability(std::string_view family,
                                    const FontFaceRequest& request) const override {
    auto result = native_.availability(family, request);
    result.contentGeneration = generation;
    return result;
  }
  std::vector<uint8_t> loadFamilyData(std::string_view family,
                                      const FontFaceRequest& request) const override {
    ++loads;
    auto bytes = native_.loadFamilyData(family, request);
    if (corrupt) bytes[8] = 0xff;  // Invalid declared input length, before Brotli allocation.
    return bytes;
  }
  bool corrupt = true;
  uint64_t generation = 1;
  mutable size_t loads = 0;

private:
  EmbeddedFontProvider native_;
};

TEST(CatalogEncodedFontStoreTest, TerminalDecodeFailureIsMemoizedPerContentGeneration) {
  AlteredCatalogProvider provider;
  Registry registry;
  FontManager manager(registry);
  manager.setFontProvider(&provider);
  const auto fallback = manager.findFont("Inter");
  EXPECT_EQ(fallback, manager.fallbackFont());
  EXPECT_EQ(manager.findFont("Inter"), fallback);
  EXPECT_EQ(manager.refreshPendingFonts(), false);
  EXPECT_EQ(provider.loads, 1u);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 1u);
  EXPECT_THAT(manager.faceDependencies(),
              ElementsAre(Field(&FontFaceDependency::state, FontFaceLoadState::Failed)));
  provider.corrupt = false;
  ++provider.generation;
  EXPECT_EQ(manager.refreshPendingFonts(), true);
  EXPECT_NE(manager.findFont("Inter"), fallback);
  EXPECT_EQ(provider.loads, 2u);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 2u);
}

TEST(CatalogEncodedFontStoreTest, ConsumerBudgetPressureDefersWithoutPoisoningTheAsset) {
  EmbeddedFontProvider provider;
  Registry registry;
  FontManager manager(registry, FontManager::kDefaultMaximumLoadedFontBytes, 1);
  manager.setFontProvider(&provider);
  const auto occupied = manager.fallbackFont();
  EXPECT_EQ(manager.findFont("Inter"), occupied);
  EXPECT_THAT(
      manager.faceDependencies(),
      ElementsAre(Field(&FontFaceDependency::state, FontFaceLoadState::WaitingForAdmission)));
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 0u);
  EXPECT_EQ(provider.availability("Inter", {}).state, FontAssetState::Ready);
  ASSERT_THAT(manager.faceDependencies(), testing::SizeIs(1));
  EXPECT_EQ(manager.faceDependencies()[0].waitReason, FontFaceWaitReason::RetainedBudget);
  EXPECT_EQ(manager.needsResourceRefresh(), false);
  const auto revision = manager.faceDependencies()[0].consumerBudgetRevision;
  auto otherConsumer =
      provider.encodedStore()->tryAcquireDecode(provider.availability("Inter", {}).contentId);
  otherConsumer.reservation.reset();
  EXPECT_EQ(manager.refreshPendingFonts(), false);
  EXPECT_EQ(manager.faceDependencies()[0].consumerBudgetRevision, revision);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 0u);
  registry.destroy(occupied.entity());
  EXPECT_EQ(manager.needsResourceRefresh(), true);
  EXPECT_EQ(manager.refreshPendingFonts(), true);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 1u);
  EXPECT_EQ(manager.isValidatedFont(manager.findFont("Inter")), true);
}

TEST(CatalogEncodedFontStoreTest, FamilyMetadataPressureDefersAndRecoversAfterRelease) {
  Registry measurementRegistry;
  FontManager measurement(measurementRegistry);
  const auto measured = measurement.loadFontData(InterBytes(), FontDataTrust::Trusted);
  ASSERT_EQ(measurement.isValidatedFont(measured), true);
  const auto data = measurement.fontData(measured);
  const auto sfnt = fonts::SfntFont::Validate(data);
  ASSERT_EQ(sfnt.has_value(), true);
  const size_t fullCharge = measurement.loadedFontBytes();
  const size_t dataAndIndexCharge = data.size() + sfnt->retainedBytes();
  ASSERT_GT(fullCharge, dataAndIndexCharge);
  ASSERT_GE(fullCharge - 1, dataAndIndexCharge);

  EmbeddedFontProvider provider;
  Registry registry;
  FontManager manager(registry, 2 * fullCharge - 1);
  const auto occupied = manager.loadFontData(InterBytes(), FontDataTrust::Trusted);
  ASSERT_EQ(manager.isValidatedFont(occupied), true);
  ASSERT_EQ(manager.loadedFontBytes(), fullCharge);
  manager.setFontProvider(&provider);
  const auto fallback = manager.findFont("Inter");
  EXPECT_EQ(fallback, manager.fallbackFont());
  EXPECT_THAT(manager.faceDependencies(),
              ElementsAre(testing::AllOf(
                  Field(&FontFaceDependency::state, FontFaceLoadState::WaitingForAdmission),
                  Field(&FontFaceDependency::waitReason, FontFaceWaitReason::RetainedBudget))));
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 2u);
  EXPECT_EQ(manager.findFont("Inter"), fallback);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 2u);

  registry.destroy(occupied.entity());
  EXPECT_EQ(manager.needsResourceRefresh(), true);
  EXPECT_EQ(manager.refreshPendingFonts(), true);
  const auto loaded = manager.findFont("Inter");
  EXPECT_NE(loaded, fallback);
  EXPECT_EQ(manager.isValidatedFont(loaded), true);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 3u);
  EXPECT_THAT(manager.faceDependencies(),
              ElementsAre(Field(&FontFaceDependency::state, FontFaceLoadState::Loaded)));
}

TEST(CatalogEncodedFontStoreTest, ReadyAssetDeferredDecodeWakesWithoutAnotherFetch) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  EmbeddedFontProvider provider(store);
  const auto& id = InterAsset().contentId;
  auto admitted = store->tryAcquireDecode(id);
  const uint64_t request = QueueAndStart(*store);
  EXPECT_EQ(store->publishVerified(id, request, InterBytes()), true);
  EXPECT_EQ(store->adoptReadyAssets(), true);
  const auto assetGeneration = store->availability(id).contentGeneration;
  std::vector<FontFaceDependency> stablePreviewDependencies;
  {
    Registry temporaryPreview;
    FontManager manager(temporaryPreview);
    manager.setFontProvider(&provider);
    auto capture = manager.captureDependencies(stablePreviewDependencies);
    EXPECT_EQ(manager.findFont("Inter"), manager.fallbackFont());
    EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 0u);
  }
  EXPECT_THAT(
      stablePreviewDependencies,
      ElementsAre(Field(&FontFaceDependency::state, FontFaceLoadState::WaitingForAdmission)));
  size_t coordinatorWakes = 0;
  store->setWakeCallback([&] { ++coordinatorWakes; });
  admitted.reservation.reset();
  EXPECT_EQ(coordinatorWakes, 1u);
  {
    Registry retryPreview;
    FontManager manager(retryPreview);
    manager.setFontProvider(&provider);
    const uint64_t revision = manager.fontResourceRevision();
    const auto resolved = manager.findFont("Inter");
    EXPECT_NE(resolved, manager.fallbackFont());
    EXPECT_GT(manager.fontResourceRevision(), revision);
    EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 1u);
    EXPECT_EQ(manager.findFont("Inter"), resolved);
    EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 1u);
    EXPECT_EQ(store->availability(id).contentGeneration, assetGeneration);
    EXPECT_EQ(store->beginFetch(id), 0u);
  }
  store->setWakeCallback({});
}

TEST(CatalogEncodedFontStoreTest, ArrivalDuringRenderWaitsForExplicitResourceAdoption) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  EmbeddedFontProvider provider(store);
  Registry registry;
  FontManager manager(registry);
  manager.setFontProvider(&provider);
  const auto fallback = manager.findFont("Inter");
  const auto revision = manager.fontResourceRevision();
  EXPECT_EQ(store->publishVerified(InterAsset().contentId, QueueAndStart(*store), InterBytes()),
            true);
  EXPECT_EQ(store->availability(InterAsset().contentId).state, FontAssetState::Fetching);
  EXPECT_THAT(store->encodedBytes(InterAsset().contentId), testing::IsNull());
  EXPECT_EQ(manager.findFont("Inter"), fallback);
  // A different face in the same running frame cannot observe the staged bytes either.
  EXPECT_EQ(manager.findFont("Inter", 700), fallback);
  EXPECT_EQ(manager.fontResourceRevision(), revision);
  EXPECT_EQ(store->adoptReadyAssets(), true);
  // Adoption is followed by explicit consumer refresh, never an opportunistic mid-frame load.
  EXPECT_EQ(manager.findFont("Inter"), fallback);
  EXPECT_EQ(manager.refreshPendingFonts(), true);
  EXPECT_NE(manager.findFont("Inter"), fallback);
  EXPECT_NE(manager.findFont("Inter", 700), fallback);
  EXPECT_GT(manager.fontResourceRevision(), revision);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 2u);
}

TEST(CatalogEncodedFontStoreTest, PendingManagerRefreshesAndCapturesCachedDependencies) {
  auto store = std::make_shared<CatalogEncodedFontStore>();
  EmbeddedFontProvider provider(store);
  Registry registry;
  FontManager manager(registry);
  manager.setFontProvider(&provider);
  const auto fallback = manager.findFont("Inter");
  EXPECT_EQ(fallback, manager.fallbackFont());
  EXPECT_EQ(store->publishVerified(InterAsset().contentId, QueueAndStart(*store), InterBytes()),
            true);
  EXPECT_EQ(store->adoptReadyAssets(), true);
  const auto revision = manager.fontResourceRevision();
  EXPECT_EQ(manager.refreshPendingFonts(), true);
  EXPECT_GT(manager.fontResourceRevision(), revision);
  const auto resolved = manager.findFont("Inter");
  EXPECT_NE(resolved, fallback);
  std::vector<FontFaceDependency> anotherRoot;
  {
    auto capture = manager.captureDependencies(anotherRoot);
    EXPECT_EQ(manager.findFont("Inter"), resolved);
    EXPECT_EQ(manager.findFont("Inter"), resolved);
  }
  EXPECT_THAT(anotherRoot,
              ElementsAre(Field(&FontFaceDependency::state, FontFaceLoadState::Loaded)));
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 1u);
  EXPECT_EQ(store->evict(InterAsset().contentId), true);
  EXPECT_EQ(manager.findFont("Inter"), resolved);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 1u);
}

class NativeCatalogDecoderTest : public testing::TestWithParam<const char*> {};

TEST_P(NativeCatalogDecoderTest, EmbeddedWoff2DecodesAndReusesTheFullFace) {
  EmbeddedFontProvider provider;
  Registry registry;
  FontManager manager(registry);
  manager.setFontProvider(&provider);
  const std::string family = GetParam();
  const auto metadata = provider.availability(family, {});
  const auto regular = manager.findFont(family);
  ASSERT_NE(regular, manager.fallbackFont()) << family;
  EXPECT_EQ(manager.fontData(regular).size(), metadata.decodedBytes);
  EXPECT_EQ(manager.isValidatedFont(regular), true);
  EXPECT_EQ(manager.findFont(family), regular);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 1u);
  const auto bold = manager.findFont(family, 700);
  EXPECT_NE(bold, regular);
  EXPECT_EQ(manager.findFont(family, 700), bold);
  EXPECT_EQ(manager.compressedFontDecompressionAttempts(), 2u);
  EXPECT_EQ(provider.encodedStore()->retainedBytes(), 0u);
}

INSTANTIATE_TEST_SUITE_P(AllTwelveFamilies, NativeCatalogDecoderTest,
                         testing::Values("Bebas Neue", "Bitter", "Inter", "JetBrains Mono", "Lato",
                                         "Lora", "Montserrat", "Open Sans", "Oswald", "Pacifico",
                                         "Playfair Display", "Roboto Mono"));
#endif

}  // namespace
}  // namespace donner::svg
