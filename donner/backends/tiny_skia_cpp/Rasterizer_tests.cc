#include "donner/backends/tiny_skia_cpp/Rasterizer.h"

#include <algorithm>
#include <span>
#include <vector>

#include "gtest/gtest.h"

namespace donner::backends::tiny_skia_cpp {
namespace {

TEST(RasterizerTests, FillsRectangle) {
  svg::PathSpline spline;
  spline.moveTo({1.0, 1.0});
  spline.lineTo({4.0, 1.0});
  spline.lineTo({4.0, 3.0});
  spline.lineTo({1.0, 3.0});
  spline.closePath();

  Mask mask = RasterizeFill(spline, 6, 6);
  ASSERT_TRUE(mask.isValid());

  const std::vector<uint8_t> expectedRow = {0, 255, 255, 255, 0, 0};
  const std::span<const uint8_t> row1(mask.data() + mask.strideBytes(), expectedRow.size());
  const std::span<const uint8_t> row2(mask.data() + mask.strideBytes() * 2, expectedRow.size());

  EXPECT_EQ(std::vector<uint8_t>(row1.begin(), row1.end()), expectedRow);
  EXPECT_EQ(std::vector<uint8_t>(row2.begin(), row2.end()), expectedRow);
}

TEST(RasterizerTests, IgnoresHorizontalEdges) {
  svg::PathSpline spline;
  spline.moveTo({0.0, 2.0});
  spline.lineTo({5.0, 2.0});
  spline.lineTo({5.0, 4.0});
  spline.lineTo({0.0, 4.0});
  spline.closePath();

  Mask mask = RasterizeFill(spline, 6, 6);
  ASSERT_TRUE(mask.isValid());

  // Scanline y=1 should remain empty because the shape starts at y=2.
  const std::span<const uint8_t> row(mask.data() + mask.strideBytes(),
                                     static_cast<size_t>(mask.width()));
  EXPECT_TRUE(std::all_of(row.begin(), row.end(), [](uint8_t value) { return value == 0; }));
}

TEST(RasterizerTests, ProducesAntialiasedCoverage) {
  svg::PathSpline spline;
  spline.moveTo({0.25, 0.25});
  spline.lineTo({2.25, 0.25});
  spline.lineTo({2.25, 2.25});
  spline.lineTo({0.25, 2.25});
  spline.closePath();

  Mask mask = RasterizeFill(spline, 3, 3, FillRule::kNonZero, true);
  ASSERT_TRUE(mask.isValid());

  const std::vector<uint8_t> expectedRow = {191, 255, 64};
  const std::span<const uint8_t> row0(mask.data(), expectedRow.size());
  const std::span<const uint8_t> row1(mask.data() + mask.strideBytes(), expectedRow.size());

  EXPECT_EQ(std::vector<uint8_t>(row0.begin(), row0.end()), expectedRow);
  EXPECT_EQ(std::vector<uint8_t>(row1.begin(), row1.end()), expectedRow);
}

TEST(RasterizerTests, DisablesAntialiasingWhenRequested) {
  svg::PathSpline spline;
  spline.moveTo({0.25, 0.25});
  spline.lineTo({2.25, 0.25});
  spline.lineTo({2.25, 2.25});
  spline.lineTo({0.25, 2.25});
  spline.closePath();

  Mask mask = RasterizeFill(spline, 3, 3, FillRule::kNonZero, false);
  ASSERT_TRUE(mask.isValid());

  const std::vector<uint8_t> expectedRow = {0, 255, 255};
  const std::span<const uint8_t> row0(mask.data(), expectedRow.size());
  const std::span<const uint8_t> row1(mask.data() + mask.strideBytes(), expectedRow.size());

  EXPECT_EQ(std::vector<uint8_t>(row0.begin(), row0.end()), expectedRow);
  EXPECT_EQ(std::vector<uint8_t>(row1.begin(), row1.end()), expectedRow);
}

TEST(RasterizerTests, AppliesEvenOddFillRule) {
  svg::PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({4.0, 0.0});
  spline.lineTo({4.0, 4.0});
  spline.lineTo({0.0, 4.0});
  spline.closePath();

  spline.moveTo({1.0, 1.0});
  spline.lineTo({3.0, 1.0});
  spline.lineTo({3.0, 3.0});
  spline.lineTo({1.0, 3.0});
  spline.closePath();

  Mask nonZero = RasterizeFill(spline, 5, 5, FillRule::kNonZero);
  Mask evenOdd = RasterizeFill(spline, 5, 5, FillRule::kEvenOdd);

  ASSERT_TRUE(nonZero.isValid());
  ASSERT_TRUE(evenOdd.isValid());

  const size_t centerIndex = static_cast<size_t>(2 * evenOdd.strideBytes() + 2);
  EXPECT_GT(nonZero.data()[centerIndex], 0);
  EXPECT_EQ(evenOdd.data()[centerIndex], 0);
}

TEST(RasterizerTests, AppliesTransformBeforeRasterizing) {
  svg::PathSpline spline;
  spline.moveTo({0.25, 0.25});
  spline.lineTo({2.25, 0.25});
  spline.lineTo({2.25, 2.25});
  spline.lineTo({0.25, 2.25});
  spline.closePath();

  Mask baseline = RasterizeFill(spline, 5, 3, FillRule::kNonZero, true, Transform());
  const Transform transform = Transform::Translate({1.0, 0.0});
  Mask shifted = RasterizeFill(spline, 5, 3, FillRule::kNonZero, true, transform);

  ASSERT_TRUE(baseline.isValid());
  ASSERT_TRUE(shifted.isValid());

  for (int y = 0; y < baseline.height(); ++y) {
    const std::span<const uint8_t> baseRow(baseline.data() + static_cast<size_t>(y) *
                                                        baseline.strideBytes(),
                                           static_cast<size_t>(baseline.width()));
    const std::span<const uint8_t> shiftedRow(shifted.data() + static_cast<size_t>(y) *
                                                          shifted.strideBytes(),
                                              static_cast<size_t>(shifted.width()));

    EXPECT_EQ(std::vector<uint8_t>(baseRow.begin(), baseRow.end() - 1),
              std::vector<uint8_t>(shiftedRow.begin() + 1, shiftedRow.end()));
    EXPECT_EQ(shiftedRow.front(), 0);
  }
}

}  // namespace
}  // namespace donner::backends::tiny_skia_cpp
