#include "donner/editor/SelectionTransformHandles.h"

#include <array>

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

}  // namespace
}  // namespace donner::editor
