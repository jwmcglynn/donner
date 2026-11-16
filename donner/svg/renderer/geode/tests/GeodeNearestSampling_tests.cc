#include "donner/svg/renderer/geode/GeodeNearestSampling.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

#include "tiny_skia/Painter.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Transform.h"

namespace donner::geode {
namespace {

/// A target-from-texel transform with its description for failure messages.
struct Placement {
  Transform2d targetFromTexel;
  const char* description;
};

tiny_skia::Transform ToTinyTransform(const Transform2d& transform) {
  return tiny_skia::Transform::fromRow(
      static_cast<float>(transform.data[0]), static_cast<float>(transform.data[1]),
      static_cast<float>(transform.data[2]), static_cast<float>(transform.data[3]),
      static_cast<float>(transform.data[4]), static_cast<float>(transform.data[5]));
}

/// Scale-and-translate placements of a \p texels wide image: every quarter-pixel width up to 100
/// at origins that put texel boundaries on pixel centers.
std::vector<Placement> AxisAlignedPlacements(int texels) {
  std::vector<Placement> placements;
  for (const double origin : {0.0, 8.0, 10.5, 1003.0}) {
    for (int quarterWidth = 1; quarterWidth <= 400; ++quarterWidth) {
      const double width = quarterWidth / 4.0;
      placements.push_back(
          {Transform2d::Scale(width / texels, 1.0) * Transform2d::Translate(origin, 0.0),
           "axis aligned"});
    }
  }
  return placements;
}

TEST(GeodeNearestSamplingTest, RowsAreTheSoftwareRenderersSinglePrecisionInverse) {
  std::vector<Placement> placements = AxisAlignedPlacements(3);
  placements.push_back({Transform2d::Rotate(0.5) * Transform2d::Translate(20.25, 7.5), "rotated"});
  placements.push_back({Transform2d::Scale(-3.75, 2.5) * Transform2d::Translate(64, 3), "flipped"});
  placements.push_back(
      {Transform2d::Scale(3.75) * Transform2d::SkewX(0.2) * Transform2d::Translate(1.5, 2),
       "skewed"});
  placements.push_back({Transform2d(), "identity"});
  placements.push_back({Transform2d::Translate(10.25, -3.5), "translated"});
  for (const Placement& placement : placements) {
    SCOPED_TRACE(testing::Message() << placement.description << " " << placement.targetFromTexel);
    const std::optional<tiny_skia::Transform> expected =
        ToTinyTransform(placement.targetFromTexel).invert();
    ASSERT_THAT(expected, testing::Ne(std::nullopt));
    const std::optional<NearestTexelRows> rows = NearestTexelRowsFor(placement.targetFromTexel);
    ASSERT_THAT(rows, testing::Ne(std::nullopt));
    EXPECT_THAT(rows->texelX, testing::ElementsAre(expected->sx, expected->kx, expected->tx));
    EXPECT_THAT(rows->texelY, testing::ElementsAre(expected->ky, expected->sy, expected->ty));
  }
}

TEST(GeodeNearestSamplingTest, SingularPlacementHasNoRows) {
  EXPECT_THAT(NearestTexelRowsFor(Transform2d::Scale(0.0, 1.0)), testing::Eq(std::nullopt));
}

/// The texel column tiny-skia samples at each pixel of row 0 when it draws a \p texels x 1 image
/// through \p targetFromTexel with nearest sampling, or -1 where it draws nothing.
std::vector<int> SoftwareRendererTexels(int texels, const Transform2d& targetFromTexel,
                                        int targetWidth) {
  std::vector<std::uint8_t> sourcePixels;
  for (int i = 0; i < texels; ++i) {
    sourcePixels.insert(sourcePixels.end(), {std::uint8_t(i + 1), 0, 0, 255});
  }
  std::optional<tiny_skia::Pixmap> target =
      tiny_skia::Pixmap::fromVec(std::vector<std::uint8_t>(std::size_t(targetWidth) * 4, 0),
                                 tiny_skia::IntSize(targetWidth, 1));
  const std::optional<tiny_skia::PixmapView> source =
      tiny_skia::PixmapView::fromBytes(sourcePixels, texels, 1);
  if (!target.has_value() || !source.has_value()) {
    ADD_FAILURE() << "could not build the tiny-skia pixmaps";
    return {};
  }
  tiny_skia::PixmapPaint paint;
  paint.quality = tiny_skia::FilterQuality::Nearest;
  tiny_skia::MutablePixmapView targetView = target->mutableView();
  tiny_skia::Painter::drawPixmap(targetView, 0, 0, *source, paint,
                                 ToTinyTransform(targetFromTexel));
  std::vector<int> result;
  for (int x = 0; x < targetWidth; ++x) {
    const std::uint8_t red = target->data()[std::size_t(x) * 4];
    const std::uint8_t alpha = target->data()[std::size_t(x) * 4 + 3];
    result.push_back(alpha == 255 ? red - 1 : -1);
  }
  return result;
}

/// The texel the rows select at pixel center \p x of row 0, evaluating `x * a + (y * b + c)` with
/// one rounding when \p fused, otherwise with a rounding after each operation.
int RowsTexel(const NearestTexelRows& rows, int x, int texels, bool fused) {
  const float centerX = static_cast<float>(x) + 0.5f;
  const float centerY = 0.5f;
  volatile float yTerm = centerY * rows.texelX[1];
  volatile float offset = yTerm + rows.texelX[2];
  float coord = 0.0f;
  if (fused) {
    coord = std::fma(centerX, rows.texelX[0], offset);
  } else {
    volatile float product = centerX * rows.texelX[0];
    coord = product + offset;
  }
  return std::clamp(static_cast<int>(std::floor(coord)), 0, texels - 1);
}

TEST(GeodeNearestSamplingTest, PixelCentersSelectTheSoftwareRenderersTexels) {
  // The software renderer evaluates the final multiply-add of its gather fused on some targets
  // and unfused on others, and a GPU compiler may do either; where the two orders pick different
  // texels, the choice depends on the platform rather than on the rows. Everywhere else the rows
  // must select the software renderer's texel.
  int compared = 0;
  int contractionDependent = 0;
  for (const int texels : {3, 8, 13}) {
    for (const Placement& placement : AxisAlignedPlacements(texels)) {
      const double origin = placement.targetFromTexel.data[4];
      if (origin > 100.0) {
        continue;  // The far origin is covered by the row comparison above.
      }
      const double width = placement.targetFromTexel.data[0] * texels;
      const int targetWidth = static_cast<int>(std::ceil(origin + width)) + 1;
      const std::vector<int> expected =
          SoftwareRendererTexels(texels, placement.targetFromTexel, targetWidth);
      const std::optional<NearestTexelRows> rows = NearestTexelRowsFor(placement.targetFromTexel);
      ASSERT_THAT(rows, testing::Ne(std::nullopt));
      for (int x = 0; x < targetWidth; ++x) {
        const double center = x + 0.5;
        if (expected[x] < 0 || center <= origin || center >= origin + width) {
          continue;
        }
        const int fused = RowsTexel(*rows, x, texels, true);
        const int unfused = RowsTexel(*rows, x, texels, false);
        ++compared;
        if (fused != unfused) {
          ++contractionDependent;
          EXPECT_THAT(expected[x], testing::AnyOf(testing::Eq(fused), testing::Eq(unfused)))
              << texels << " texels at " << origin << " width " << width << " pixel " << x;
          continue;
        }
        EXPECT_THAT(fused, testing::Eq(expected[x]))
            << texels << " texels at " << origin << " width " << width << " pixel " << x;
      }
    }
  }
  std::cout << "Compared " << compared << " pixel centers; " << contractionDependent
            << " depend on multiply-add contraction\n";
  EXPECT_THAT(compared, testing::Gt(100000));
}

TEST(GeodeNearestSamplingTest, SinglePrecisionInverseDecidesFarFromTheOrigin) {
  // A double-precision inverse rounded once selects texel 1 at pixel 1023; the software
  // renderer's single-precision inverse selects texel 2, in either multiply-add order.
  const Transform2d targetFromTexel =
      Transform2d::Scale(30.75 / 3, 1.0) * Transform2d::Translate(1003, 0);
  const std::vector<int> expected = SoftwareRendererTexels(3, targetFromTexel, 1040);
  const std::optional<NearestTexelRows> rows = NearestTexelRowsFor(targetFromTexel);
  ASSERT_THAT(rows, testing::Ne(std::nullopt));
  EXPECT_THAT(expected[1023], testing::Eq(2));
  EXPECT_THAT(RowsTexel(*rows, 1023, 3, /*fused=*/true), testing::Eq(2));
  EXPECT_THAT(RowsTexel(*rows, 1023, 3, /*fused=*/false), testing::Eq(2));
}

TEST(GeodeNearestSamplingTest, HalfPixelOriginTieDependsOnContraction) {
  // At x 10.5, width 10.5, the boundary between texels 1 and 2 passes exactly through pixel
  // center 17.5. The single-precision mapping lands on it only with a fused multiply-add, so the
  // selected texel follows the evaluation order, on the CPU and on the GPU alike.
  const Transform2d targetFromTexel =
      Transform2d::Scale(10.5 / 3, 1.0) * Transform2d::Translate(10.5, 0);
  const std::vector<int> expected = SoftwareRendererTexels(3, targetFromTexel, 32);
  const std::optional<NearestTexelRows> rows = NearestTexelRowsFor(targetFromTexel);
  ASSERT_THAT(rows, testing::Ne(std::nullopt));
  EXPECT_THAT(RowsTexel(*rows, 17, 3, /*fused=*/true), testing::Eq(2));
  EXPECT_THAT(RowsTexel(*rows, 17, 3, /*fused=*/false), testing::Eq(1));
  EXPECT_THAT(expected[17], testing::AnyOf(testing::Eq(1), testing::Eq(2)));
  std::cout << "Software renderer texel at pixel 17: " << expected[17] << '\n';
}

}  // namespace
}  // namespace donner::geode
