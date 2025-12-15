#include "donner/backends/tiny_skia_cpp/Painter.h"

#include <span>
#include <vector>

#include "donner/backends/tiny_skia_cpp/Canvas.h"
#include "donner/backends/tiny_skia_cpp/Color.h"
#include "donner/backends/tiny_skia_cpp/Mask.h"
#include "donner/backends/tiny_skia_cpp/Stroke.h"
#include "gtest/gtest.h"

namespace donner::backends::tiny_skia_cpp {
namespace {

TEST(PainterTests, FillsPathIntoPixmap) {
  auto canvas = Canvas::Create(6, 6);
  ASSERT_TRUE(canvas.hasValue());
  Canvas surface = std::move(canvas.value());
  surface.clear(Color::RGB(0, 0, 0));

  svg::PathSpline spline;
  spline.moveTo({1.0, 1.0});
  spline.lineTo({4.0, 1.0});
  spline.lineTo({4.0, 3.0});
  spline.lineTo({1.0, 3.0});
  spline.closePath();

  Paint paint;
  paint.color = Color::RGB(10, 20, 30);
  auto result = FillPath(spline, paint, surface.pixmap());
  ASSERT_TRUE(result.hasValue()) << result.error();

  const std::vector<uint8_t> expectedRow = {0,  0,  0,  255, 10, 20, 30, 255, 10, 20, 30, 255,
                                            10, 20, 30, 255, 0,  0,  0,  255, 0,  0,  0,  255};

  const std::span<const uint8_t> row1(surface.pixmap().data() + surface.pixmap().strideBytes(),
                                      expectedRow.size());
  const std::span<const uint8_t> row2(surface.pixmap().data() + surface.pixmap().strideBytes() * 2,
                                      expectedRow.size());

  EXPECT_EQ(std::vector<uint8_t>(row1.begin(), row1.end()), expectedRow);
  EXPECT_EQ(std::vector<uint8_t>(row2.begin(), row2.end()), expectedRow);
}

TEST(PainterTests, RejectsInvalidPixmap) {
  Pixmap invalid;
  svg::PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({1.0, 1.0});
  spline.closePath();

  Paint paint;
  auto result = FillPath(spline, paint, invalid);
  EXPECT_FALSE(result.hasValue());
}

TEST(PainterTests, AppliesClipMask) {
  auto canvas = Canvas::Create(4, 2);
  ASSERT_TRUE(canvas.hasValue());
  Canvas surface = std::move(canvas.value());
  surface.clear(Color::RGB(0, 0, 0));

  Mask clip = Mask::Create(4, 2);
  ASSERT_TRUE(clip.isValid());
  clip.clear(0);
  // Enable coverage only on the right half.
  for (int y = 0; y < clip.height(); ++y) {
    uint8_t* row = clip.data() + static_cast<size_t>(y) * clip.strideBytes();
    row[2] = 255;
    row[3] = 255;
  }

  svg::PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({4.0, 0.0});
  spline.lineTo({4.0, 2.0});
  spline.lineTo({0.0, 2.0});
  spline.closePath();

  Paint paint;
  paint.color = Color::RGB(50, 60, 70);
  auto result =
      FillPath(spline, paint, surface.pixmap(), FillRule::kNonZero, Transform(), &clip);
  ASSERT_TRUE(result.hasValue()) << result.error();

  const std::vector<uint8_t> expectedLeft = {0, 0, 0, 255, 0, 0, 0, 255};
  const std::vector<uint8_t> expectedRight = {50, 60, 70, 255, 50, 60, 70, 255};
  const std::span<const uint8_t> row(surface.pixmap().data(),
                                     static_cast<size_t>(surface.pixmap().width()) * 4);

  EXPECT_EQ(std::vector<uint8_t>(row.begin(), row.begin() + 8), expectedLeft);
  EXPECT_EQ(std::vector<uint8_t>(row.begin() + 8, row.end()), expectedRight);
}

TEST(PainterTests, RejectsMismatchedClipMask) {
  auto canvas = Canvas::Create(2, 2);
  ASSERT_TRUE(canvas.hasValue());
  Canvas surface = std::move(canvas.value());

  Mask clip = Mask::Create(1, 1);
  ASSERT_TRUE(clip.isValid());

  svg::PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({1.0, 0.0});
  spline.lineTo({1.0, 1.0});
  spline.closePath();

  Paint paint;
  auto result =
      FillPath(spline, paint, surface.pixmap(), FillRule::kNonZero, Transform(), &clip);
  EXPECT_FALSE(result.hasValue());
}

TEST(PainterTests, StrokesPathIntoPixmap) {
  auto canvas = Canvas::Create(6, 4);
  ASSERT_TRUE(canvas.hasValue());
  Canvas surface = std::move(canvas.value());
  surface.clear(Color::RGB(0, 0, 0));

  svg::PathSpline spline;
  spline.moveTo({1.0, 1.0});
  spline.lineTo({4.0, 1.0});

  Stroke stroke;
  stroke.width = 2.0f;
  stroke.lineCap = LineCap::kSquare;

  Paint paint;
  paint.color = Color::RGB(80, 90, 100);
  auto result = StrokePath(spline, stroke, paint, surface.pixmap());
  ASSERT_TRUE(result.hasValue()) << result.error();

  std::vector<uint8_t> paintedRow;
  for (int i = 0; i < 5; ++i) {
    paintedRow.insert(paintedRow.end(), {80, 90, 100, 255});
  }
  paintedRow.insert(paintedRow.end(), {0, 0, 0, 255});

  std::vector<uint8_t> clearRow;
  for (int i = 0; i < 6; ++i) {
    clearRow.insert(clearRow.end(), {0, 0, 0, 255});
  }

  const std::span<const uint8_t> row0(surface.pixmap().data(), paintedRow.size());
  const std::span<const uint8_t> row1(surface.pixmap().data() + surface.pixmap().strideBytes(),
                                      paintedRow.size());
  const std::span<const uint8_t> row2(surface.pixmap().data() + surface.pixmap().strideBytes() * 2,
                                      clearRow.size());
  const std::span<const uint8_t> row3(surface.pixmap().data() + surface.pixmap().strideBytes() * 3,
                                      clearRow.size());

  EXPECT_EQ(std::vector<uint8_t>(row0.begin(), row0.end()), paintedRow);
  EXPECT_EQ(std::vector<uint8_t>(row1.begin(), row1.end()), paintedRow);
  EXPECT_EQ(std::vector<uint8_t>(row2.begin(), row2.end()), clearRow);
  EXPECT_EQ(std::vector<uint8_t>(row3.begin(), row3.end()), clearRow);
}

TEST(PainterTests, AppliesClipMaskToStroke) {
  auto canvas = Canvas::Create(6, 4);
  ASSERT_TRUE(canvas.hasValue());
  Canvas surface = std::move(canvas.value());
  surface.clear(Color::RGB(0, 0, 0));

  Mask clip = Mask::Create(6, 4);
  ASSERT_TRUE(clip.isValid());
  clip.clear(0);
  for (int y = 0; y < clip.height(); ++y) {
    uint8_t* row = clip.data() + static_cast<size_t>(y) * clip.strideBytes();
    row[2] = 255;
    row[3] = 255;
    row[4] = 255;
  }

  svg::PathSpline spline;
  spline.moveTo({1.0, 1.0});
  spline.lineTo({4.0, 1.0});

  Stroke stroke;
  stroke.width = 2.0f;
  stroke.lineCap = LineCap::kSquare;

  Paint paint;
  paint.color = Color::RGB(120, 130, 140);
  auto result = StrokePath(spline, stroke, paint, surface.pixmap(), Transform(), &clip);
  ASSERT_TRUE(result.hasValue()) << result.error();

  std::vector<uint8_t> clippedRow;
  clippedRow.insert(clippedRow.end(), {0, 0, 0, 255, 0, 0, 0, 255});
  clippedRow.insert(clippedRow.end(), {120, 130, 140, 255, 120, 130, 140, 255, 120, 130, 140, 255});
  clippedRow.insert(clippedRow.end(), {0, 0, 0, 255});

  std::vector<uint8_t> clearRow;
  for (int i = 0; i < 6; ++i) {
    clearRow.insert(clearRow.end(), {0, 0, 0, 255});
  }

  const std::span<const uint8_t> row0(surface.pixmap().data(), clippedRow.size());
  const std::span<const uint8_t> row2(surface.pixmap().data() + surface.pixmap().strideBytes() * 2,
                                      clearRow.size());

  EXPECT_EQ(std::vector<uint8_t>(row0.begin(), row0.end()), clippedRow);
  EXPECT_EQ(std::vector<uint8_t>(row2.begin(), row2.end()), clearRow);
}

}  // namespace
}  // namespace donner::backends::tiny_skia_cpp
