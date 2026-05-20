#include "donner/editor/GlTextureCache.h"

#include "donner/svg/renderer/RendererInterface.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

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

TEST(GlTextureCacheTest, PayloadIdentityUsesActualBitmapDimensions) {
  RenderResult::CompositedTile tile = MetadataTile(RenderResult::CompositedTile::Kind::Segment, 4,
                                                   Vector2i(1, 1), Vector2i(64, 64));
  tile.bitmap.dimensions = Vector2i(8, 9);
  tile.bitmap.rowBytes = 8u * 4u;
  tile.bitmap.pixels.resize(8u * 9u * 4u);

  const CompositedTileTextureIdentity identity = TextureIdentityForCompositedTile(tile);

  EXPECT_EQ(identity.textureDimsPx, Vector2i(8, 9));
  EXPECT_EQ(identity.rasterCanvasSize, Vector2i(64, 64));
}

#ifdef DONNER_EDITOR_WGPU
class CountingTextureSnapshot final : public svg::RendererTextureSnapshot {
public:
  explicit CountingTextureSnapshot(int* destructionCount) : destructionCount_(destructionCount) {}
  ~CountingTextureSnapshot() override { ++(*destructionCount_); }

  [[nodiscard]] Vector2i dimensions() const override { return Vector2i(1, 1); }
  [[nodiscard]] svg::AlphaType alphaType() const override { return svg::AlphaType::Premultiplied; }

private:
  int* destructionCount_ = nullptr;
};

TEST(GlTextureCacheTest, RetiredSnapshotsAgeByPresentationFrame) {
  int destructionCount = 0;

  {
    GlTextureCache cache;
    std::shared_ptr<const svg::RendererTextureSnapshot> snapshot =
        std::make_shared<CountingTextureSnapshot>(&destructionCount);
    cache.uploadOverlayTexture(snapshot);
    snapshot.reset();

    EXPECT_EQ(destructionCount, 0);

    cache.advancePresentationFrame();
    cache.advancePresentationFrame();
    cache.advancePresentationFrame();
    EXPECT_EQ(destructionCount, 0);

    cache.advancePresentationFrame();
    EXPECT_EQ(destructionCount, 1);
  }

  EXPECT_EQ(destructionCount, 1);
}
#endif

}  // namespace
}  // namespace donner::editor
