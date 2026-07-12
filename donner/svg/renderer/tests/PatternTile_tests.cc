#include "donner/svg/renderer/PatternTile.h"

#include <gmock/gmock.h>

#include <cmath>
#include <limits>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {
namespace {

using testing::DoubleNear;

TEST(PatternTile, RasterMetricsUsePatternThenTargetComposition) {
  const Transform2d targetFromPattern = Transform2d::SkewX(std::atan(1.0));
  const Transform2d deviceFromTarget = Transform2d::Scale(2.0, 3.0);

  const std::optional<PatternTileRasterMetrics> metrics = ComputePatternTileRasterMetrics(
      Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0), targetFromPattern * deviceFromTarget);

  ASSERT_TRUE(metrics.has_value());
  EXPECT_EQ(metrics->pixelWidth, 4);
  EXPECT_EQ(metrics->pixelHeight, 8);
  EXPECT_THAT(metrics->rasterFromPatternScale,
              Vector2Eq(DoubleNear(4.0, 1e-9), DoubleNear(8.0, 1e-9)));
}

TEST(PatternTile, RasterMetricsPreserveNonSquareDimensions) {
  const std::optional<PatternTileRasterMetrics> metrics =
      ComputePatternTileRasterMetrics(Box2d::FromXYWH(3.0, 4.0, 2.5, 4.0), Transform2d());

  ASSERT_TRUE(metrics.has_value());
  EXPECT_EQ(metrics->pixelWidth, 5);
  EXPECT_EQ(metrics->pixelHeight, 8);
  EXPECT_THAT(metrics->rasterFromPatternScale,
              Vector2Eq(DoubleNear(2.0, 1e-9), DoubleNear(2.0, 1e-9)));
}

TEST(PatternTile, TargetFromRasterPreservesTranslatedPatternPlacement) {
  const Transform2d targetFromPattern = Transform2d::SkewX(std::atan(1.0)) *
                                        Transform2d::Scale(2.0) * Transform2d::Translate(3.0, 5.0);

  const Transform2d targetFromRaster =
      TargetFromPatternRaster(targetFromPattern, Vector2d(4.0, 6.0));

  EXPECT_THAT(targetFromRaster, TransformIs(0.5, 0.0, 1.0 / 3.0, 1.0 / 3.0, 3.0, 5.0));
  EXPECT_THAT(targetFromRaster.transformPosition(Vector2d(8.0, 12.0)),
              Vector2Eq(DoubleNear(11.0, 1e-9), DoubleNear(9.0, 1e-9)));
  EXPECT_THAT(targetFromRaster.inverse().transformPosition(Vector2d(3.0, 5.0)),
              Vector2Eq(DoubleNear(0.0, 1e-9), DoubleNear(0.0, 1e-9)));
}

TEST(PatternTile, TargetFromRasterDividesCoordinatesBeforeAnisotropicTransform) {
  const Transform2d targetFromPattern = Transform2d::Rotate(std::atan(1.0)) *
                                        Transform2d::SkewX(std::atan(0.5)) *
                                        Transform2d::Translate(7.0, 11.0);
  const Vector2d rasterFromPatternScale(3.0, 5.0);
  const Vector2d rasterPosition(12.0, 10.0);

  const Transform2d targetFromRaster =
      TargetFromPatternRaster(targetFromPattern, rasterFromPatternScale);
  const Vector2d expectedTargetPosition =
      targetFromPattern.transformPosition(rasterPosition / rasterFromPatternScale);

  EXPECT_THAT(targetFromRaster.transformPosition(rasterPosition),
              Vector2Eq(DoubleNear(expectedTargetPosition.x, 1e-9),
                        DoubleNear(expectedTargetPosition.y, 1e-9)));
}

TEST(PatternTile, RasterMetricsRejectUnsafeDimensionsAndTransforms) {
  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  const Transform2d identity;

  EXPECT_FALSE(ComputePatternTileRasterMetrics(Box2d::FromXYWH(0.0, 0.0, 0.0, 1.0), identity));
  EXPECT_FALSE(ComputePatternTileRasterMetrics(Box2d::FromXYWH(0.0, 0.0, -1.0, 1.0), identity));
  EXPECT_FALSE(
      ComputePatternTileRasterMetrics(Box2d::FromXYWH(0.0, 0.0, kInfinity, 1.0), identity));
  EXPECT_FALSE(ComputePatternTileRasterMetrics(Box2d::FromXYWH(0.0, 0.0, kNaN, 1.0), identity));
  EXPECT_FALSE(ComputePatternTileRasterMetrics(
      Box2d::FromXYWH(0.0, 0.0, kMaxPatternTileRasterDimension / 2.0 + 1.0, 1.0), identity));
  EXPECT_FALSE(ComputePatternTileRasterMetrics(Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0),
                                               Transform2d::Scale(0.0, 1.0)));
  EXPECT_FALSE(ComputePatternTileRasterMetrics(
      Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0),
      Transform2d::Scale(std::numeric_limits<double>::denorm_min(), 1.0)));

  Transform2d nonFiniteTransform;
  nonFiniteTransform.data[0] = kInfinity;
  EXPECT_FALSE(
      ComputePatternTileRasterMetrics(Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0), nonFiniteTransform));
}

TEST(PatternTile, RasterMetricsAcceptMaximumBoundedAllocation) {
  const std::optional<PatternTileRasterMetrics> metrics = ComputePatternTileRasterMetrics(
      Box2d::FromXYWH(0.0, 0.0, kMaxPatternTileRasterDimension / 2.0,
                      kMaxPatternTileRasterDimension / 2.0),
      Transform2d());

  ASSERT_TRUE(metrics.has_value());
  EXPECT_EQ(metrics->pixelWidth, kMaxPatternTileRasterDimension);
  EXPECT_EQ(metrics->pixelHeight, kMaxPatternTileRasterDimension);
}

}  // namespace
}  // namespace donner::svg
