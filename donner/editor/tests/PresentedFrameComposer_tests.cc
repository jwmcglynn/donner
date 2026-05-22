#include "donner/editor/PresentedFrameComposer.h"

#include <limits>
#include <optional>

#include "donner/base/EcsRegistry.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

std::optional<PresentedDragBaseline> Baseline(Entity entity,
                                              const Vector2d& representedTranslationDoc,
                                              const Vector2d& activeTranslationDoc) {
  return PresentedDragBaseline{
      .entity = entity,
      .representedTranslationDoc = representedTranslationDoc,
      .activeTranslationDoc = activeTranslationDoc,
  };
}

TEST(PresentedFrameComposerTest, ActiveDragTargetAddsLiveDeltaToCachedTranslation) {
  PresentedFrameTileGeometry tile;
  tile.isDragTarget = true;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  const Entity entity{123};
  const std::optional<PresentedDragBaseline> baseline =
      Baseline(entity, Vector2d::Zero(), Vector2d(7.0, 8.0));

  EXPECT_EQ(ResolvePresentedTileDragTranslation(tile, baseline), Vector2d(8.0, 10.0));
}

TEST(PresentedFrameComposerTest, ActiveDragTargetDoesNotDoubleApplyDisplayedDelta) {
  PresentedFrameTileGeometry tile;
  tile.isDragTarget = true;
  tile.dragTranslationDoc = Vector2d(14.0, 6.0);

  const Entity entity{123};
  const std::optional<PresentedDragBaseline> baseline =
      Baseline(entity, Vector2d(14.0, 6.0), Vector2d(14.0, 6.0));

  EXPECT_EQ(ResolvePresentedTileDragTranslation(tile, baseline), Vector2d(14.0, 6.0));
}

TEST(PresentedFrameComposerTest, ActiveRedragTargetKeepsPreviousDragOffset) {
  PresentedFrameTileGeometry tile;
  tile.isDragTarget = true;
  tile.dragTranslationDoc = Vector2d(21.0, 32.0);

  const Entity entity{123};
  const std::optional<PresentedDragBaseline> baseline =
      Baseline(entity, Vector2d::Zero(), Vector2d(7.0, 8.0));

  EXPECT_EQ(ResolvePresentedTileDragTranslation(tile, baseline), Vector2d(28.0, 40.0));
}

TEST(PresentedFrameComposerTest, IdleDragTargetKeepsCachedTileTranslation) {
  PresentedFrameTileGeometry tile;
  tile.isDragTarget = true;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  EXPECT_EQ(ResolvePresentedTileDragTranslation(tile, std::nullopt), Vector2d(1.0, 2.0));
}

TEST(PresentedFrameComposerTest, MissingBaselineKeepsCachedTileTranslation) {
  PresentedFrameTileGeometry tile;
  tile.isDragTarget = true;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  EXPECT_EQ(ResolvePresentedTileDragTranslation(tile, std::nullopt), Vector2d(1.0, 2.0));
}

TEST(PresentedFrameComposerTest, NonDragTilesKeepCachedTileTranslation) {
  PresentedFrameTileGeometry tile;
  tile.isDragTarget = false;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  const std::optional<PresentedDragBaseline> baseline =
      Baseline(Entity{123}, Vector2d(7.0, 8.0), Vector2d(7.0, 8.0));

  EXPECT_EQ(ResolvePresentedTileDragTranslation(tile, baseline), Vector2d(1.0, 2.0));
}

TEST(PresentedFrameComposerTest, OverlayAddsLiveDeltaToCachedTranslation) {
  const std::optional<PresentedDragBaseline> baseline =
      Baseline(Entity{123}, Vector2d(14.0, 6.0), Vector2d(20.0, 8.0));

  EXPECT_EQ(ResolvePresentedOverlayDragTranslation(baseline), Vector2d(6.0, 2.0));
}

TEST(PresentedFrameComposerTest, OverlayWithoutBaselineStaysAtCachedPlacement) {
  EXPECT_EQ(ResolvePresentedOverlayDragTranslation(std::nullopt), Vector2d::Zero());
}

TEST(PresentedFrameComposerTest, ComputesTileRectFromDocumentGeometry) {
  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(10.0, 20.0);
  tile.bitmapDimsDoc = Vector2d(30.0, 40.0);
  tile.dragTranslationDoc = Vector2d(2.0, 3.0);

  const Transform2d outputFromCanvasTransform =
      Transform2d::Scale(2.0) * Transform2d::Translate(Vector2d(100.0, 200.0));
  const std::optional<PresentedTileRect> rect =
      ComputePresentedTileRect(tile, outputFromCanvasTransform, std::nullopt);

  ASSERT_TRUE(rect.has_value());
  EXPECT_EQ(rect->topLeft, Vector2d(124.0, 246.0));
  EXPECT_EQ(rect->bottomRight, Vector2d(184.0, 326.0));
  EXPECT_EQ(rect->effectiveDragTranslationDoc, Vector2d(2.0, 3.0));
}

TEST(PresentedFrameComposerTest, ComputesTileRectFromNonZeroCanvasOrigin) {
  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(12.25, 24.75);
  tile.bitmapDimsDoc = Vector2d(3.5, 4.5);
  tile.dragTranslationDoc = Vector2d(0.75, 1.25);

  const Vector2d viewBoxTopLeft(10.0, 20.0);
  const Transform2d outputFromCanvasTransform =
      Transform2d::Translate(-viewBoxTopLeft) * Transform2d::Scale(Vector2d(4.0, 5.0));
  const std::optional<PresentedTileRect> rect =
      ComputePresentedTileRect(tile, outputFromCanvasTransform, std::nullopt);

  ASSERT_TRUE(rect.has_value());
  EXPECT_EQ(rect->topLeft, Vector2d(12.0, 30.0));
  EXPECT_EQ(rect->bottomRight, Vector2d(26.0, 52.5));
}

TEST(PresentedFrameComposerTest, RejectsInvalidTileGeometry) {
  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(10.0, 20.0);
  tile.bitmapDimsDoc = Vector2d(0.0, 40.0);

  EXPECT_FALSE(ComputePresentedTileRect(tile, Transform2d::Scale(2.0), std::nullopt).has_value());

  tile.bitmapDimsDoc = Vector2d(30.0, -1.0);
  EXPECT_FALSE(ComputePresentedTileRect(tile, Transform2d::Scale(2.0), std::nullopt).has_value());
}

TEST(PresentedFrameComposerTest, RejectsInvalidOutputTransform) {
  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(10.0, 20.0);
  tile.bitmapDimsDoc = Vector2d(30.0, 40.0);

  EXPECT_FALSE(ComputePresentedTileRect(tile, Transform2d::Scale(0.0), std::nullopt).has_value());

  const double infinity = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(
      ComputePresentedTileRect(tile, Transform2d::Translate(Vector2d(infinity, 0.0)), std::nullopt)
          .has_value());
}

TEST(PresentedFrameComposerTest, RoundsPresentedRectToPixelRect) {
  const PresentedTileRect rect{
      .topLeft = Vector2d(10.4, 20.5),
      .bottomRight = Vector2d(41.0, 59.6),
      .effectiveDragTranslationDoc = Vector2d::Zero(),
  };

  EXPECT_EQ(RoundPresentedTileRectToPixelRect(rect), (PresentedPixelRect{
                                                         .x = 10,
                                                         .y = 21,
                                                         .width = 31,
                                                         .height = 39,
                                                     }));
}

}  // namespace
}  // namespace donner::editor
