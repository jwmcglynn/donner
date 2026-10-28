#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "tiny_skia/Mask.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"

using ::testing::Optional;

TEST(PathTest, ConvexClassificationRejectsRepeatedContour) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  for (int lap = 0; lap < 2; ++lap) {
    builder.lineTo(8.0f, 2.0f);
    builder.lineTo(8.0f, 8.0f);
    builder.lineTo(2.0f, 8.0f);
    if (lap == 0) {
      builder.lineTo(2.0f, 2.0f);
    }
  }
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  EXPECT_THAT(path->isConvex(), testing::IsFalse());
  const auto rectangle = tiny_skia::Rect::fromLTRB(2.0f, 2.0f, 8.0f, 8.0f);
  ASSERT_THAT(rectangle, Optional(testing::_));
  EXPECT_THAT(tiny_skia::Path::fromRect(*rectangle).isConvex(), testing::IsTrue());
}

TEST(MaskTest, RepeatedContourRespectsWindingRule) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  for (int lap = 0; lap < 2; ++lap) {
    builder.lineTo(8.0f, 2.0f);
    builder.lineTo(8.0f, 8.0f);
    builder.lineTo(2.0f, 8.0f);
    if (lap == 0) {
      builder.lineTo(2.0f, 2.0f);
    }
  }
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  for (const bool antiAlias : {false, true}) {
    for (const auto rule : {tiny_skia::FillRule::Winding, tiny_skia::FillRule::EvenOdd}) {
      SCOPED_TRACE(antiAlias);
      SCOPED_TRACE(static_cast<int>(rule));
      auto mask = tiny_skia::Mask::fromSize(10, 10);
      ASSERT_THAT(mask, Optional(testing::_));
      mask->fillPath(*path, rule, antiAlias, tiny_skia::Transform::identity());
      std::vector<std::uint8_t> expected(100, 0);
      if (rule == tiny_skia::FillRule::Winding) {
        for (std::size_t y = 2; y < 8; ++y) {
          for (std::size_t x = 2; x < 8; ++x) {
            expected[y * 10 + x] = 255;
          }
        }
      }
      EXPECT_THAT(mask->data(), testing::ElementsAreArray(expected));
    }
  }
}
