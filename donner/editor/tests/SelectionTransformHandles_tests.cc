#include "donner/editor/SelectionTransformHandles.h"

#include <array>
#include <limits>

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

TEST(SelectionTransformHandlesTest, HitTestFindsResizeCorner) {
  const std::array<Box2d, 1> bounds{Box2d::FromXYWH(20.0, 30.0, 80.0, 40.0)};

  const SelectionTransformHandleIntent intent =
      HitTestSelectionTransformHandles(bounds, Vector2d(20.0, 30.0), /*pixelsPerDocUnit=*/1.0);

  EXPECT_EQ(intent.kind, SelectionTransformHandleKind::Resize);
  EXPECT_EQ(intent.corner, SelectionTransformCorner::TopLeft);
}

TEST(SelectionTransformHandlesTest, HitTestFindsRotateRingOutsideResizeHandle) {
  const std::array<Box2d, 1> bounds{Box2d::FromXYWH(20.0, 30.0, 80.0, 40.0)};

  const SelectionTransformHandleIntent intent =
      HitTestSelectionTransformHandles(bounds, Vector2d(20.0, 16.0), /*pixelsPerDocUnit=*/1.0);

  EXPECT_EQ(intent.kind, SelectionTransformHandleKind::Rotate);
  EXPECT_EQ(intent.corner, SelectionTransformCorner::TopLeft);
}

TEST(SelectionTransformHandlesTest, HitTestCanSuppressRotateWithoutSuppressingResize) {
  const std::array<Box2d, 1> bounds{Box2d::FromXYWH(20.0, 30.0, 80.0, 40.0)};

  EXPECT_EQ(HitTestSelectionTransformHandles(bounds, Vector2d(20.0, 16.0),
                                             /*pixelsPerDocUnit=*/1.0,
                                             /*includeRotate=*/false)
                .kind,
            SelectionTransformHandleKind::None);
  EXPECT_EQ(HitTestSelectionTransformHandles(bounds, Vector2d(20.0, 30.0),
                                             /*pixelsPerDocUnit=*/1.0,
                                             /*includeRotate=*/false)
                .kind,
            SelectionTransformHandleKind::Resize);
}

TEST(SelectionTransformHandlesTest, ResizeTakesPriorityOverRotate) {
  const std::array<Box2d, 1> bounds{Box2d::FromXYWH(20.0, 30.0, 80.0, 40.0)};

  const SelectionTransformHandleIntent intent =
      HitTestSelectionTransformHandles(bounds, Vector2d(22.0, 32.0), /*pixelsPerDocUnit=*/1.0);

  EXPECT_EQ(intent.kind, SelectionTransformHandleKind::Resize);
  EXPECT_EQ(intent.corner, SelectionTransformCorner::TopLeft);
}

TEST(SelectionTransformHandlesTest, HitSizeStaysStableAcrossZoom) {
  const std::array<Box2d, 1> bounds{Box2d::FromXYWH(20.0, 30.0, 80.0, 40.0)};

  EXPECT_EQ(HitTestSelectionTransformHandles(bounds, Vector2d(21.0, 31.0),
                                             /*pixelsPerDocUnit=*/4.0)
                .kind,
            SelectionTransformHandleKind::Resize);
  EXPECT_EQ(HitTestSelectionTransformHandles(bounds, Vector2d(27.0, 37.0),
                                             /*pixelsPerDocUnit=*/4.0)
                .kind,
            SelectionTransformHandleKind::None);
}

TEST(SelectionTransformHandlesTest, CombinesMultiSelectionBounds) {
  const std::array<Box2d, 2> bounds{Box2d::FromXYWH(20.0, 30.0, 20.0, 20.0),
                                    Box2d::FromXYWH(100.0, 110.0, 30.0, 40.0)};

  const Box2d combined = CombinedSelectionBounds(bounds);

  EXPECT_EQ(combined, Box2d::FromXYWH(20.0, 30.0, 110.0, 120.0));
}

TEST(SelectionTransformHandlesTest, EmptyAndDegenerateInputsReturnNoIntent) {
  const std::array<Box2d, 0> emptyBounds{};
  EXPECT_EQ(HitTestSelectionTransformHandles(emptyBounds, Vector2d(0.0, 0.0),
                                             /*pixelsPerDocUnit=*/1.0)
                .kind,
            SelectionTransformHandleKind::None);

  const std::array<Box2d, 1> bounds{Box2d::FromXYWH(20.0, 30.0, 80.0, 40.0)};
  EXPECT_EQ(HitTestSelectionTransformHandles(
                bounds, Vector2d(std::numeric_limits<double>::infinity(), 30.0),
                /*pixelsPerDocUnit=*/1.0)
                .kind,
            SelectionTransformHandleKind::None);
  EXPECT_EQ(HitTestSelectionTransformHandles(bounds, Vector2d(20.0, 30.0),
                                             /*pixelsPerDocUnit=*/0.0)
                .kind,
            SelectionTransformHandleKind::None);
  EXPECT_EQ(HitTestSelectionTransformHandles(bounds, Vector2d(20.0, 30.0),
                                             /*pixelsPerDocUnit=*/
                                             std::numeric_limits<double>::quiet_NaN())
                .kind,
            SelectionTransformHandleKind::None);
  const std::array<Box2d, 1> emptyBoxBounds{Box2d()};
  EXPECT_EQ(HitTestSelectionTransformHandles(emptyBoxBounds, Vector2d(0.0, 0.0),
                                             /*pixelsPerDocUnit=*/1.0)
                .kind,
            SelectionTransformHandleKind::None);
}

TEST(SelectionTransformHandlesTest, CornerHelpersCoverAllCorners) {
  const Box2d bounds = Box2d::FromXYWH(10.0, 20.0, 30.0, 40.0);

  EXPECT_EQ(SelectionTransformCornerPoint(bounds, SelectionTransformCorner::TopLeft),
            Vector2d(10.0, 20.0));
  EXPECT_EQ(SelectionTransformCornerPoint(bounds, SelectionTransformCorner::TopRight),
            Vector2d(40.0, 20.0));
  EXPECT_EQ(SelectionTransformCornerPoint(bounds, SelectionTransformCorner::BottomRight),
            Vector2d(40.0, 60.0));
  EXPECT_EQ(SelectionTransformCornerPoint(bounds, SelectionTransformCorner::BottomLeft),
            Vector2d(10.0, 60.0));

  EXPECT_EQ(OppositeSelectionTransformCorner(SelectionTransformCorner::TopLeft),
            SelectionTransformCorner::BottomRight);
  EXPECT_EQ(OppositeSelectionTransformCorner(SelectionTransformCorner::TopRight),
            SelectionTransformCorner::BottomLeft);
  EXPECT_EQ(OppositeSelectionTransformCorner(SelectionTransformCorner::BottomRight),
            SelectionTransformCorner::TopLeft);
  EXPECT_EQ(OppositeSelectionTransformCorner(SelectionTransformCorner::BottomLeft),
            SelectionTransformCorner::TopRight);
}

TEST(SelectionTransformHandlesTest, RotateRingRejectsInsideBoundsAndOutsideOuterRadius) {
  const std::array<Box2d, 1> bounds{Box2d::FromXYWH(20.0, 30.0, 80.0, 40.0)};

  EXPECT_EQ(HitTestSelectionTransformHandles(bounds, Vector2d(50.0, 50.0),
                                             /*pixelsPerDocUnit=*/1.0)
                .kind,
            SelectionTransformHandleKind::None);
  EXPECT_EQ(HitTestSelectionTransformHandles(bounds, Vector2d(20.0, 4.0),
                                             /*pixelsPerDocUnit=*/1.0)
                .kind,
            SelectionTransformHandleKind::None);
}

}  // namespace
}  // namespace donner::editor
