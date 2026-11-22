#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>

#include "ImageComparisonTestFixture.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Mask.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia/shaders/Shaders.h"

using namespace tiny_skia;

namespace {

// Helper: create a small triangle pixmap. Matches Rust `crate_triangle`.
// The returned Pixmap must stay alive while any Pattern references it.
Pixmap createTriangle() {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder pb;
  pb.moveTo(0.0f, 20.0f);
  pb.lineTo(20.0f, 20.0f);
  pb.lineTo(10.0f, 0.0f);
  pb.close();
  auto path = pb.finish();
  EXPECT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(20, 20);
  EXPECT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
  return std::move(*pixmap);
}

TEST(PatternTest, NearestSourceOverPreservesOpaqueDestinationAcrossBatches) {
  constexpr int width = 37;
  auto tile = Pixmap::fromSize(width, 1);
  auto destination = Pixmap::fromSize(width, 1);
  ASSERT_THAT(tile, testing::Optional(testing::_));
  ASSERT_THAT(destination, testing::Optional(testing::_));
  // Alternate transparent and half-transparent red texels so the opaque-copy shortcut cannot apply.
  for (int x = 1; x < width; x += 2) {
    tile->data()[x * 4] = 128;
    tile->data()[x * 4 + 3] = 128;
  }
  const auto rect = Rect::fromLTRB(0, 0, width, 1);
  ASSERT_THAT(rect, testing::Optional(testing::_));
  for (const SpreadMode mode : {SpreadMode::Pad, SpreadMode::Repeat}) {
    for (const auto [opacity, red] : {std::pair{1.0f, 128}, std::pair{0.5f, 64},
                                      std::pair{0.19f, 25}}) {
      destination->fill(Color::fromRgba8(0, 255, 0, 255));
      Paint paint;
      paint.antiAlias = false;
      paint.shader = Pattern(tile->view(), mode, FilterQuality::Nearest, opacity,
                             Transform::identity());
      auto view = destination->mutableView();
      Painter::fillRect(view, *rect, paint, Transform::identity());
      for (int x = 0; x < width; ++x) {
        const int alpha = x % 2 ? red : 0;
        EXPECT_EQ(destination->data()[x * 4], alpha) << x;
        EXPECT_EQ(destination->data()[x * 4 + 1], 255 - alpha) << x;
        EXPECT_EQ(destination->data()[x * 4 + 3], 255) << x;
      }
    }
  }
}

TEST(PatternTest, PadNearest) {
  auto triangle = createTriangle();

  Paint paint;
  paint.antiAlias = false;
  paint.shader = Pattern(triangle.view(), SpreadMode::Pad, FilterQuality::Nearest, 1.0f,
                         Transform::identity());

  auto pathRect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
  ASSERT_TRUE(pathRect.has_value());
  auto path = Path::fromRect(*pathRect);

  auto pixmap = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "pattern/pad-nearest.png");
}

TEST(PatternTest, RepeatNearest) {
  auto triangle = createTriangle();

  Paint paint;
  paint.antiAlias = false;
  paint.shader = Pattern(triangle.view(), SpreadMode::Repeat, FilterQuality::Nearest, 1.0f,
                         Transform::identity());

  auto pathRect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
  ASSERT_TRUE(pathRect.has_value());
  auto path = Path::fromRect(*pathRect);

  auto pixmap = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "pattern/repeat-nearest.png");
}

TEST(PatternTest, ReflectNearest) {
  auto triangle = createTriangle();

  Paint paint;
  paint.antiAlias = false;
  paint.shader = Pattern(triangle.view(), SpreadMode::Reflect, FilterQuality::Nearest, 1.0f,
                         Transform::identity());

  auto pathRect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
  ASSERT_TRUE(pathRect.has_value());
  auto path = Path::fromRect(*pathRect);

  auto pixmap = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  // TODO: 29682/40000 pixels differ — investigate pattern reflect rendering.
  EXPECT_GOLDEN_MATCH(*pixmap, "pattern/reflect-nearest.png");
}

TEST(PatternTest, PadBicubic) {
  auto triangle = createTriangle();

  Paint paint;
  paint.antiAlias = false;
  // Rust: Transform::fromRow(1.1, 0.3, 0.0, 1.4, 0.0, 0.0)
  //   = (sx=1.1, kx=0.3, ky=0.0, sy=1.4, tx=0.0, ty=0.0)
  // C++: fromRow(sx, ky, kx, sy, tx, ty)
  paint.shader = Pattern(triangle.view(), SpreadMode::Pad, FilterQuality::Bicubic, 1.0f,
                         Transform::fromRow(1.1f, 0.3f, 0.0f, 1.4f, 0.0f, 0.0f));

  auto pathRect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
  ASSERT_TRUE(pathRect.has_value());
  auto path = Path::fromRect(*pathRect);

  auto pixmap = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "pattern/pad-bicubic.png");
}

TEST(PatternTest, RepeatBicubic) {
  auto triangle = createTriangle();

  Paint paint;
  paint.antiAlias = false;
  // Rust: Transform::fromRow(1.1, 0.3, 0.0, 1.4, 0.0, 0.0)
  paint.shader = Pattern(triangle.view(), SpreadMode::Repeat, FilterQuality::Bicubic, 1.0f,
                         Transform::fromRow(1.1f, 0.3f, 0.0f, 1.4f, 0.0f, 0.0f));

  auto pathRect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
  ASSERT_TRUE(pathRect.has_value());
  auto path = Path::fromRect(*pathRect);

  auto pixmap = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "pattern/repeat-bicubic.png");
}

TEST(PatternTest, ReflectBicubic) {
  auto triangle = createTriangle();

  Paint paint;
  paint.antiAlias = false;
  // Rust: Transform::fromRow(1.1, 0.3, 0.0, 1.4, 0.0, 0.0)
  paint.shader = Pattern(triangle.view(), SpreadMode::Reflect, FilterQuality::Bicubic, 1.0f,
                         Transform::fromRow(1.1f, 0.3f, 0.0f, 1.4f, 0.0f, 0.0f));

  auto pathRect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
  ASSERT_TRUE(pathRect.has_value());
  auto path = Path::fromRect(*pathRect);

  auto pixmap = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "pattern/reflect-bicubic.png");
}

TEST(PatternTest, FilterNearestNoTs) {
  auto triangle = createTriangle();

  Paint paint;
  paint.antiAlias = false;
  paint.shader = Pattern(triangle.view(), SpreadMode::Repeat, FilterQuality::Nearest, 1.0f,
                         Transform::identity());

  auto pathRect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
  ASSERT_TRUE(pathRect.has_value());
  auto path = Path::fromRect(*pathRect);

  auto pixmap = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "pattern/filter-nearest-no-ts.png");
}

TEST(PatternTest, FilterNearest) {
  auto triangle = createTriangle();

  Paint paint;
  paint.antiAlias = false;
  // Rust: Transform::fromRow(1.5, 0.0, -0.4, -0.8, 5.0, 1.0)
  //   = (sx=1.5, kx=0.0, ky=-0.4, sy=-0.8, tx=5.0, ty=1.0)
  // C++: fromRow(sx, ky, kx, sy, tx, ty)
  paint.shader = Pattern(triangle.view(), SpreadMode::Repeat, FilterQuality::Nearest, 1.0f,
                         Transform::fromRow(1.5f, 0.0f, -0.4f, -0.8f, 5.0f, 1.0f));

  auto pathRect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
  ASSERT_TRUE(pathRect.has_value());
  auto path = Path::fromRect(*pathRect);

  auto pixmap = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "pattern/filter-nearest.png");
}

TEST(PatternTest, FilterBilinear) {
  auto triangle = createTriangle();

  Paint paint;
  paint.antiAlias = false;
  // Rust: Transform::fromRow(1.5, 0.0, -0.4, -0.8, 5.0, 1.0)
  paint.shader = Pattern(triangle.view(), SpreadMode::Repeat, FilterQuality::Bilinear, 1.0f,
                         Transform::fromRow(1.5f, 0.0f, -0.4f, -0.8f, 5.0f, 1.0f));

  auto pathRect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
  ASSERT_TRUE(pathRect.has_value());
  auto path = Path::fromRect(*pathRect);

  auto pixmap = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "pattern/filter-bilinear.png");
}

TEST(PatternTest, FilterBicubic) {
  auto triangle = createTriangle();

  Paint paint;
  paint.antiAlias = false;
  // Rust: Transform::fromRow(1.5, 0.0, -0.4, -0.8, 5.0, 1.0)
  paint.shader = Pattern(triangle.view(), SpreadMode::Repeat, FilterQuality::Bicubic, 1.0f,
                         Transform::fromRow(1.5f, 0.0f, -0.4f, -0.8f, 5.0f, 1.0f));

  auto pathRect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
  ASSERT_TRUE(pathRect.has_value());
  auto path = Path::fromRect(*pathRect);

  auto pixmap = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "pattern/filter-bicubic.png");
}

}  // namespace
