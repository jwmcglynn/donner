/**
 * Tests for paint/font-related SVG core enums and ostream output operators.
 */

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/core/ColorInterpolationFilters.h"
#include "donner/svg/core/FontStyle.h"
#include "donner/svg/core/FontVariant.h"
#include "donner/svg/core/ImageRendering.h"
#include "donner/svg/core/PaintOrder.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref ImageRendering values.
TEST(ImageRenderingTest, OstreamOutput) {
  EXPECT_THAT(ImageRendering::Auto, ToStringIs("auto"));
  EXPECT_THAT(ImageRendering::OptimizeSpeed, ToStringIs("optimizeSpeed"));
  EXPECT_THAT(ImageRendering::OptimizeQuality, ToStringIs("optimizeQuality"));
  EXPECT_THAT(ImageRendering::CrispEdges, ToStringIs("crisp-edges"));
  EXPECT_THAT(ImageRendering::Pixelated, ToStringIs("pixelated"));
  EXPECT_THAT(ImageRendering::Smooth, ToStringIs("smooth"));
}

/// @test Ostream output \c operator<< for all \ref ColorInterpolationFilters values.
TEST(ColorInterpolationFiltersTest, OstreamOutput) {
  EXPECT_THAT(ColorInterpolationFilters::SRGB, ToStringIs("sRGB"));
  EXPECT_THAT(ColorInterpolationFilters::LinearRGB, ToStringIs("linearRGB"));
  EXPECT_THAT(static_cast<ColorInterpolationFilters>(99),
              ToStringIs("ColorInterpolationFilters(99)"));
}

/// @test Ostream output \c operator<< for all \ref FontStyle values.
TEST(FontStyleTest, OstreamOutput) {
  EXPECT_THAT(FontStyle::Normal, ToStringIs("normal"));
  EXPECT_THAT(FontStyle::Italic, ToStringIs("italic"));
  EXPECT_THAT(FontStyle::Oblique, ToStringIs("oblique"));
}

/// @test Ostream output \c operator<< for all \ref FontVariant values.
TEST(FontVariantTest, OstreamOutput) {
  EXPECT_THAT(FontVariant::Normal, ToStringIs("normal"));
  EXPECT_THAT(FontVariant::SmallCaps, ToStringIs("small-caps"));
}

/// @test Ostream output \c operator<< for all \ref PaintComponent values.
TEST(PaintComponentTest, OstreamOutput) {
  EXPECT_THAT(PaintComponent::Fill, ToStringIs("fill"));
  EXPECT_THAT(PaintComponent::Stroke, ToStringIs("stroke"));
  EXPECT_THAT(PaintComponent::Markers, ToStringIs("markers"));
}

TEST(PaintOrderTest, DefaultOrderAndOstreamOutput) {
  EXPECT_THAT(PaintOrder(), ToStringIs("PaintOrder(fill stroke markers)"));

  PaintOrder strokeFirst{
      .order = {PaintComponent::Stroke, PaintComponent::Fill, PaintComponent::Markers},
  };
  EXPECT_THAT(strokeFirst, ToStringIs("PaintOrder(stroke fill markers)"));
  EXPECT_NE(strokeFirst, PaintOrder());
}

}  // namespace donner::svg
