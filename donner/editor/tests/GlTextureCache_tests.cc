#include "donner/editor/GlTextureCache.h"

#include <memory>
#include <utility>

#include "donner/svg/renderer/RendererInterface.h"
#ifdef DONNER_EDITOR_WGPU
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#endif
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

  EXPECT_TRUE(cache.tiles().empty());
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

std::shared_ptr<const svg::RendererTextureSnapshot> CreateCountingGeodeTextureSnapshot(
    const std::shared_ptr<geode::GeodeDevice>& device, int* destructionCount,
    const Vector2i& dimensions = Vector2i(1, 1)) {
  if (device == nullptr || destructionCount == nullptr) {
    return nullptr;
  }

  donner::gpu::Result<donner::gpu::Texture> texture =
      device->adapterDevice().createTexture(donner::gpu::TextureDescriptor{
          "CountingGeodeSnapshot",
          donner::gpu::Extent2d{static_cast<uint32_t>(dimensions.x),
                                static_cast<uint32_t>(dimensions.y)},
          donner::gpu::TextureFormat::RGBA8Unorm,
          donner::gpu::TextureUsage::Sampled | donner::gpu::TextureUsage::CopyDst});
  if (texture.hasError()) {
    return nullptr;
  }

  return std::shared_ptr<const svg::RendererTextureSnapshot>(
      new svg::RendererGeodeTextureSnapshot(device, std::move(texture).result(), dimensions,
                                            wgpu::TextureFormat::RGBA8Unorm),
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

TEST(GlTextureCacheTest, RetiredSnapshotsAgeByPresentationFrame) {
  int firstDestructionCount = 0;
  int secondDestructionCount = 0;

  {
    std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
    ASSERT_NE(device, nullptr);

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

    cache.advancePresentationFrame();
    cache.advancePresentationFrame();
    cache.advancePresentationFrame();
    EXPECT_EQ(firstDestructionCount, 0);
    EXPECT_EQ(secondDestructionCount, 0);

    cache.advancePresentationFrame();
    EXPECT_EQ(firstDestructionCount, 1);
    EXPECT_EQ(secondDestructionCount, 0);
  }

  EXPECT_EQ(firstDestructionCount, 1);
  EXPECT_EQ(secondDestructionCount, 1);
}

TEST(GlTextureCacheTest, PresentationResourceStatsTrackActiveAndRetiredTextures) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);

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
