#include "donner/editor/GlTextureCache.h"

#include <memory>
#include <utility>

#include "donner/svg/renderer/RendererInterface.h"
#ifdef DONNER_EDITOR_WGPU
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
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

  wgpu::TextureDescriptor textureDesc = {};
  textureDesc.size = {static_cast<uint32_t>(dimensions.x), static_cast<uint32_t>(dimensions.y), 1};
  textureDesc.mipLevelCount = 1;
  textureDesc.sampleCount = 1;
  textureDesc.dimension = wgpu::TextureDimension::_2D;
  textureDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  textureDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  wgpu::Texture texture = device->device().createTexture(textureDesc);
  if (!texture) {
    return nullptr;
  }

  return std::shared_ptr<const svg::RendererTextureSnapshot>(
      new svg::RendererGeodeTextureSnapshot(device, texture, dimensions,
                                            wgpu::TextureFormat::RGBA8Unorm),
      [destructionCount](const svg::RendererTextureSnapshot* snapshot) {
        delete snapshot;
        ++(*destructionCount);
      });
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
    cache.uploadOverlayTexture(firstSnapshot);
    firstSnapshot.reset();

    std::shared_ptr<const svg::RendererTextureSnapshot> secondSnapshot =
        CreateCountingGeodeTextureSnapshot(device, &secondDestructionCount);
    ASSERT_NE(secondSnapshot, nullptr);
    cache.uploadOverlayTexture(secondSnapshot);
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

TEST(GlTextureCacheTest, OverlayScreenRectTracksCurrentUploadOnly) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);

  int firstDestructionCount = 0;
  std::shared_ptr<const svg::RendererTextureSnapshot> firstSnapshot =
      CreateCountingGeodeTextureSnapshot(device, &firstDestructionCount);
  ASSERT_NE(firstSnapshot, nullptr);

  GlTextureCache cache(device);
  cache.uploadOverlayTexture(firstSnapshot);
  firstSnapshot.reset();
  cache.setOverlayScreenRect(Box2d::FromXYWH(10.0, 20.0, 30.0, 40.0));

  ASSERT_TRUE(cache.overlayScreenRect().has_value());
  EXPECT_EQ(*cache.overlayScreenRect(), Box2d::FromXYWH(10.0, 20.0, 30.0, 40.0));

  int secondDestructionCount = 0;
  std::shared_ptr<const svg::RendererTextureSnapshot> secondSnapshot =
      CreateCountingGeodeTextureSnapshot(device, &secondDestructionCount);
  ASSERT_NE(secondSnapshot, nullptr);

  cache.uploadOverlayTexture(secondSnapshot);
  EXPECT_FALSE(cache.overlayScreenRect().has_value())
      << "A new upload must not inherit stale screen-space placement.";

  cache.setOverlayScreenRect(Box2d::FromXYWH(1.0, 2.0, 3.0, 4.0));
  cache.clearOverlay();
  EXPECT_FALSE(cache.overlayScreenRect().has_value());
}

TEST(GlTextureCacheTest, PresentationResourceStatsTrackActiveAndRetiredTextures) {
  std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_NE(device, nullptr);

  int firstDestructionCount = 0;
  std::shared_ptr<const svg::RendererTextureSnapshot> firstSnapshot =
      CreateCountingGeodeTextureSnapshot(device, &firstDestructionCount, Vector2i(3, 5));
  ASSERT_NE(firstSnapshot, nullptr);

  GlTextureCache cache(device);
  cache.uploadOverlayTexture(firstSnapshot);
  firstSnapshot.reset();

  PresentationResourceStats stats = cache.presentationResourceStats();
  EXPECT_EQ(stats.overlayBytes, 3u * 5u * 4u);
  EXPECT_EQ(stats.pendingRetiredBytes, 0u);
  EXPECT_EQ(stats.totalTrackedBytes, 3u * 5u * 4u);
  EXPECT_EQ(stats.peakTrackedBytes, stats.totalTrackedBytes);
  EXPECT_EQ(stats.largestAllocationPx, Vector2i(3, 5));

  int secondDestructionCount = 0;
  std::shared_ptr<const svg::RendererTextureSnapshot> secondSnapshot =
      CreateCountingGeodeTextureSnapshot(device, &secondDestructionCount, Vector2i(2, 2));
  ASSERT_NE(secondSnapshot, nullptr);
  cache.uploadOverlayTexture(secondSnapshot);
  secondSnapshot.reset();

  stats = cache.presentationResourceStats();
  EXPECT_EQ(stats.overlayBytes, 2u * 2u * 4u);
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
  EXPECT_EQ(cache.tiles().front().rasterCanvasSize, Vector2i(20, 20));
  EXPECT_EQ(cache.overviewTiles().front().rasterCanvasSize, Vector2i(100, 100))
      << "Bounded uploads may reuse tile ids and must not overwrite the retained overview.";
  EXPECT_EQ(overviewDestructionCount, 0);
  EXPECT_EQ(boundedDestructionCount, 0);
}
#endif

}  // namespace
}  // namespace donner::editor
