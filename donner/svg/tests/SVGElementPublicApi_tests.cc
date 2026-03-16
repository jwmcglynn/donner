/**
 * @file Tests for SVG element public APIs: SVGSVGElement, SVGImageElement, SVGUseElement,
 * SVGStopElement, SVGTextPositioningElement.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/Length.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGImageElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/SVGStopElement.h"
#include "donner/svg/SVGTextPositioningElement.h"
#include "donner/svg/SVGTSpanElement.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::DoubleNear;
using testing::Eq;
using testing::FloatNear;
using testing::Ne;
using testing::Optional;

namespace donner::svg {

namespace {

constexpr parser::SVGParser::Options kExperimentalOptions = []() {
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  return options;
}();

}  // namespace

// --- SVGSVGElement ---

TEST(SVGSVGElementTest, DefaultAttributes) {
  auto document = instantiateSubtree(R"(<rect width="10" height="10"/>)");
  auto svg = document.svgElement();
  EXPECT_EQ(svg.x(), Lengthd(0));
  EXPECT_EQ(svg.y(), Lengthd(0));
}

TEST(SVGSVGElementTest, SetAndGetViewBox) {
  auto document = instantiateSubtree(R"(<rect width="10" height="10"/>)");
  auto svg = document.svgElement();

  svg.setViewBox(10, 20, 300, 400);
  auto viewBox = svg.viewBox();
  ASSERT_TRUE(viewBox.has_value());
  EXPECT_NEAR(viewBox->topLeft.x, 10.0, 0.001);
  EXPECT_NEAR(viewBox->topLeft.y, 20.0, 0.001);
  EXPECT_NEAR(viewBox->size().x, 300.0, 0.001);
  EXPECT_NEAR(viewBox->size().y, 400.0, 0.001);
}

TEST(SVGSVGElementTest, SetWidthHeight) {
  auto document = instantiateSubtree(R"(<rect width="10" height="10"/>)");
  auto svg = document.svgElement();

  svg.setWidth(Lengthd(500, Lengthd::Unit::Px));
  svg.setHeight(Lengthd(300, Lengthd::Unit::Px));

  ASSERT_TRUE(svg.width().has_value());
  EXPECT_EQ(svg.width().value(), Lengthd(500, Lengthd::Unit::Px));
  ASSERT_TRUE(svg.height().has_value());
  EXPECT_EQ(svg.height().value(), Lengthd(300, Lengthd::Unit::Px));
}

TEST(SVGSVGElementTest, ClearViewBox) {
  auto document = instantiateSubtree(R"(<rect width="10" height="10"/>)");
  auto svg = document.svgElement();

  svg.setViewBox(0, 0, 100, 100);
  EXPECT_TRUE(svg.viewBox().has_value());

  svg.setViewBox(std::nullopt);
  EXPECT_FALSE(svg.viewBox().has_value());
}

// --- SVGImageElement ---

TEST(SVGImageElementTest, DefaultAttributes) {
  auto frag = instantiateSubtreeElementAs<SVGImageElement>(R"(<image />)");
  EXPECT_EQ(frag->x(), Lengthd(0));
  EXPECT_EQ(frag->y(), Lengthd(0));
  EXPECT_FALSE(frag->width().has_value());
  EXPECT_FALSE(frag->height().has_value());
  EXPECT_EQ(frag->href(), "");
}

TEST(SVGImageElementTest, SetAttributes) {
  auto frag = instantiateSubtreeElementAs<SVGImageElement>(
      R"(<image x="10" y="20" width="80" height="60" href="test.png"/>)");

  EXPECT_EQ(frag->x(), Lengthd(10));
  EXPECT_EQ(frag->y(), Lengthd(20));
  ASSERT_TRUE(frag->width().has_value());
  EXPECT_EQ(frag->width().value(), Lengthd(80));
  ASSERT_TRUE(frag->height().has_value());
  EXPECT_EQ(frag->height().value(), Lengthd(60));
  EXPECT_EQ(frag->href(), "test.png");
}

TEST(SVGImageElementTest, SetHrefProgrammatically) {
  auto frag = instantiateSubtreeElementAs<SVGImageElement>(R"(<image />)");
  frag->setHref("data:image/png;base64,abc");
  EXPECT_EQ(frag->href(), "data:image/png;base64,abc");
}

TEST(SVGImageElementTest, SetPositionProgrammatically) {
  auto frag = instantiateSubtreeElementAs<SVGImageElement>(R"(<image />)");
  frag->setX(Lengthd(15));
  frag->setY(Lengthd(25));
  EXPECT_EQ(frag->x(), Lengthd(15));
  EXPECT_EQ(frag->y(), Lengthd(25));
}

// --- SVGUseElement ---

TEST(SVGUseElementTest, DefaultAttributes) {
  auto frag = instantiateSubtreeElementAs<SVGUseElement>(R"(<use />)");
  EXPECT_EQ(frag->x(), Lengthd(0));
  EXPECT_EQ(frag->y(), Lengthd(0));
  EXPECT_FALSE(frag->width().has_value());
  EXPECT_FALSE(frag->height().has_value());
  EXPECT_EQ(frag->href(), "");
}

TEST(SVGUseElementTest, ParsedAttributes) {
  auto frag = instantiateSubtreeElementAs<SVGUseElement>(
      R"(<use x="10" y="20" href="#template"/>)");

  EXPECT_EQ(frag->x(), Lengthd(10));
  EXPECT_EQ(frag->y(), Lengthd(20));
  EXPECT_EQ(frag->href(), "#template");
}

TEST(SVGUseElementTest, SetAttributesProgrammatically) {
  auto frag = instantiateSubtreeElementAs<SVGUseElement>(R"(<use />)");
  frag->setX(Lengthd(30));
  frag->setY(Lengthd(40));
  frag->setHref("#myShape");
  EXPECT_EQ(frag->x(), Lengthd(30));
  EXPECT_EQ(frag->y(), Lengthd(40));
  EXPECT_EQ(frag->href(), "#myShape");
}

TEST(SVGUseElementTest, OptionalWidthHeight) {
  auto frag = instantiateSubtreeElementAs<SVGUseElement>(
      R"(<use width="50" height="60"/>)");

  ASSERT_TRUE(frag->width().has_value());
  EXPECT_EQ(frag->width().value(), Lengthd(50));
  ASSERT_TRUE(frag->height().has_value());
  EXPECT_EQ(frag->height().value(), Lengthd(60));
}

// --- SVGStopElement ---

TEST(SVGStopElementTest, DefaultOffset) {
  auto frag = instantiateSubtreeElementAs<SVGStopElement>(R"(<stop />)");
  EXPECT_THAT(frag->offset(), FloatNear(0.0f, 0.001f));
}

TEST(SVGStopElementTest, ParsedOffset) {
  auto frag = instantiateSubtreeElementAs<SVGStopElement>(
      R"(<stop offset="0.7" stop-color="red"/>)");
  EXPECT_THAT(frag->offset(), FloatNear(0.7f, 0.001f));
}

TEST(SVGStopElementTest, SetOffsetProgrammatically) {
  auto frag = instantiateSubtreeElementAs<SVGStopElement>(R"(<stop />)");
  frag->setOffset(0.5f);
  EXPECT_THAT(frag->offset(), FloatNear(0.5f, 0.001f));
}

TEST(SVGStopElementTest, SetStopColor) {
  auto frag = instantiateSubtreeElementAs<SVGStopElement>(R"(<stop />)");
  frag->setStopColor(css::Color(css::RGBA(255, 0, 0, 255)));
  auto color = frag->stopColor();
  EXPECT_TRUE(color.hasValue());
}

TEST(SVGStopElementTest, SetStopOpacity) {
  auto frag = instantiateSubtreeElementAs<SVGStopElement>(R"(<stop />)");
  frag->setStopOpacity(0.5);
  EXPECT_THAT(frag->stopOpacity(), DoubleNear(0.5, 0.001));
}

// --- SVGTextPositioningElement (via TSpan) ---

TEST(SVGTextPositioningElementTest, SetAndGetSinglePositions) {
  auto tspan = instantiateSubtreeElementAs<SVGTSpanElement>("<tspan />", kExperimentalOptions);

  tspan->setX(Lengthd(10));
  tspan->setY(Lengthd(20));
  tspan->setDx(Lengthd(5));
  tspan->setDy(Lengthd(3));

  EXPECT_THAT(tspan->x(), Optional(LengthIs(10, Lengthd::Unit::None)));
  EXPECT_THAT(tspan->y(), Optional(LengthIs(20, Lengthd::Unit::None)));
  EXPECT_THAT(tspan->dx(), Optional(LengthIs(5, Lengthd::Unit::None)));
  EXPECT_THAT(tspan->dy(), Optional(LengthIs(3, Lengthd::Unit::None)));
}

TEST(SVGTextPositioningElementTest, SetAndGetRotate) {
  auto tspan = instantiateSubtreeElementAs<SVGTSpanElement>("<tspan />", kExperimentalOptions);

  tspan->setRotate(45.0);
  EXPECT_THAT(tspan->rotate(), Optional(DoubleNear(45.0, 1e-6)));
}

TEST(SVGTextPositioningElementTest, ClearPositions) {
  auto tspan = instantiateSubtreeElementAs<SVGTSpanElement>(
      R"(<tspan x="10" y="20" />)", kExperimentalOptions);

  EXPECT_THAT(tspan->x(), Ne(std::nullopt));
  tspan->setX(std::nullopt);
  EXPECT_THAT(tspan->x(), Eq(std::nullopt));
}

TEST(SVGTextPositioningElementTest, PositionLists) {
  auto tspan = instantiateSubtreeElementAs<SVGTSpanElement>(
      R"(<tspan x="1 2 3" y="4 5 6" />)", kExperimentalOptions);

  EXPECT_EQ(tspan->xList().size(), 3u);
  EXPECT_EQ(tspan->yList().size(), 3u);
}

TEST(SVGTextPositioningElementTest, RotateList) {
  auto tspan = instantiateSubtreeElementAs<SVGTSpanElement>(
      R"(<tspan rotate="10 20 30" />)", kExperimentalOptions);

  EXPECT_EQ(tspan->rotateList().size(), 3u);
}

}  // namespace donner::svg
