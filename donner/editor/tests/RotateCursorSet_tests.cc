#include "donner/editor/RotateCursorSet.h"

#include <cstddef>
#include <optional>

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

std::size_t CountNonTransparentPixels(const RotateCursorImage& image) {
  std::size_t count = 0;
  for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
    if (image.rgba[i] != 0) {
      ++count;
    }
  }
  return count;
}

std::size_t CountOpaqueBlackPixels(const RotateCursorImage& image) {
  std::size_t count = 0;
  for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
    if (image.rgba[i + 3] > 180 && image.rgba[i] < 40 && image.rgba[i + 1] < 40 &&
        image.rgba[i + 2] < 40) {
      ++count;
    }
  }
  return count;
}

std::size_t CountOpaqueWhitePixels(const RotateCursorImage& image) {
  std::size_t count = 0;
  for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
    if (image.rgba[i + 3] > 180 && image.rgba[i] > 220 && image.rgba[i + 1] > 220 &&
        image.rgba[i + 2] > 220) {
      ++count;
    }
  }
  return count;
}

TEST(RotateCursorSetTest, RendersSvgCursorImagesForAllCorners) {
  for (SelectionTransformCorner corner :
       {SelectionTransformCorner::TopLeft, SelectionTransformCorner::TopRight,
        SelectionTransformCorner::BottomRight, SelectionTransformCorner::BottomLeft}) {
    std::optional<RotateCursorImage> image = RenderRotateCursorImage(corner, nullptr);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->width, 32);
    EXPECT_EQ(image->height, 32);
    EXPECT_EQ(image->rgba.size(), 32u * 32u * 4u);
    EXPECT_GT(CountNonTransparentPixels(*image), 80u);
    EXPECT_LT(CountNonTransparentPixels(*image), 32u * 32u);
  }
}

TEST(RotateCursorSetTest, RotateCursorUsesBlackGlyphWithWhiteOutline) {
  std::optional<RotateCursorImage> image =
      RenderRotateCursorImage(SelectionTransformCorner::TopLeft, nullptr);

  ASSERT_TRUE(image.has_value());
  EXPECT_GT(CountOpaqueBlackPixels(*image), 30u);
  EXPECT_GT(CountOpaqueWhitePixels(*image), 10u);
}

TEST(RotateCursorSetTest, RendersPanCursorImage) {
  for (PanCursorKind kind : {PanCursorKind::OpenHand, PanCursorKind::ClosedHand}) {
    std::optional<RotateCursorImage> image = RenderPanCursorImage(kind, nullptr);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->width, 32);
    EXPECT_EQ(image->height, 32);
    EXPECT_EQ(image->rgba.size(), 32u * 32u * 4u);
    EXPECT_GT(CountNonTransparentPixels(*image), 100u);
    EXPECT_LT(CountNonTransparentPixels(*image), 32u * 32u);
  }
}

TEST(RotateCursorSetTest, PenCursorUsesBlackGlyphWithWhiteOutline) {
  std::optional<RotateCursorImage> image = RenderPenCursorImage(nullptr);

  ASSERT_TRUE(image.has_value());
  EXPECT_EQ(image->width, 32);
  EXPECT_EQ(image->height, 32);
  EXPECT_EQ(image->rgba.size(), 32u * 32u * 4u);
  EXPECT_GT(CountNonTransparentPixels(*image), 90u);
  EXPECT_LT(CountNonTransparentPixels(*image), 32u * 32u);
  EXPECT_GT(CountOpaqueBlackPixels(*image), 25u);
  EXPECT_GT(CountOpaqueWhitePixels(*image), 8u);
}

TEST(RotateCursorSetTest, OpenAndClosedPanCursorsProduceDifferentBitmaps) {
  std::optional<RotateCursorImage> openHand =
      RenderPanCursorImage(PanCursorKind::OpenHand, nullptr);
  std::optional<RotateCursorImage> closedHand =
      RenderPanCursorImage(PanCursorKind::ClosedHand, nullptr);

  ASSERT_TRUE(openHand.has_value());
  ASSERT_TRUE(closedHand.has_value());
  EXPECT_NE(openHand->rgba, closedHand->rgba);
}

TEST(RotateCursorSetTest, RotatedCornersProduceDifferentBitmaps) {
  std::optional<RotateCursorImage> topLeft =
      RenderRotateCursorImage(SelectionTransformCorner::TopLeft, nullptr);
  std::optional<RotateCursorImage> topRight =
      RenderRotateCursorImage(SelectionTransformCorner::TopRight, nullptr);

  ASSERT_TRUE(topLeft.has_value());
  ASSERT_TRUE(topRight.has_value());
  EXPECT_NE(topLeft->rgba, topRight->rgba);
}

TEST(RotateCursorSetTest, UninitializedCursorSetRejectsCursorChanges) {
  RotateCursorSet cursorSet;

  EXPECT_FALSE(cursorSet.valid());
  EXPECT_FALSE(cursorSet.setRotateCursor(SelectionTransformCorner::TopLeft));
  EXPECT_FALSE(cursorSet.setRotateCursor(SelectionTransformCorner::BottomRight));
  EXPECT_FALSE(cursorSet.setPanCursor(PanCursorKind::OpenHand));
  EXPECT_FALSE(cursorSet.setPanCursor(PanCursorKind::ClosedHand));
  EXPECT_FALSE(cursorSet.setPenCursor());

  cursorSet.clearIfActive();
  EXPECT_FALSE(cursorSet.valid());
}

TEST(RotateCursorSetTest, InitializeWithNullWindowFailsAndLeavesSetInvalid) {
  RotateCursorSet cursorSet;

  EXPECT_FALSE(cursorSet.initialize(nullptr, nullptr));
  EXPECT_FALSE(cursorSet.valid());
  EXPECT_FALSE(cursorSet.setRotateCursor(SelectionTransformCorner::TopRight));
  EXPECT_FALSE(cursorSet.setPanCursor(PanCursorKind::ClosedHand));
  EXPECT_FALSE(cursorSet.setPenCursor());
}

}  // namespace
}  // namespace donner::editor
