#include "donner/editor/PresentedFrameComposer.h"

#include <array>
#include <limits>
#include <optional>
#include <vector>

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

TEST(PresentedFrameComposerTest, ComputesTileQuadFromAffineActivePreview) {
  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(0.0, 0.0);
  tile.bitmapDimsDoc = Vector2d(10.0, 10.0);
  tile.isDragTarget = true;

  const std::optional<PresentedDragBaseline> baseline = PresentedDragBaseline{
      .entity = Entity{123},
      .representedDocumentFromCachedDocument = Transform2d(),
      .activeDocumentFromCachedDocument = Transform2d::Scale(2.0),
  };
  const std::optional<PresentedTileQuad> quad =
      ComputePresentedTileQuad(tile, Transform2d(), baseline);

  ASSERT_TRUE(quad.has_value());
  EXPECT_EQ(quad->topLeft, Vector2d(0.0, 0.0));
  EXPECT_EQ(quad->topRight, Vector2d(20.0, 0.0));
  EXPECT_EQ(quad->bottomRight, Vector2d(20.0, 20.0));
  EXPECT_EQ(quad->bottomLeft, Vector2d(0.0, 20.0));
}

TEST(PresentedFrameComposerTest, RepresentedAffineTransformDoesNotDoubleApply) {
  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(0.0, 0.0);
  tile.bitmapDimsDoc = Vector2d(10.0, 10.0);
  tile.documentFromCachedDocument = Transform2d::Scale(2.0);
  tile.isDragTarget = true;

  const std::optional<PresentedDragBaseline> baseline = PresentedDragBaseline{
      .entity = Entity{123},
      .representedDocumentFromCachedDocument = Transform2d::Scale(2.0),
      .activeDocumentFromCachedDocument = Transform2d::Scale(2.0),
  };
  const std::optional<PresentedTileQuad> quad =
      ComputePresentedTileQuad(tile, Transform2d(), baseline);

  ASSERT_TRUE(quad.has_value());
  EXPECT_EQ(quad->bottomRight, Vector2d(20.0, 20.0));
}

// A re-captured (baked) drag tile carries its content at the `represented`
// transform with `documentFromCachedDocument == identity` (the worker bakes the
// transform into the pixels and resets canvasFromBitmap). To present that bitmap
// at the live `active` transform, applying the returned transform to represented
// points must land them on the same points transformed by `active`.
TEST(PresentedFrameComposerTest, BakedTileRebasesOntoActiveForNonCommutingDrag) {
  // Rotated non-uniform scales: represented and active do NOT commute.
  const Transform2d representedDocumentFromStartDocument =
      Transform2d::Scale(Vector2d(2.0, 1.0)) * Transform2d::Rotate(0.3);
  const Transform2d activeDocumentFromStartDocument =
      Transform2d::Scale(Vector2d(2.5, 1.0)) * Transform2d::Rotate(0.6);

  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(0.0, 0.0);
  tile.bitmapDimsDoc = Vector2d(10.0, 10.0);
  tile.documentFromCachedDocument = Transform2d();  // identity: a fresh bake
  tile.isDragTarget = true;

  const std::optional<PresentedDragBaseline> baseline = PresentedDragBaseline{
      .entity = Entity{123},
      .representedDocumentFromCachedDocument = representedDocumentFromStartDocument,
      .activeDocumentFromCachedDocument = activeDocumentFromStartDocument,
  };

  const Transform2d activeDocumentFromRepresentedDocument =
      ResolvePresentedTileDocumentTransform(PresentedFrameTileGeometry{tile}, baseline);

  for (const Vector2d point : {Vector2d(0.0, 0.0), Vector2d(10.0, 4.0), Vector2d(6.0, 12.0)}) {
    const Vector2d presented = activeDocumentFromRepresentedDocument.transformPosition(
        representedDocumentFromStartDocument.transformPosition(point));
    const Vector2d live = activeDocumentFromStartDocument.transformPosition(point);
    EXPECT_NEAR(presented.x, live.x, 1e-9) << "x mismatch at " << point;
    EXPECT_NEAR(presented.y, live.y, 1e-9) << "y mismatch at " << point;
  }
}

TEST(PresentedFrameComposerTest, BakedTileTransformsRepresentedPointsOntoActiveDrag) {
  const Transform2d representedDocumentFromStartDocument =
      Transform2d::Translate(Vector2d(10.0, 5.0)) * Transform2d::Scale(Vector2d(1.5, 0.8)) *
      Transform2d::Rotate(0.25);
  const Transform2d activeDocumentFromStartDocument = Transform2d::Translate(Vector2d(18.0, -3.0)) *
                                                      Transform2d::Scale(Vector2d(2.2, 1.1)) *
                                                      Transform2d::Rotate(0.6);

  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(0.0, 0.0);
  tile.bitmapDimsDoc = Vector2d(10.0, 10.0);
  tile.documentFromCachedDocument = Transform2d();
  tile.isDragTarget = true;

  const std::optional<PresentedDragBaseline> baseline = PresentedDragBaseline{
      .entity = Entity{123},
      .representedDocumentFromCachedDocument = representedDocumentFromStartDocument,
      .activeDocumentFromCachedDocument = activeDocumentFromStartDocument,
  };

  const Transform2d activeDocumentFromRepresentedDocument =
      ResolvePresentedTileDocumentTransform(tile, baseline);
  for (const Vector2d point : {Vector2d(0.0, 0.0), Vector2d(10.0, 4.0), Vector2d(6.0, 12.0)}) {
    const Vector2d presented = activeDocumentFromRepresentedDocument.transformPosition(
        representedDocumentFromStartDocument.transformPosition(point));
    const Vector2d live = activeDocumentFromStartDocument.transformPosition(point);
    EXPECT_NEAR(presented.x, live.x, 1e-9) << "x mismatch at " << point;
    EXPECT_NEAR(presented.y, live.y, 1e-9) << "y mismatch at " << point;
  }
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

  const double infinity = std::numeric_limits<double>::infinity();
  tile.bitmapDimsDoc = Vector2d(30.0, 40.0);
  tile.canvasOffsetDoc = Vector2d(10.0, infinity);
  EXPECT_FALSE(ComputePresentedTileRect(tile, Transform2d::Scale(2.0), std::nullopt).has_value());

  tile.canvasOffsetDoc = Vector2d(10.0, 20.0);
  tile.dragTranslationDoc = Vector2d(infinity, 0.0);
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

TEST(PresentedFrameComposerTest, RejectsInvalidEffectiveDragBaseline) {
  const double infinity = std::numeric_limits<double>::infinity();
  PresentedFrameTileGeometry tile;
  tile.canvasOffsetDoc = Vector2d(0.0, 0.0);
  tile.bitmapDimsDoc = Vector2d(10.0, 10.0);
  tile.isDragTarget = true;

  std::optional<PresentedDragBaseline> baseline = PresentedDragBaseline{
      .entity = Entity{123},
      .representedTranslationDoc = Vector2d::Zero(),
      .activeTranslationDoc = Vector2d(infinity, 0.0),
  };
  EXPECT_FALSE(ComputePresentedTileQuad(tile, Transform2d(), baseline).has_value());

  baseline->activeTranslationDoc = Vector2d::Zero();
  baseline->activeDocumentFromCachedDocument = Transform2d::Translate(Vector2d(0.0, infinity));
  EXPECT_FALSE(ComputePresentedTileQuad(tile, Transform2d(), baseline).has_value());
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

TEST(PresentedFrameComposerTest, RejectsDegeneratePresentedPixelRects) {
  EXPECT_FALSE(RoundPresentedTileRectToPixelRect(PresentedTileRect{
                                                     .topLeft = Vector2d(10.0, 10.0),
                                                     .bottomRight = Vector2d(10.0, 20.0),
                                                 })
                   .has_value());
  EXPECT_FALSE(RoundPresentedTileRectToPixelRect(PresentedTileRect{
                                                     .topLeft = Vector2d(10.0, 10.0),
                                                     .bottomRight = Vector2d(20.0, 10.0),
                                                 })
                   .has_value());
  EXPECT_FALSE(RoundPresentedTileRectToPixelRect(PresentedTileRect{
                                                     .topLeft = Vector2d(10.0, 10.0),
                                                     .bottomRight = Vector2d(10.2, 10.2),
                                                 })
                   .has_value());
}

TEST(PresentedFrameComposerTest, SubtractsCoveredTileBoundsFromOverviewClip) {
  const Box2d clip = Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0);
  const std::array<Box2d, 1> covered{Box2d::FromXYWH(25.0, 25.0, 50.0, 50.0)};

  const std::vector<Box2d> uncovered = SubtractPresentedTileBoundsFromClip(clip, covered);

  const std::vector<Box2d> expected{
      Box2d::FromXYWH(0.0, 0.0, 100.0, 25.0),
      Box2d::FromXYWH(0.0, 75.0, 100.0, 25.0),
      Box2d::FromXYWH(0.0, 25.0, 25.0, 50.0),
      Box2d::FromXYWH(75.0, 25.0, 25.0, 50.0),
  };
  EXPECT_EQ(uncovered, expected);
}

TEST(PresentedFrameComposerTest, SubtractIgnoresInvalidAndNonIntersectingCoveredRects) {
  const Box2d clip = Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0);
  const std::array<Box2d, 3> covered{
      Box2d::FromXYWH(150.0, 150.0, 10.0, 10.0),
      Box2d(Vector2d(25.0, 25.0), Vector2d(25.0, 50.0)),
      Box2d::FromXYWH(10.0, 10.0, 20.0, 20.0),
  };

  const std::vector<Box2d> uncovered = SubtractPresentedTileBoundsFromClip(clip, covered);

  EXPECT_EQ(uncovered.size(), 4u);
}

TEST(PresentedFrameComposerTest, SubtractReturnsEmptyForInvalidClipOrFullCoverage) {
  EXPECT_TRUE(SubtractPresentedTileBoundsFromClip(Box2d(Vector2d(0.0, 0.0), Vector2d(0.0, 10.0)),
                                                  std::span<const Box2d>())
                  .empty());

  const Box2d clip = Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0);
  const std::array<Box2d, 1> covered{clip};
  EXPECT_TRUE(SubtractPresentedTileBoundsFromClip(clip, covered).empty());
}

}  // namespace
}  // namespace donner::editor
