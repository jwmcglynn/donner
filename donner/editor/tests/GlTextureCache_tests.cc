#include "donner/editor/GlTextureCache.h"

#include <array>
#include <memory>
#include <utility>

#include "donner/svg/renderer/RendererInterface.h"
#ifdef DONNER_EDITOR_WGPU
#include "donner/editor/gui/ImGuiRuntimeRenderer.h"
#include "donner/editor/gui/UiTextureRegistry.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#endif
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

class PayloadTextureSnapshot final : public svg::RendererTextureSnapshot {
public:
  explicit PayloadTextureSnapshot(const Vector2i& dimensions) : dimensions_(dimensions) {}

  [[nodiscard]] Vector2i dimensions() const override { return dimensions_; }
  [[nodiscard]] svg::AlphaType alphaType() const override { return svg::AlphaType::Premultiplied; }

private:
  Vector2i dimensions_;
};

RenderResult::CompositedTile MetadataTile(RenderResult::CompositedTile::Kind kind,
                                          std::uint64_t generation, const Vector2i& textureDimsPx,
                                          const Vector2i& rasterCanvasSize) {
  RenderResult::CompositedTile tile;
  tile.kind = kind;
  tile.generation = generation;
  tile.bitmapDimsPx = textureDimsPx;
  tile.rasterCanvasSize = rasterCanvasSize;
  return tile;
}

[[maybe_unused]] EditorRasterViewport RasterViewportForTest(bool viewportBounded) {
  EditorRasterViewport viewport;
  viewport.documentRect = Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0);
  viewport.outputSizePx = viewportBounded ? Vector2i(20, 20) : Vector2i(100, 100);
  viewport.semanticCanvasSizePx = Vector2i(100, 100);
  viewport.outputFromDocument = Transform2d();
  viewport.viewportBounded = viewportBounded;
  return viewport;
}

TEST(GlTextureCacheTest, MetadataOnlyReuseRequiresFullTextureIdentity) {
  const RenderResult::CompositedTile tile = MetadataTile(RenderResult::CompositedTile::Kind::Layer,
                                                         12, Vector2i(32, 48), Vector2i(200, 120));
  const CompositedTileTextureIdentity cachedIdentity = TextureIdentityForCompositedTile(tile);

  EXPECT_TRUE(TextureIdentityMatchesCompositedTile(cachedIdentity, tile));

  RenderResult::CompositedTile changedKind = tile;
  changedKind.kind = RenderResult::CompositedTile::Kind::Segment;
  EXPECT_FALSE(TextureIdentityMatchesCompositedTile(cachedIdentity, changedKind));

  RenderResult::CompositedTile changedGeneration = tile;
  changedGeneration.generation += 1;
  EXPECT_FALSE(TextureIdentityMatchesCompositedTile(cachedIdentity, changedGeneration));

  RenderResult::CompositedTile changedTextureDims = tile;
  changedTextureDims.bitmapDimsPx = Vector2i(33, 48);
  EXPECT_FALSE(TextureIdentityMatchesCompositedTile(cachedIdentity, changedTextureDims));

  RenderResult::CompositedTile changedRasterCanvas = tile;
  changedRasterCanvas.rasterCanvasSize = Vector2i(220, 120);
  EXPECT_FALSE(TextureIdentityMatchesCompositedTile(cachedIdentity, changedRasterCanvas));
}

TEST(GlTextureCacheTest, BitmapPayloadBytesUsesRowStride) {
  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(8, 9);
  bitmap.rowBytes = 40u;
  bitmap.pixels.resize(bitmap.rowBytes * static_cast<std::size_t>(bitmap.dimensions.y));

  EXPECT_EQ(BitmapPayloadBytes(bitmap), 360u);
}

TEST(GlTextureCacheTest, PowerOfTwoTextureDimensionsBucketPayloadDimensions) {
  EXPECT_EQ(PowerOfTwoTextureDimensionsForPayload(Vector2i::Zero()), Vector2i::Zero());
  EXPECT_EQ(PowerOfTwoTextureDimensionsForPayload(Vector2i(12, 0)), Vector2i::Zero());
  EXPECT_EQ(PowerOfTwoTextureDimensionsForPayload(Vector2i(1, 1)), Vector2i(1, 1));
  EXPECT_EQ(PowerOfTwoTextureDimensionsForPayload(Vector2i(3, 5)), Vector2i(4, 8));
  EXPECT_EQ(PowerOfTwoTextureDimensionsForPayload(Vector2i(1024, 1025)), Vector2i(1024, 2048));
}

TEST(GlTextureCacheTest, TextureUvBottomRightSamplesOnlyPayloadRegion) {
  const Vector2d uv = TextureUvBottomRightForPayload(Vector2i(3, 5), Vector2i(4, 8));

  EXPECT_DOUBLE_EQ(uv.x, 0.75);
  EXPECT_DOUBLE_EQ(uv.y, 0.625);
}

TEST(GlTextureCacheTest, CompositedUploadCostCountsPayloadBytesAndTileCoverage) {
  RenderResult::CompositedPreview preview;

  RenderResult::CompositedTile bitmapTile = MetadataTile(
      RenderResult::CompositedTile::Kind::Immediate, 1, Vector2i(1, 1), Vector2i(100, 100));
  bitmapTile.id = "seg:0";
  bitmapTile.bitmap.dimensions = Vector2i(8, 9);
  bitmapTile.bitmap.rowBytes = 40u;
  bitmapTile.bitmap.pixels.resize(bitmapTile.bitmap.rowBytes *
                                  static_cast<std::size_t>(bitmapTile.bitmap.dimensions.y));
  preview.tiles.push_back(std::move(bitmapTile));

  RenderResult::CompositedTile textureTile = MetadataTile(RenderResult::CompositedTile::Kind::Layer,
                                                          2, Vector2i(1, 1), Vector2i(100, 100));
  textureTile.id = "layer:1";
  textureTile.textureSnapshot = std::make_shared<PayloadTextureSnapshot>(Vector2i(3, 5));
  preview.tiles.push_back(std::move(textureTile));

  RenderResult::CompositedTile metadataTile = MetadataTile(
      RenderResult::CompositedTile::Kind::Segment, 3, Vector2i(2, 7), Vector2i(100, 100));
  metadataTile.id = "seg:1";
  preview.tiles.push_back(std::move(metadataTile));

  const FrameCostBreakdown::CompositedUpload cost = CostForCompositedPreviewUpload(preview);

  EXPECT_EQ(cost.tileCount, 3);
  EXPECT_EQ(cost.payloadTileCount, 2);
  EXPECT_EQ(cost.bitmapPayloadTileCount, 1);
  EXPECT_EQ(cost.texturePayloadTileCount, 1);
  EXPECT_EQ(cost.metadataOnlyTileCount, 1);
  EXPECT_EQ(cost.immediateTileCount, 1);
  EXPECT_EQ(cost.payloadBytes, 360u + 3u * 5u * 4u);
  EXPECT_EQ(cost.payloadPixelArea, 8u * 9u + 3u * 5u);
  EXPECT_EQ(cost.tilePixelArea, 8u * 9u + 3u * 5u + 2u * 7u);
}

TEST(GlTextureCacheTest, PayloadIdentityUsesActualBitmapDimensions) {
  RenderResult::CompositedTile tile = MetadataTile(RenderResult::CompositedTile::Kind::Segment, 4,
                                                   Vector2i(1, 1), Vector2i(64, 64));
  tile.bitmap.dimensions = Vector2i(8, 9);
  tile.bitmap.rowBytes = 8u * 4u;
  tile.bitmap.pixels.resize(8u * 9u * 4u);

  const CompositedTileTextureIdentity identity = TextureIdentityForCompositedTile(tile);

  EXPECT_EQ(identity.textureDimsPx, Vector2i(8, 9));
  EXPECT_EQ(identity.rasterCanvasSize, Vector2i(64, 64));
  EXPECT_EQ(PowerOfTwoTextureDimensionsForPayload(identity.textureDimsPx), Vector2i(8, 16));
}

TEST(GlTextureCacheTest, PayloadSizeHelpersHandleEmptyAndInvalidDimensions) {
  svg::RendererBitmap bitmap;
  EXPECT_EQ(BitmapPayloadBytes(bitmap), 0u);

  bitmap.dimensions = Vector2i(3, 2);
  bitmap.rowBytes = 0u;
  bitmap.pixels.resize(24u);
  EXPECT_EQ(BitmapPayloadBytes(bitmap), 0u);

  bitmap.rowBytes = 12u;
  bitmap.dimensions = Vector2i(3, 0);
  EXPECT_EQ(BitmapPayloadBytes(bitmap), 0u);

  EXPECT_EQ(TexturePayloadBytes(Vector2i(-1, 10)), 0u);
  EXPECT_EQ(TexturePayloadBytes(Vector2i(3, 4)), 48u);
  EXPECT_EQ(PowerOfTwoTextureDimensionsForPayload(Vector2i((1 << 30) + 1, 3)),
            Vector2i((1 << 30) + 1, 4));

  EXPECT_EQ(TextureUvBottomRightForPayload(Vector2i(0, 1), Vector2i(2, 2)), Vector2d(1.0, 1.0));
  EXPECT_EQ(TextureUvBottomRightForPayload(Vector2i(3, 1), Vector2i(4, 0)), Vector2d(1.0, 1.0));
}

TEST(GlTextureCacheTest, MetadataOnlyCompositedUploadTracksMissesAndViewportDiagnostics) {
  RenderResult::CompositedPreview preview;
  RenderResult::CompositedTile tile = MetadataTile(RenderResult::CompositedTile::Kind::Segment, 7,
                                                   Vector2i(8, 9), Vector2i(20, 20));
  tile.id = "seg:0";
  tile.canvasOffsetDoc = Vector2d(1.0, 2.0);
  tile.bitmapDimsDoc = Vector2d(3.0, 4.0);
  tile.dragTranslationDoc = Vector2d(5.0, 6.0);
  preview.tiles.push_back(std::move(tile));

  GlTextureCache cache;
  cache.uploadComposited(preview, RasterViewportForTest(/*viewportBounded=*/true));

  ASSERT_EQ(cache.tiles().size(), 1u);
  EXPECT_EQ(cache.tiles().front().id, "seg:0");
  EXPECT_EQ(cache.tiles().front().texture, 0u);
  EXPECT_TRUE(cache.tiles().front().metadataOnly);
  EXPECT_EQ(cache.tiles().front().bitmapDimsPx, Vector2i(8, 9));
  EXPECT_TRUE(cache.overviewTiles().empty());
  EXPECT_TRUE(cache.activeTilesViewportBounded());
  EXPECT_EQ(cache.metadataOnlyMissCount(), 1);
  EXPECT_EQ(cache.duplicateLiveTextureCount(), 0);
  EXPECT_EQ(cache.lastCompositedUploadCost().tileCount, 1);
  EXPECT_EQ(cache.lastCompositedUploadCost().metadataOnlyTileCount, 1);
  EXPECT_EQ(cache.lastCompositedUploadCost().tilePixelArea, 8u * 9u);
  EXPECT_EQ(cache.lastCompositedUploadCost().payloadBytes, 0u);

  const PresentationCoverageDiagnostics coverage = cache.coverageDiagnostics();
  EXPECT_TRUE(coverage.activeTilesViewportBounded);
  EXPECT_FALSE(coverage.overviewInfillAvailable);
  EXPECT_EQ(coverage.activeRasterDocumentRect, Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));
  EXPECT_EQ(coverage.overviewRasterDocumentRect, Box2d());
  EXPECT_EQ(coverage.activeOutputSizePx, Vector2i(20, 20));
  EXPECT_EQ(coverage.overviewOutputSizePx, Vector2i::Zero());
}

TEST(GlTextureCacheTest, MetadataOnlyOverviewUploadTracksMissAndRetainsViewport) {
  RenderResult::CompositedPreview preview;
  RenderResult::CompositedTile tile = MetadataTile(RenderResult::CompositedTile::Kind::Layer, 8,
                                                   Vector2i(6, 7), Vector2i(100, 100));
  tile.id = "layer:0";
  preview.tiles.push_back(std::move(tile));

  GlTextureCache cache;
  cache.uploadCompositedOverview(preview, RasterViewportForTest(/*viewportBounded=*/false));

  EXPECT_TRUE(cache.tiles().empty());
  EXPECT_TRUE(cache.overviewTiles().empty());
  EXPECT_FALSE(cache.activeTilesViewportBounded());
  EXPECT_EQ(cache.metadataOnlyMissCount(), 1);
  EXPECT_EQ(cache.lastCompositedUploadCost().tileCount, 1);
  EXPECT_EQ(cache.lastCompositedUploadCost().metadataOnlyTileCount, 1);
  EXPECT_EQ(cache.lastCompositedUploadCost().tilePixelArea, 6u * 7u);

  const PresentationCoverageDiagnostics coverage = cache.coverageDiagnostics();
  EXPECT_FALSE(coverage.activeTilesViewportBounded);
  EXPECT_FALSE(coverage.overviewInfillAvailable);
  EXPECT_EQ(coverage.activeRasterDocumentRect, Box2d());
  EXPECT_EQ(coverage.overviewRasterDocumentRect, Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));
  EXPECT_EQ(coverage.activeOutputSizePx, Vector2i::Zero());
  EXPECT_EQ(coverage.overviewOutputSizePx, Vector2i(100, 100));
}

TEST(GlTextureCacheTest, ResetCompositedClearsMetadataBookkeepingAndCost) {
  RenderResult::CompositedPreview preview;
  RenderResult::CompositedTile tile = MetadataTile(RenderResult::CompositedTile::Kind::Immediate, 9,
                                                   Vector2i(4, 5), Vector2i(20, 20));
  tile.id = "immediate:0";
  preview.tiles.push_back(std::move(tile));

  GlTextureCache cache;
  cache.uploadComposited(preview, RasterViewportForTest(/*viewportBounded=*/true));
  ASSERT_TRUE(cache.activeTilesViewportBounded());
  ASSERT_EQ(cache.metadataOnlyMissCount(), 1);
  ASSERT_EQ(cache.lastCompositedUploadCost().tileCount, 1);

  cache.resetComposited();

  EXPECT_TRUE(cache.tiles().empty());
  EXPECT_TRUE(cache.overviewTiles().empty());
  EXPECT_FALSE(cache.activeTilesViewportBounded());
  EXPECT_EQ(cache.metadataOnlyMissCount(), 0);
  EXPECT_EQ(cache.duplicateLiveTextureCount(), 0);
  EXPECT_EQ(cache.lastCompositedUploadCost().tileCount, 0);
  EXPECT_EQ(cache.lastCompositedUploadCost().tilePixelArea, 0u);

  const PresentationCoverageDiagnostics coverage = cache.coverageDiagnostics();
  EXPECT_FALSE(coverage.activeTilesViewportBounded);
  EXPECT_FALSE(coverage.overviewInfillAvailable);
  EXPECT_EQ(coverage.activeRasterDocumentRect, Box2d());
  EXPECT_EQ(coverage.overviewRasterDocumentRect, Box2d());
  EXPECT_EQ(coverage.activeOutputSizePx, Vector2i::Zero());
  EXPECT_EQ(coverage.overviewOutputSizePx, Vector2i::Zero());
}

TEST(GlTextureCacheTest, DocumentCompositeBytesParticipateInTrackedResourceTotal) {
  GlTextureCache cache;
  cache.setDocumentCompositeBytes(8192u);

  const PresentationResourceStats stats = cache.presentationResourceStats();
  EXPECT_EQ(stats.documentCompositeBytes, 8192u);
  EXPECT_EQ(stats.totalTrackedBytes, 8192u);
  EXPECT_EQ(stats.peakTrackedBytes, 8192u);
}

TEST(GlTextureCacheTest, EmptyThumbnailAndRetainOnEmptyDoNotAllocate) {
  GlTextureCache cache;
  svg::RendererBitmap bitmap;

  const GlTextureCache::ThumbnailTextureView view = cache.uploadThumbnail(42u, bitmap);

  EXPECT_EQ(view.texture, 0);
  EXPECT_EQ(view.uvBottomRight, Vector2d(1.0, 1.0));
  EXPECT_EQ(cache.thumbnailTextureCount(), 0u);

  cache.retainThumbnailsOnly({42u});
  EXPECT_EQ(cache.thumbnailTextureCount(), 0u);
  EXPECT_EQ(cache.presentationResourceStats().totalTrackedBytes, 0u);
}

#ifdef DONNER_EDITOR_WGPU
std::shared_ptr<geode::GeodeDevice> SharedGeodeDevice() {
  static const std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  return device;
}

/// Skips the current case, naming why, when \p device does not render through the transitional
/// adapter: the UI renderer imports and registers textures through that adapter's wgpu objects,
/// so a case that installs it exercises the adapter.
#define SKIP_UNLESS_UI_RENDERER_CAN_INSTALL(device)                                     \
  do {                                                                                  \
    if (!(device)->hasTransitionalAdapter()) {                                          \
      GTEST_SKIP() << "installs the UI renderer, which registers textures through the " \
                      "transitional adapter";                                           \
    }                                                                                   \
  } while (false)

std::unique_ptr<ImGuiRuntimeRenderer> InstallTestUiRenderer(
    geode::GeodeWgpuAdapterDevice& device, std::unique_ptr<UiTextureRegistry>* registry) {
  *registry = std::make_unique<UiTextureRegistry>(device);
  gpu::Result<std::unique_ptr<ImGuiRuntimeRenderer>> created =
      ImGuiRuntimeRenderer::Create(device, **registry, gpu::TextureFormat::RGBA8Unorm);
  if (created.hasError()) {
    return nullptr;
  }
  std::unique_ptr<ImGuiRuntimeRenderer> renderer = std::move(created).result();
  renderer->install();
  return renderer;
}

svg::RendererBitmap MakeBitmap(Vector2i dimensions, std::size_t rowBytes, uint8_t seed) {
  svg::RendererBitmap bitmap;
  bitmap.dimensions = dimensions;
  bitmap.rowBytes = rowBytes;
  bitmap.pixels.assign(rowBytes * static_cast<std::size_t>(dimensions.y), 0xEEu);
  for (int y = 0; y < dimensions.y; ++y) {
    for (int x = 0; x < dimensions.x; ++x) {
      uint8_t* pixel = bitmap.pixels.data() + static_cast<std::size_t>(y) * rowBytes +
                       static_cast<std::size_t>(x) * 4u;
      pixel[0] = static_cast<uint8_t>(seed + x);
      pixel[1] = static_cast<uint8_t>(seed + y);
      pixel[2] = static_cast<uint8_t>(seed + x + y);
      pixel[3] = 0xFFu;
    }
  }
  return bitmap;
}

RenderResult::CompositedPreview SingleBitmapTilePreview(std::uint64_t generation,
                                                        svg::RendererBitmap bitmap) {
  RenderResult::CompositedPreview preview;
  RenderResult::CompositedTile tile =
      MetadataTile(RenderResult::CompositedTile::Kind::Segment, generation, bitmap.dimensions,
                   bitmap.dimensions);
  tile.id = "bitmap:0";
  tile.bitmapDimsDoc = Vector2d(bitmap.dimensions.x, bitmap.dimensions.y);
  tile.bitmap = std::move(bitmap);
  preview.tiles.push_back(std::move(tile));
  return preview;
}

std::shared_ptr<const svg::RendererGeodeTextureSnapshot> UploadedSnapshot(
    const GlTextureCache& cache) {
  if (cache.tiles().size() != 1u) {
    return nullptr;
  }
  return std::static_pointer_cast<const svg::RendererGeodeTextureSnapshot>(
      cache.tiles().front().textureSnapshot);
}

std::array<uint8_t, 4> PixelAt(const svg::RendererBitmap& bitmap, int x, int y) {
  const uint8_t* pixel = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes +
                         static_cast<std::size_t>(x) * 4u;
  return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

TEST(GlTextureCacheTest, RuntimeBitmapUploadReplicatesBordersAndClearsUnusedAllocation) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);
  GlTextureCache cache(device);

  const svg::RendererBitmap first = MakeBitmap(Vector2i(7, 7), /*rowBytes=*/32u, /*seed=*/10u);
  cache.uploadComposited(SingleBitmapTilePreview(/*generation=*/1, first));
  std::shared_ptr<const svg::RendererGeodeTextureSnapshot> firstSnapshot = UploadedSnapshot(cache);
  ASSERT_NE(firstSnapshot, nullptr);
  ASSERT_THAT(firstSnapshot->allocationDimensions(), testing::Eq(Vector2i(8, 8)));
  const std::uint64_t createsAfterFirstUpload = device->lifetimeTextureCreates();

  const svg::RendererBitmap second = MakeBitmap(Vector2i(5, 5), /*rowBytes=*/24u, /*seed=*/40u);
  cache.uploadComposited(SingleBitmapTilePreview(/*generation=*/2, second));
  std::shared_ptr<const svg::RendererGeodeTextureSnapshot> secondSnapshot = UploadedSnapshot(cache);
  ASSERT_NE(secondSnapshot, nullptr);
  EXPECT_THAT(secondSnapshot.get(), testing::Ne(firstSnapshot.get()))
      << "A replacement must land in its own allocation instead of overwriting the one the "
         "previous publication still presents.";
  EXPECT_THAT(device->lifetimeTextureCreates(), testing::Eq(createsAfterFirstUpload + 1u));
  EXPECT_THAT(secondSnapshot->dimensions(), testing::Eq(Vector2i(5, 5)));
  EXPECT_THAT(secondSnapshot->allocationDimensions(), testing::Eq(Vector2i(8, 8)));

  auto* mutableSnapshot = const_cast<svg::RendererGeodeTextureSnapshot*>(secondSnapshot.get());
  ASSERT_TRUE(mutableSnapshot->setDimensions(Vector2i(8, 8)));
  const svg::RendererBitmap allocation = mutableSnapshot->takeSnapshot();
  ASSERT_THAT(allocation.dimensions, testing::Eq(Vector2i(8, 8)));
  EXPECT_THAT(PixelAt(allocation, 0, 0), testing::ElementsAre(40u, 40u, 40u, 255u));
  EXPECT_THAT(PixelAt(allocation, 4, 4), testing::ElementsAre(44u, 44u, 48u, 255u));
  EXPECT_THAT(PixelAt(allocation, 5, 4), testing::ElementsAre(44u, 44u, 48u, 255u));
  EXPECT_THAT(PixelAt(allocation, 6, 4), testing::ElementsAre(0u, 0u, 0u, 0u));
  EXPECT_THAT(PixelAt(allocation, 0, 5), testing::ElementsAre(40u, 44u, 44u, 255u));
  EXPECT_THAT(PixelAt(allocation, 5, 5), testing::ElementsAre(44u, 44u, 48u, 255u));
  EXPECT_THAT(PixelAt(allocation, 6, 5), testing::ElementsAre(0u, 0u, 0u, 0u));
  EXPECT_THAT(PixelAt(allocation, 0, 6), testing::ElementsAre(0u, 0u, 0u, 0u));
}

// The runtime write is chunked, so a replacement that overwrote the allocation a live registration
// still points at could leave that allocation holding part of the old payload and part of the new
// one once any chunk was refused. The superseded allocation must therefore stay byte-identical to
// what it was published with.
TEST(GlTextureCacheTest, ReplacedTilePayloadLeavesTheSupersededAllocationIntact) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);
  GlTextureCache cache(device);

  const svg::RendererBitmap published = MakeBitmap(Vector2i(5, 5), /*rowBytes=*/24u, /*seed=*/40u);
  cache.uploadComposited(SingleBitmapTilePreview(/*generation=*/1, published));
  std::shared_ptr<const svg::RendererGeodeTextureSnapshot> publishedSnapshot =
      UploadedSnapshot(cache);
  ASSERT_NE(publishedSnapshot, nullptr);

  const svg::RendererBitmap replacement =
      MakeBitmap(Vector2i(5, 5), /*rowBytes=*/24u, /*seed=*/90u);
  cache.uploadComposited(SingleBitmapTilePreview(/*generation=*/2, replacement));
  ASSERT_NE(UploadedSnapshot(cache), nullptr);

  const svg::RendererBitmap retained = publishedSnapshot->takeSnapshot();
  ASSERT_THAT(retained.dimensions, testing::Eq(Vector2i(5, 5)));
  EXPECT_THAT(PixelAt(retained, 0, 0), testing::ElementsAre(40u, 40u, 40u, 255u));
  EXPECT_THAT(PixelAt(retained, 4, 4), testing::ElementsAre(44u, 44u, 48u, 255u));
  EXPECT_THAT(PixelAt(retained, 4, 0), testing::ElementsAre(44u, 40u, 44u, 255u));
}

TEST(GlTextureCacheTest, RuntimeBitmapUploadPreservesBordersAcrossStagingChunkBoundary) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);
  GlTextureCache cache(device);
  const std::uint64_t createsBefore = device->lifetimeTextureCreates();
  const uint64_t submittedBefore = device->runtimeDevice().lastSubmittedSerial();
  geode::GeodeCounters counters;
  device->setCounters(&counters);

  const svg::RendererBitmap bitmap =
      MakeBitmap(Vector2i(257, 513), /*rowBytes=*/1032u, /*seed=*/7u);
  cache.uploadComposited(SingleBitmapTilePreview(/*generation=*/1, bitmap));
  device->setCounters(nullptr);
  std::shared_ptr<const svg::RendererGeodeTextureSnapshot> snapshot = UploadedSnapshot(cache);
  ASSERT_NE(snapshot, nullptr);
  EXPECT_THAT(snapshot->allocationDimensions(), testing::Eq(Vector2i(512, 1024)));
  EXPECT_THAT(device->lifetimeTextureCreates(), testing::Eq(createsBefore + 1u));
  EXPECT_THAT(device->runtimeDevice().lastSubmittedSerial(), testing::Eq(submittedBefore));
  EXPECT_THAT(counters.textureCreates, testing::Eq(1u));
  EXPECT_THAT(counters.textureWriteBytes, testing::Eq(512u * 1024u * 4u));
  EXPECT_THAT(counters.submits, testing::Eq(0u));

  auto* mutableSnapshot = const_cast<svg::RendererGeodeTextureSnapshot*>(snapshot.get());
  ASSERT_TRUE(mutableSnapshot->setDimensions(Vector2i(512, 1024)));
  const svg::RendererBitmap allocation = mutableSnapshot->takeSnapshot();
  ASSERT_THAT(allocation.dimensions, testing::Eq(Vector2i(512, 1024)));
  EXPECT_THAT(PixelAt(allocation, 0, 511), testing::ElementsAre(7u, 6u, 6u, 255u));
  EXPECT_THAT(PixelAt(allocation, 0, 512), testing::ElementsAre(7u, 7u, 7u, 255u));
  EXPECT_THAT(PixelAt(allocation, 256, 512), testing::ElementsAre(7u, 7u, 7u, 255u));
  EXPECT_THAT(PixelAt(allocation, 257, 512), testing::ElementsAre(7u, 7u, 7u, 255u));
  EXPECT_THAT(PixelAt(allocation, 258, 512), testing::ElementsAre(0u, 0u, 0u, 0u));
  EXPECT_THAT(PixelAt(allocation, 0, 513), testing::ElementsAre(7u, 7u, 7u, 255u));
  EXPECT_THAT(PixelAt(allocation, 257, 513), testing::ElementsAre(7u, 7u, 7u, 255u));
  EXPECT_THAT(PixelAt(allocation, 258, 513), testing::ElementsAre(0u, 0u, 0u, 0u));
  EXPECT_THAT(PixelAt(allocation, 0, 514), testing::ElementsAre(0u, 0u, 0u, 0u));
}

TEST(GlTextureCacheTest, RuntimeBitmapUploadRejectsShortStrideAndStorage) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);
  GlTextureCache cache(device);

  svg::RendererBitmap shortStride = MakeBitmap(Vector2i(3, 2), /*rowBytes=*/12u, /*seed=*/1u);
  shortStride.rowBytes = 11u;
  cache.uploadComposited(SingleBitmapTilePreview(/*generation=*/1, std::move(shortStride)));
  EXPECT_THAT(cache.tiles(), testing::IsEmpty());

  svg::RendererBitmap shortStorage = MakeBitmap(Vector2i(3, 2), /*rowBytes=*/12u, /*seed=*/1u);
  shortStorage.pixels.pop_back();
  cache.uploadComposited(SingleBitmapTilePreview(/*generation=*/2, std::move(shortStorage)));
  EXPECT_THAT(cache.tiles(), testing::IsEmpty());
}

TEST(GlTextureCacheTest, RuntimeBitmapUploadRejectsUnsupportedDimensionWithoutBackendAllocation) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);
  GlTextureCache cache(device);

  svg::RendererBitmap oversized =
      MakeBitmap(Vector2i(1 << 20, 1), /*rowBytes=*/4u << 20, /*seed=*/1u);
  const std::uint64_t createsBefore = device->lifetimeTextureCreates();
  cache.uploadComposited(SingleBitmapTilePreview(/*generation=*/1, std::move(oversized)));

  EXPECT_THAT(cache.tiles(), testing::IsEmpty());
  EXPECT_THAT(device->lifetimeTextureCreates(), testing::Eq(createsBefore));
}

std::shared_ptr<const svg::RendererTextureSnapshot> CreateCountingGeodeTextureSnapshot(
    const std::shared_ptr<geode::GeodeDevice>& device, int* destructionCount,
    const Vector2i& dimensions = Vector2i(1, 1)) {
  if (device == nullptr || destructionCount == nullptr) {
    return nullptr;
  }

  gpu::Result<gpu::Texture> created = device->runtimeDevice().createTexture(gpu::TextureDescriptor{
      "countingSnapshot",
      {static_cast<uint32_t>(dimensions.x), static_cast<uint32_t>(dimensions.y)},
      gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
  if (created.hasError()) {
    return nullptr;
  }

  svg::RendererGeodeTextureSnapshot snapshot =
      svg::RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
          device, std::move(created).result(), dimensions, wgpu::TextureFormat::RGBA8Unorm,
          svg::AlphaType::Premultiplied);
  if (!snapshot.isValid()) {
    return nullptr;
  }

  return std::shared_ptr<const svg::RendererTextureSnapshot>(
      new svg::RendererGeodeTextureSnapshot(std::move(snapshot)),
      [destructionCount](const svg::RendererTextureSnapshot* snapshot) {
        delete snapshot;
        ++(*destructionCount);
      });
}

// Build a one-tile composited preview backed by a GPU texture snapshot. Reusing
// the same `id` across uploads with an advancing `generation` exercises the
// cache's per-tile texture retirement, which is the machinery the deleted
// overlay-texture path used to drive.
RenderResult::CompositedPreview SingleSnapshotTilePreview(
    std::string id, std::uint64_t generation,
    std::shared_ptr<const svg::RendererTextureSnapshot> snapshot) {
  const Vector2i dims = snapshot != nullptr ? snapshot->dimensions() : Vector2i(1, 1);
  RenderResult::CompositedPreview preview;
  RenderResult::CompositedTile tile =
      MetadataTile(RenderResult::CompositedTile::Kind::Layer, generation, dims, Vector2i(100, 100));
  tile.id = std::move(id);
  tile.bitmapDimsDoc = Vector2d(dims.x, dims.y);
  tile.textureSnapshot = std::move(snapshot);
  preview.tiles.push_back(std::move(tile));
  return preview;
}

/// A retired snapshot's backing survives its safety window and is then destroyed explicitly, not
/// left for the backend to collect. A released snapshot hands its backing to the owning context's
/// retirement mailbox, which that context drains at every frame boundary, so the case drains it
/// wherever a frame boundary would fall before it looks.
TEST(GlTextureCacheTest, RetiredSnapshotsAgeByPresentationFrame) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);
  SKIP_UNLESS_UI_RENDERER_CAN_INSTALL(device);
  // Start from an empty mailbox, so backing released by earlier cases is not counted here.
  device->drainDeferredTextureBackings();
  int firstDestructionCount = 0;
  int secondDestructionCount = 0;
  const std::uint64_t backingDestroysBefore =
      geode::ScopedWgpuHandle<wgpu::Texture>::backingDestroyCountForTesting();

  {
    ImGuiContext* context = ImGui::CreateContext();
    std::unique_ptr<UiTextureRegistry> registry;
    std::unique_ptr<ImGuiRuntimeRenderer> renderer =
        InstallTestUiRenderer(device->adapterDevice(), &registry);
    ASSERT_NE(renderer, nullptr);

    GlTextureCache cache(device);
    std::shared_ptr<const svg::RendererTextureSnapshot> firstSnapshot =
        CreateCountingGeodeTextureSnapshot(device, &firstDestructionCount);
    ASSERT_NE(firstSnapshot, nullptr);
    cache.uploadComposited(SingleSnapshotTilePreview("layer:0", /*generation=*/1, firstSnapshot));
    firstSnapshot.reset();

    std::shared_ptr<const svg::RendererTextureSnapshot> secondSnapshot =
        CreateCountingGeodeTextureSnapshot(device, &secondDestructionCount);
    ASSERT_NE(secondSnapshot, nullptr);
    // Same tile id with an advanced generation retires the first snapshot's
    // texture, mirroring the old overlay-texture retirement path.
    cache.uploadComposited(SingleSnapshotTilePreview("layer:0", /*generation=*/2, secondSnapshot));
    secondSnapshot.reset();

    EXPECT_EQ(firstDestructionCount, 0);
    EXPECT_EQ(secondDestructionCount, 0);
    device->drainDeferredTextureBackings();
    EXPECT_EQ(geode::ScopedWgpuHandle<wgpu::Texture>::backingDestroyCountForTesting(),
              backingDestroysBefore)
        << "Retirement must keep the replaced texture backing alive through its safety window";

    cache.advancePresentationFrame();
    cache.advancePresentationFrame();
    cache.advancePresentationFrame();
    EXPECT_EQ(firstDestructionCount, 0);
    EXPECT_EQ(secondDestructionCount, 0);
    device->drainDeferredTextureBackings();
    EXPECT_EQ(geode::ScopedWgpuHandle<wgpu::Texture>::backingDestroyCountForTesting(),
              backingDestroysBefore);

    cache.advancePresentationFrame();
    EXPECT_EQ(firstDestructionCount, 1);
    EXPECT_EQ(secondDestructionCount, 0);
    EXPECT_THAT(device->deferredTextureDestroyCountForTesting(), testing::Eq(1u))
        << "The aged-out snapshot must hand its backing to the owning context's retirement";
    device->drainDeferredTextureBackings();
    EXPECT_EQ(geode::ScopedWgpuHandle<wgpu::Texture>::backingDestroyCountForTesting(),
              backingDestroysBefore + 1u)
        << "The aged-out owned snapshot must explicitly destroy its GPU backing";
    renderer->uninstall();
    ImGui::DestroyContext(context);
  }

  EXPECT_EQ(firstDestructionCount, 1);
  EXPECT_EQ(secondDestructionCount, 1);
  EXPECT_THAT(device->deferredTextureDestroyCountForTesting(), testing::Eq(1u))
      << "Cache teardown must hand the remaining active snapshot's backing to the owning "
         "context's retirement";
  device->drainDeferredTextureBackings();
  EXPECT_EQ(geode::ScopedWgpuHandle<wgpu::Texture>::backingDestroyCountForTesting(),
            backingDestroysBefore + 2u)
      << "Cache teardown must explicitly destroy the remaining active snapshot backing";
}

TEST(GlTextureCacheTest, RegisteredBackingSurvivesUntilItsExactRetirementIsReleased) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);
  SKIP_UNLESS_UI_RENDERER_CAN_INSTALL(device);
  ImGuiContext* context = ImGui::CreateContext();
  geode::GeodeWgpuAdapterDevice& runtimeDevice = device->adapterDevice();
  UiTextureRegistry registry(runtimeDevice);
  gpu::Result<std::unique_ptr<ImGuiRuntimeRenderer>> created =
      ImGuiRuntimeRenderer::Create(runtimeDevice, registry, gpu::TextureFormat::RGBA8Unorm);
  ASSERT_FALSE(created.hasError());
  std::unique_ptr<ImGuiRuntimeRenderer> renderer = std::move(created).result();
  renderer->install();

  GlTextureCache cache(device);
  int firstDestructionCount = 0;
  int secondDestructionCount = 0;
  cache.uploadComposited(SingleSnapshotTilePreview(
      "layer:retirement", 1, CreateCountingGeodeTextureSnapshot(device, &firstDestructionCount)));
  cache.uploadComposited(SingleSnapshotTilePreview(
      "layer:retirement", 2, CreateCountingGeodeTextureSnapshot(device, &secondDestructionCount)));

  for (uint32_t frame = 0; frame <= UiTextureRegistry::kDefaultRetirementFrames; ++frame) {
    renderer->advanceFrame();
    cache.advancePresentationFrame();
  }
  EXPECT_EQ(renderer->retainedTextureBackingCountForTest(), 1u)
      << "cache retirement must transfer the backing into the renderer";
  for (uint32_t frame = 1; frame < registry.retirementFrames(); ++frame) {
    renderer->advanceFrame();
    EXPECT_EQ(renderer->retainedTextureBackingCountForTest(), 1u);
  }
  EXPECT_FALSE(renderer->advanceFrame().empty());
  EXPECT_EQ(renderer->retainedTextureBackingCountForTest(), 0u);

  renderer->uninstall();
  ImGui::DestroyContext(context);
}

TEST(GlTextureCacheTest, RegisteredBackingIsDestroyedIfRendererUninstallsBeforeCache) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);
  SKIP_UNLESS_UI_RENDERER_CAN_INSTALL(device);
  ImGuiContext* context = ImGui::CreateContext();
  std::unique_ptr<UiTextureRegistry> registry;
  std::unique_ptr<ImGuiRuntimeRenderer> renderer =
      InstallTestUiRenderer(device->adapterDevice(), &registry);
  ASSERT_NE(renderer, nullptr);
  int destructionCount = 0;
  {
    GlTextureCache cache(device);
    cache.uploadComposited(SingleSnapshotTilePreview(
        "layer:uninstalled", 1, CreateCountingGeodeTextureSnapshot(device, &destructionCount)));
    renderer->uninstall();
  }
  EXPECT_EQ(destructionCount, 1);
  EXPECT_EQ(renderer->retainedTextureBackingCountForTest(), 0u)
      << "a cache cannot publish backing into a renderer after it uninstalls";
  ImGui::DestroyContext(context);
}

TEST(GlTextureCacheTest, PresentationResourceStatsTrackActiveAndRetiredTextures) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);
  SKIP_UNLESS_UI_RENDERER_CAN_INSTALL(device);
  ImGuiContext* context = ImGui::CreateContext();
  std::unique_ptr<UiTextureRegistry> registry;
  std::unique_ptr<ImGuiRuntimeRenderer> renderer =
      InstallTestUiRenderer(device->adapterDevice(), &registry);
  ASSERT_NE(renderer, nullptr);

  int firstDestructionCount = 0;
  std::shared_ptr<const svg::RendererTextureSnapshot> firstSnapshot =
      CreateCountingGeodeTextureSnapshot(device, &firstDestructionCount, Vector2i(3, 5));
  ASSERT_NE(firstSnapshot, nullptr);

  GlTextureCache cache(device);
  cache.uploadComposited(SingleSnapshotTilePreview("layer:0", /*generation=*/1, firstSnapshot));
  firstSnapshot.reset();

  PresentationResourceStats stats = cache.presentationResourceStats();
  EXPECT_EQ(stats.activeTileBytes, 3u * 5u * 4u);
  EXPECT_EQ(stats.pendingRetiredBytes, 0u);
  EXPECT_EQ(stats.totalTrackedBytes, 3u * 5u * 4u);
  EXPECT_EQ(stats.peakTrackedBytes, stats.totalTrackedBytes);
  EXPECT_EQ(stats.largestAllocationPx, Vector2i(3, 5));

  int secondDestructionCount = 0;
  std::shared_ptr<const svg::RendererTextureSnapshot> secondSnapshot =
      CreateCountingGeodeTextureSnapshot(device, &secondDestructionCount, Vector2i(2, 2));
  ASSERT_NE(secondSnapshot, nullptr);
  // Same tile id, advanced generation: the first texture retires.
  cache.uploadComposited(SingleSnapshotTilePreview("layer:0", /*generation=*/2, secondSnapshot));
  secondSnapshot.reset();

  stats = cache.presentationResourceStats();
  EXPECT_EQ(stats.activeTileBytes, 2u * 2u * 4u);
  EXPECT_EQ(stats.pendingRetiredBytes, 3u * 5u * 4u);
  EXPECT_EQ(stats.pendingRetiredTextures, 1);
  EXPECT_EQ(stats.totalTrackedBytes, 2u * 2u * 4u + 3u * 5u * 4u);
  EXPECT_EQ(stats.peakTrackedBytes, stats.totalTrackedBytes);
  EXPECT_EQ(stats.largestAllocationPx, Vector2i(3, 5));

  cache.advancePresentationFrame();
  stats = cache.presentationResourceStats();
  EXPECT_EQ(stats.pendingRetiredBytes, 0u);
  EXPECT_EQ(stats.agedRetiredBytes, 3u * 5u * 4u);
  EXPECT_EQ(stats.agedRetiredTextures, 1);

  cache.advancePresentationFrame();
  cache.advancePresentationFrame();
  cache.advancePresentationFrame();
  stats = cache.presentationResourceStats();
  EXPECT_EQ(stats.pendingRetiredBytes, 0u);
  EXPECT_EQ(stats.agedRetiredBytes, 0u);
  EXPECT_EQ(stats.activeTileBytes, 2u * 2u * 4u);
  EXPECT_EQ(stats.totalTrackedBytes, 2u * 2u * 4u);
  EXPECT_GE(stats.peakTrackedBytes, 2u * 2u * 4u + 3u * 5u * 4u);
  EXPECT_EQ(firstDestructionCount, 1);
  EXPECT_EQ(secondDestructionCount, 0);
  renderer->uninstall();
  ImGui::DestroyContext(context);
}

TEST(GlTextureCacheTest, UnboundedUploadRetainsSeparateOverviewAcrossBoundedUpload) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);

  int overviewDestructionCount = 0;
  RenderResult::CompositedPreview overviewPreview;
  RenderResult::CompositedTile overviewTile = MetadataTile(
      RenderResult::CompositedTile::Kind::Segment, 1, Vector2i(1, 1), Vector2i(100, 100));
  overviewTile.id = "seg:0";
  overviewTile.bitmapDimsDoc = Vector2d(100.0, 100.0);
  overviewTile.textureSnapshot =
      CreateCountingGeodeTextureSnapshot(device, &overviewDestructionCount);
  ASSERT_NE(overviewTile.textureSnapshot, nullptr);
  overviewPreview.tiles.push_back(std::move(overviewTile));

  GlTextureCache cache(device);
  cache.uploadComposited(overviewPreview, RasterViewportForTest(/*viewportBounded=*/false));
  ASSERT_EQ(cache.tiles().size(), 1u);
  ASSERT_EQ(cache.overviewTiles().size(), 1u);
  EXPECT_FALSE(cache.activeTilesViewportBounded());
  EXPECT_EQ(cache.overviewTiles().front().rasterCanvasSize, Vector2i(100, 100));

  int boundedDestructionCount = 0;
  RenderResult::CompositedPreview boundedPreview;
  RenderResult::CompositedTile boundedTile = MetadataTile(
      RenderResult::CompositedTile::Kind::Segment, 2, Vector2i(1, 1), Vector2i(20, 20));
  boundedTile.id = "seg:0";
  boundedTile.canvasOffsetDoc = Vector2d(40.0, 40.0);
  boundedTile.bitmapDimsDoc = Vector2d(10.0, 10.0);
  boundedTile.textureSnapshot =
      CreateCountingGeodeTextureSnapshot(device, &boundedDestructionCount);
  ASSERT_NE(boundedTile.textureSnapshot, nullptr);
  boundedPreview.tiles.push_back(std::move(boundedTile));

  cache.uploadComposited(boundedPreview, RasterViewportForTest(/*viewportBounded=*/true));

  ASSERT_EQ(cache.tiles().size(), 1u);
  ASSERT_EQ(cache.overviewTiles().size(), 1u);
  EXPECT_TRUE(cache.activeTilesViewportBounded());
  const PresentationCoverageDiagnostics coverage = cache.coverageDiagnostics();
  EXPECT_TRUE(coverage.activeTilesViewportBounded);
  EXPECT_TRUE(coverage.overviewInfillAvailable);
  EXPECT_EQ(coverage.activeRasterDocumentRect, Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));
  EXPECT_EQ(coverage.overviewRasterDocumentRect, Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));
  EXPECT_EQ(coverage.activeOutputSizePx, Vector2i(20, 20));
  EXPECT_EQ(coverage.overviewOutputSizePx, Vector2i(100, 100));
  EXPECT_EQ(cache.tiles().front().rasterCanvasSize, Vector2i(20, 20));
  EXPECT_EQ(cache.overviewTiles().front().rasterCanvasSize, Vector2i(100, 100))
      << "Bounded uploads may reuse tile ids and must not overwrite the retained overview.";
  EXPECT_EQ(overviewDestructionCount, 0);
  EXPECT_EQ(boundedDestructionCount, 0);
}

TEST(GlTextureCacheTest, OverviewUploadDoesNotReplaceActiveBoundedTiles) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);

  int boundedDestructionCount = 0;
  RenderResult::CompositedPreview boundedPreview;
  RenderResult::CompositedTile boundedTile = MetadataTile(
      RenderResult::CompositedTile::Kind::Segment, 2, Vector2i(1, 1), Vector2i(20, 20));
  boundedTile.id = "seg:0";
  boundedTile.canvasOffsetDoc = Vector2d(40.0, 40.0);
  boundedTile.bitmapDimsDoc = Vector2d(10.0, 10.0);
  boundedTile.textureSnapshot =
      CreateCountingGeodeTextureSnapshot(device, &boundedDestructionCount);
  ASSERT_NE(boundedTile.textureSnapshot, nullptr);
  boundedPreview.tiles.push_back(std::move(boundedTile));

  GlTextureCache cache(device);
  cache.uploadComposited(boundedPreview, RasterViewportForTest(/*viewportBounded=*/true));
  ASSERT_EQ(cache.tiles().size(), 1u);
  EXPECT_TRUE(cache.overviewTiles().empty());

  int overviewDestructionCount = 0;
  RenderResult::CompositedPreview overviewPreview;
  RenderResult::CompositedTile overviewTile = MetadataTile(
      RenderResult::CompositedTile::Kind::Segment, 1, Vector2i(1, 1), Vector2i(100, 100));
  overviewTile.id = "full-canvas";
  overviewTile.bitmapDimsDoc = Vector2d(100.0, 100.0);
  overviewTile.textureSnapshot =
      CreateCountingGeodeTextureSnapshot(device, &overviewDestructionCount);
  ASSERT_NE(overviewTile.textureSnapshot, nullptr);
  overviewPreview.tiles.push_back(std::move(overviewTile));

  cache.uploadCompositedOverview(overviewPreview, RasterViewportForTest(/*viewportBounded=*/false));

  ASSERT_EQ(cache.tiles().size(), 1u);
  ASSERT_EQ(cache.overviewTiles().size(), 1u);
  EXPECT_TRUE(cache.activeTilesViewportBounded());
  EXPECT_EQ(cache.tiles().front().id, "seg:0");
  EXPECT_EQ(cache.overviewTiles().front().id, "full-canvas");
  EXPECT_TRUE(cache.coverageDiagnostics().overviewInfillAvailable);
  EXPECT_EQ(boundedDestructionCount, 0);
  EXPECT_EQ(overviewDestructionCount, 0);
}
#endif

}  // namespace
}  // namespace donner::editor
