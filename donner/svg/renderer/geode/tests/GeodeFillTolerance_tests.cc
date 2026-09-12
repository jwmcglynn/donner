#include "donner/svg/renderer/geode/GeodeFillTolerance.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "donner/base/Path.h"

namespace donner::geode {

TEST(GeodeFillTolerance, RefinesOnlyAtScaleBucketCrossings) {
  EXPECT_DOUBLE_EQ(FillCubicToleranceFor(Transform2d()), 0.1);
  EXPECT_DOUBLE_EQ(FillCubicToleranceFor(Transform2d::Scale(0.25)), 0.1);
  EXPECT_DOUBLE_EQ(FillCubicToleranceFor(Transform2d::Scale(32.0)), 0.1 / 32.0);
  EXPECT_DOUBLE_EQ(FillCubicToleranceFor(Transform2d::Scale(25.0)), 0.1 / 32.0);
  EXPECT_DOUBLE_EQ(FillCubicToleranceFor(Transform2d::Scale(33.0)), 0.1 / 64.0);
  EXPECT_DOUBLE_EQ(FillCubicToleranceFor(Transform2d::Scale(1e200)), kMinFillCubicTolerance);
}

TEST(GeodeFillTolerance, NonfiniteLinearCoefficientsUseBoundedConservativeTolerance) {
  for (double invalid :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    for (size_t index = 0; index < 4; ++index) {
      Transform2d deviceFromLocal;
      deviceFromLocal.data[index] = invalid;
      EXPECT_DOUBLE_EQ(FillCubicToleranceFor(deviceFromLocal), kMinFillCubicTolerance)
          << "linear coefficient " << index << " was " << invalid;
    }
  }
}

TEST(GeodeFillTolerance, BoundsCubicShapeErrorUnderAffineTransforms) {
  const Path cubic = PathBuilder()
                         .moveTo({0.0, 0.0})
                         .curveTo({1.0 / 3.0, 0.0}, {2.0 / 3.0, 0.0}, {1.0, 1.0})
                         .build();
  Transform2d shear;
  shear.data[2] = 32.0;
  for (const Transform2d& deviceFromLocal :
       {Transform2d(), Transform2d::Scale(32.0), Transform2d::Scale(1024.0),
        Transform2d::Scale(Vector2d(2.0, 64.0)), shear}) {
    SCOPED_TRACE(deviceFromLocal);
    const Path approximation = cubic.cubicToQuadratic(FillCubicToleranceFor(deviceFromLocal));
    double maximumError = 0.0;
    Vector2d start;
    size_t sampledCurves = 0;
    for (const Path::Command& command : approximation.commands()) {
      if (command.verb == Path::Verb::MoveTo) {
        start = approximation.points()[command.pointIndex];
      } else if (command.verb == Path::Verb::QuadTo) {
        const Vector2d control = approximation.points()[command.pointIndex];
        const Vector2d end = approximation.points()[command.pointIndex + 1];
        ++sampledCurves;
        for (int sample = 0; sample <= 32; ++sample) {
          const double t = static_cast<double>(sample) / 32.0;
          const double u = 1.0 - t;
          const Vector2d point = start * (u * u) + control * (2.0 * u * t) + end * (t * t);
          const Vector2d reference(point.x, point.x * point.x * point.x);
          maximumError =
              std::max(maximumError, deviceFromLocal.transformVector(point - reference).length());
        }
        start = end;
      }
    }
    EXPECT_THAT(sampledCurves, testing::Gt(0u));
    EXPECT_THAT(maximumError, testing::Le(kFillCubicDevicePixels));
  }
}

}  // namespace donner::geode
