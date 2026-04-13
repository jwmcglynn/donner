/**
 * Tests for \ref donner::svg::ElementType enum and its ostream output operator.
 */

#include "donner/svg/ElementType.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref ElementType values.
TEST(ElementTypeTest, OstreamOutput) {
  EXPECT_THAT(ElementType::Circle, ToStringIs("Circle"));
  EXPECT_THAT(ElementType::ClipPath, ToStringIs("ClipPath"));
  EXPECT_THAT(ElementType::Defs, ToStringIs("Defs"));
  EXPECT_THAT(ElementType::Ellipse, ToStringIs("Ellipse"));
  EXPECT_THAT(ElementType::FeGaussianBlur, ToStringIs("FeGaussianBlur"));
  EXPECT_THAT(ElementType::Filter, ToStringIs("Filter"));
  EXPECT_THAT(ElementType::G, ToStringIs("G"));
  EXPECT_THAT(ElementType::Image, ToStringIs("Image"));
  EXPECT_THAT(ElementType::Line, ToStringIs("Line"));
  EXPECT_THAT(ElementType::LinearGradient, ToStringIs("LinearGradient"));
  EXPECT_THAT(ElementType::Marker, ToStringIs("Marker"));
  EXPECT_THAT(ElementType::Mask, ToStringIs("Mask"));
  EXPECT_THAT(ElementType::Path, ToStringIs("Path"));
  EXPECT_THAT(ElementType::Pattern, ToStringIs("Pattern"));
  EXPECT_THAT(ElementType::Polygon, ToStringIs("Polygon"));
  EXPECT_THAT(ElementType::Polyline, ToStringIs("Polyline"));
  EXPECT_THAT(ElementType::RadialGradient, ToStringIs("RadialGradient"));
  EXPECT_THAT(ElementType::Rect, ToStringIs("Rect"));
  EXPECT_THAT(ElementType::Stop, ToStringIs("Stop"));
  EXPECT_THAT(ElementType::Style, ToStringIs("Style"));
  EXPECT_THAT(ElementType::SVG, ToStringIs("SVG"));
  EXPECT_THAT(ElementType::Symbol, ToStringIs("Symbol"));
  EXPECT_THAT(ElementType::Text, ToStringIs("Text"));
  EXPECT_THAT(ElementType::TextPath, ToStringIs("TextPath"));
  EXPECT_THAT(ElementType::TSpan, ToStringIs("TSpan"));
  EXPECT_THAT(ElementType::Unknown, ToStringIs("Unknown"));
  EXPECT_THAT(ElementType::Use, ToStringIs("Use"));
}

}  // namespace donner::svg
