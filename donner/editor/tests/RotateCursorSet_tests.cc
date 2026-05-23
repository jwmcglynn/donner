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

TEST(RotateCursorSetTest, RotatedCornersProduceDifferentBitmaps) {
  std::optional<RotateCursorImage> topLeft =
      RenderRotateCursorImage(SelectionTransformCorner::TopLeft, nullptr);
  std::optional<RotateCursorImage> topRight =
      RenderRotateCursorImage(SelectionTransformCorner::TopRight, nullptr);

  ASSERT_TRUE(topLeft.has_value());
  ASSERT_TRUE(topRight.has_value());
  EXPECT_NE(topLeft->rgba, topRight->rgba);
}

}  // namespace
}  // namespace donner::editor
