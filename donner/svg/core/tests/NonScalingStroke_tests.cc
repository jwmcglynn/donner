#include "donner/svg/core/NonScalingStroke.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>

namespace donner::svg {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInfinity = std::numeric_limits<double>::infinity();

Transform2d WithComponent(size_t index, double value) {
  Transform2d transform;
  transform.data[index] = value;
  return transform;
}

}  // namespace

TEST(NonScalingStroke, SimilarityClassification) {
  EXPECT_TRUE(IsSimilarityTransform(Transform2d()));
  EXPECT_TRUE(IsSimilarityTransform(Transform2d::Scale({3.0, 3.0})));
  EXPECT_TRUE(IsSimilarityTransform(Transform2d::Rotate(0.7)));
  EXPECT_TRUE(IsSimilarityTransform(Transform2d::Scale({-2.0, 2.0}))) << "reflection is uniform";
  EXPECT_TRUE(IsSimilarityTransform(Transform2d::Translate({5.0, 9.0})));

  EXPECT_FALSE(IsSimilarityTransform(Transform2d::Scale({2.0, 1.0})));
  EXPECT_FALSE(IsSimilarityTransform(WithComponent(2, 0.5))) << "shear is not a similarity";
  EXPECT_FALSE(IsSimilarityTransform(Transform2d::Scale({0.0, 0.0})));
}

TEST(NonScalingStroke, ModeIsScalingWithoutTheVectorEffect) {
  EXPECT_EQ(ResolveNonScalingStrokeMode(VectorEffect::None, Transform2d::Scale({2.0, 1.0}),
                                        /*geometryMustStayLocal=*/false),
            NonScalingStrokeMode::Scaling);
  EXPECT_EQ(
      ResolveNonScalingStrokeMode(VectorEffect::NonScalingSize, Transform2d::Scale({2.0, 1.0}),
                                  /*geometryMustStayLocal=*/false),
      NonScalingStrokeMode::Scaling);
}

TEST(NonScalingStroke, ModeSplitsOnSimilarityAndLocalGeometry) {
  EXPECT_EQ(ResolveNonScalingStrokeMode(VectorEffect::NonScalingStroke, Transform2d::Scale(2.0),
                                        /*geometryMustStayLocal=*/false),
            NonScalingStrokeMode::ScaledLocal)
      << "a similarity reaches the authored width with the scalar adjustment";
  EXPECT_EQ(
      ResolveNonScalingStrokeMode(VectorEffect::NonScalingStroke, Transform2d::Scale({2.0, 1.0}),
                                  /*geometryMustStayLocal=*/false),
      NonScalingStrokeMode::HostSpace);
  EXPECT_EQ(
      ResolveNonScalingStrokeMode(VectorEffect::NonScalingStroke, Transform2d::Scale({2.0, 1.0}),
                                  /*geometryMustStayLocal=*/true),
      NonScalingStrokeMode::ScaledLocal)
      << "text and pattern strokes cannot be handed host-space geometry";
}

// A non-finite CTM is not a similarity, but it must not select the host-space path: the mapped
// centerline would be non-finite, and every cache keyed on the transform would miss forever
// because a NaN component compares unequal to itself.
TEST(NonScalingStroke, ModeKeepsNonFiniteTransformsOutOfHostSpace) {
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(ResolveNonScalingStrokeMode(VectorEffect::NonScalingStroke, WithComponent(i, kNaN),
                                          /*geometryMustStayLocal=*/false),
              NonScalingStrokeMode::ScaledLocal)
        << "NaN in component " << i;
    EXPECT_EQ(
        ResolveNonScalingStrokeMode(VectorEffect::NonScalingStroke, WithComponent(i, kInfinity),
                                    /*geometryMustStayLocal=*/false),
        NonScalingStrokeMode::ScaledLocal)
        << "infinity in component " << i;
  }
}

TEST(NonScalingStroke, EffectiveWidthDividesOnlyTheScaledLocalMode) {
  const Transform2d anisotropic = Transform2d::Scale({2.0, 8.0});
  const double scale = 4.0;  // sqrt(|det|) = sqrt(16)

  EXPECT_DOUBLE_EQ(EffectiveStrokeWidth(10.0, NonScalingStrokeMode::Scaling, anisotropic), 10.0);
  EXPECT_DOUBLE_EQ(EffectiveStrokeWidth(10.0, NonScalingStrokeMode::HostSpace, anisotropic), 10.0);
  EXPECT_DOUBLE_EQ(EffectiveStrokeWidth(10.0, NonScalingStrokeMode::ScaledLocal, anisotropic),
                   10.0 / scale);
}

TEST(NonScalingStroke, EffectiveWidthFallsBackOnADegenerateTransform) {
  EXPECT_DOUBLE_EQ(
      EffectiveStrokeWidth(10.0, NonScalingStrokeMode::ScaledLocal, Transform2d::Scale(0.0)), 10.0);
  EXPECT_DOUBLE_EQ(
      EffectiveStrokeWidth(10.0, NonScalingStrokeMode::ScaledLocal, WithComponent(0, kNaN)), 10.0);
}

TEST(NonScalingStroke, CullHalfExtentCoversTheMiterTip) {
  EXPECT_DOUBLE_EQ(StrokeCullHalfExtent(20.0, StrokeLinecap::Butt, StrokeLinejoin::Round, 4.0),
                   10.0);
  EXPECT_DOUBLE_EQ(StrokeCullHalfExtent(20.0, StrokeLinecap::Butt, StrokeLinejoin::Bevel, 4.0),
                   10.0);
  EXPECT_DOUBLE_EQ(StrokeCullHalfExtent(20.0, StrokeLinecap::Butt, StrokeLinejoin::Miter, 4.0),
                   40.0);
  EXPECT_DOUBLE_EQ(StrokeCullHalfExtent(20.0, StrokeLinecap::Butt, StrokeLinejoin::MiterClip, 2.5),
                   25.0);
  EXPECT_DOUBLE_EQ(StrokeCullHalfExtent(20.0, StrokeLinecap::Butt, StrokeLinejoin::Miter, 0.5),
                   10.0)
      << "a miter limit below 1 cannot pull the reach inside the half width";
}

// A square cap extends the segment by half the stroke width, so its far corners sit
// sqrt(2) * halfStroke from the endpoint rather than halfStroke from the centerline.
TEST(NonScalingStroke, CullHalfExtentCoversTheSquareCapCorner) {
  EXPECT_DOUBLE_EQ(StrokeCullHalfExtent(20.0, StrokeLinecap::Square, StrokeLinejoin::Round, 4.0),
                   10.0 * std::numbers::sqrt2);
  EXPECT_DOUBLE_EQ(StrokeCullHalfExtent(20.0, StrokeLinecap::Round, StrokeLinejoin::Round, 4.0),
                   10.0);
  EXPECT_DOUBLE_EQ(StrokeCullHalfExtent(20.0, StrokeLinecap::Square, StrokeLinejoin::Miter, 4.0),
                   40.0)
      << "the miter tip is further than the cap corner, so it wins";
}

// A cull bound may only over-count. An unbounded miter limit has an unbounded tip, so the only
// honest answer is an infinite reach, which callers read as "cannot bound".
TEST(NonScalingStroke, CullHalfExtentIsInfiniteForANonFiniteMiterLimit) {
  EXPECT_TRUE(std::isinf(
      StrokeCullHalfExtent(20.0, StrokeLinecap::Butt, StrokeLinejoin::Miter, kInfinity)));
  EXPECT_TRUE(
      std::isinf(StrokeCullHalfExtent(20.0, StrokeLinecap::Butt, StrokeLinejoin::Miter, kNaN)));
  EXPECT_DOUBLE_EQ(
      StrokeCullHalfExtent(20.0, StrokeLinecap::Butt, StrokeLinejoin::Round, kInfinity), 10.0)
      << "a round join ignores the miter limit entirely";
}

}  // namespace donner::svg
