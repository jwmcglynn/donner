#include "donner/editor/PresentedFrameComposer.h"

#include <limits>
#include <optional>

#include "donner/base/EcsRegistry.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

std::optional<PresentedDragPreview> Preview(Entity entity, const Vector2d& translationDoc) {
  return PresentedDragPreview{
      .entity = entity,
      .translationDoc = translationDoc,
  };
}

TEST(PresentedFrameComposerTest, ActiveDragTargetUsesLivePreviewTranslation) {
  PresentedFrameTileGeometry tile;
  tile.isDragTarget = true;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  const Entity entity{123};
  const std::optional<PresentedDragPreview> active = Preview(entity, Vector2d(7.0, 8.0));
  const std::optional<PresentedDragPreview> displayed = Preview(entity, Vector2d::Zero());

  EXPECT_EQ(ResolvePresentedTileDragTranslation(tile, active, displayed), Vector2d(7.0, 8.0));
}

TEST(PresentedFrameComposerTest, IdleDragTargetKeepsCachedTileTranslation) {
  PresentedFrameTileGeometry tile;
  tile.isDragTarget = true;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  const std::optional<PresentedDragPreview> displayed = Preview(Entity{123}, Vector2d::Zero());

  EXPECT_EQ(ResolvePresentedTileDragTranslation(tile, std::nullopt, displayed), Vector2d(1.0, 2.0));
}

TEST(PresentedFrameComposerTest, TargetSwitchKeepsCachedTileTranslation) {
  PresentedFrameTileGeometry tile;
  tile.isDragTarget = true;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  const std::optional<PresentedDragPreview> active = Preview(Entity{456}, Vector2d(7.0, 8.0));
  const std::optional<PresentedDragPreview> displayed = Preview(Entity{123}, Vector2d::Zero());

  EXPECT_EQ(ResolvePresentedTileDragTranslation(tile, active, displayed), Vector2d(1.0, 2.0));
}

TEST(PresentedFrameComposerTest, NonDragTilesKeepCachedTileTranslation) {
  PresentedFrameTileGeometry tile;
  tile.isDragTarget = false;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  const Entity entity{123};
  const std::optional<PresentedDragPreview> active = Preview(entity, Vector2d(7.0, 8.0));
  const std::optional<PresentedDragPreview> displayed = Preview(entity, Vector2d(7.0, 8.0));

  EXPECT_EQ(ResolvePresentedTileDragTranslation(tile, active, displayed), Vector2d(1.0, 2.0));
}

TEST(PresentedFrameComposerTest, ComputesTileRectFromDocumentGeometry) {
  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(10.0, 20.0);
  tile.bitmapDimsDoc = Vector2d(30.0, 40.0);
  tile.dragTranslationDoc = Vector2d(2.0, 3.0);

  const Transform2d outputFromCanvasTransform =
      Transform2d::Scale(2.0) * Transform2d::Translate(Vector2d(100.0, 200.0));
  const std::optional<PresentedTileRect> rect =
      ComputePresentedTileRect(tile, outputFromCanvasTransform, std::nullopt, std::nullopt);

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
      ComputePresentedTileRect(tile, outputFromCanvasTransform, std::nullopt, std::nullopt);

  ASSERT_TRUE(rect.has_value());
  EXPECT_EQ(rect->topLeft, Vector2d(12.0, 30.0));
  EXPECT_EQ(rect->bottomRight, Vector2d(26.0, 52.5));
}

TEST(PresentedFrameComposerTest, RejectsInvalidTileGeometry) {
  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(10.0, 20.0);
  tile.bitmapDimsDoc = Vector2d(0.0, 40.0);

  EXPECT_FALSE(ComputePresentedTileRect(tile, Transform2d::Scale(2.0), std::nullopt, std::nullopt)
                   .has_value());

  tile.bitmapDimsDoc = Vector2d(30.0, -1.0);
  EXPECT_FALSE(ComputePresentedTileRect(tile, Transform2d::Scale(2.0), std::nullopt, std::nullopt)
                   .has_value());
}

TEST(PresentedFrameComposerTest, RejectsInvalidOutputTransform) {
  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(10.0, 20.0);
  tile.bitmapDimsDoc = Vector2d(30.0, 40.0);

  EXPECT_FALSE(ComputePresentedTileRect(tile, Transform2d::Scale(0.0), std::nullopt, std::nullopt)
                   .has_value());

  const double infinity = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(ComputePresentedTileRect(tile, Transform2d::Translate(Vector2d(infinity, 0.0)),
                                        std::nullopt, std::nullopt)
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
