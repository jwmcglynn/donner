/**
 * @file Tests for AttributeParser: verifies that XML attributes are correctly parsed and applied
 * to SVG elements across all element types.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/Length.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGCircleElement.h"
#include "donner/svg/SVGClipPathElement.h"
#include "donner/svg/SVGEllipseElement.h"
#include "donner/svg/SVGFilterElement.h"
#include "donner/svg/SVGImageElement.h"
#include "donner/svg/SVGLineElement.h"
#include "donner/svg/SVGLinearGradientElement.h"
#include "donner/svg/SVGMarkerElement.h"
#include "donner/svg/SVGPatternElement.h"
#include "donner/svg/SVGPolygonElement.h"
#include "donner/svg/SVGPolylineElement.h"
#include "donner/svg/SVGRadialGradientElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGStopElement.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/parser/SVGParser.h"

using testing::DoubleNear;
using testing::FloatNear;

namespace donner::svg::parser {

namespace {

SVGDocument ParseSVG(std::string_view input) {
  auto maybeResult = SVGParser::ParseSVG(input);
  EXPECT_THAT(maybeResult, NoParseError());
  return std::move(maybeResult).result();
}

template <typename T>
T QueryElement(SVGDocument& document, const char* selector) {
  auto element = document.querySelector(selector);
  EXPECT_TRUE(element.has_value()) << "Missing: " << selector;
  return element->cast<T>();
}

}  // namespace

// --- Circle attributes ---

TEST(AttributeParserTest, CircleAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <circle id="c" cx="25" cy="30" r="15"/>
    </svg>
  )");

  auto circle = QueryElement<SVGCircleElement>(document, "#c");
  EXPECT_EQ(circle.cx(), Lengthd(25));
  EXPECT_EQ(circle.cy(), Lengthd(30));
  EXPECT_EQ(circle.r(), Lengthd(15));
}

TEST(AttributeParserTest, CirclePercentageAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 100">
      <circle id="c" cx="50%" cy="25%" r="10%"/>
    </svg>
  )");

  auto circle = QueryElement<SVGCircleElement>(document, "#c");
  EXPECT_EQ(circle.cx(), Lengthd(50, Lengthd::Unit::Percent));
  EXPECT_EQ(circle.cy(), Lengthd(25, Lengthd::Unit::Percent));
  EXPECT_EQ(circle.r(), Lengthd(10, Lengthd::Unit::Percent));
}

// --- Ellipse attributes ---

TEST(AttributeParserTest, EllipseAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <ellipse id="e" cx="50" cy="50" rx="30" ry="20"/>
    </svg>
  )");

  auto ellipse = QueryElement<SVGEllipseElement>(document, "#e");
  EXPECT_EQ(ellipse.cx(), Lengthd(50));
  EXPECT_EQ(ellipse.cy(), Lengthd(50));
  EXPECT_EQ(ellipse.rx(), Lengthd(30));
  EXPECT_EQ(ellipse.ry(), Lengthd(20));
}

// --- Rect attributes ---

TEST(AttributeParserTest, RectAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="20" width="80" height="60"/>
    </svg>
  )");

  auto rect = QueryElement<SVGRectElement>(document, "#r");
  EXPECT_EQ(rect.x(), Lengthd(10));
  EXPECT_EQ(rect.y(), Lengthd(20));
  EXPECT_EQ(rect.width(), Lengthd(80));
  EXPECT_EQ(rect.height(), Lengthd(60));
}

TEST(AttributeParserTest, RectRoundedCorners) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="0" y="0" width="100" height="100" rx="5" ry="10"/>
    </svg>
  )");

  auto rect = QueryElement<SVGRectElement>(document, "#r");
  ASSERT_TRUE(rect.rx().has_value());
  EXPECT_EQ(rect.rx().value(), Lengthd(5));
  ASSERT_TRUE(rect.ry().has_value());
  EXPECT_EQ(rect.ry().value(), Lengthd(10));
}

TEST(AttributeParserTest, RectNoRoundedCorners) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="0" y="0" width="100" height="100"/>
    </svg>
  )");

  auto rect = QueryElement<SVGRectElement>(document, "#r");
  EXPECT_FALSE(rect.rx().has_value());
  EXPECT_FALSE(rect.ry().has_value());
}

// --- Line attributes ---

TEST(AttributeParserTest, LineAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <line id="l" x1="10" y1="20" x2="90" y2="80"/>
    </svg>
  )");

  auto line = QueryElement<SVGLineElement>(document, "#l");
  EXPECT_EQ(line.x1(), Lengthd(10));
  EXPECT_EQ(line.y1(), Lengthd(20));
  EXPECT_EQ(line.x2(), Lengthd(90));
  EXPECT_EQ(line.y2(), Lengthd(80));
}

// --- Linear gradient attributes ---

TEST(AttributeParserTest, LinearGradientAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="100%">
          <stop offset="0" stop-color="red"/>
        </linearGradient>
      </defs>
    </svg>
  )");

  auto gradient = QueryElement<SVGLinearGradientElement>(document, "#g");
  ASSERT_TRUE(gradient.x1().has_value());
  EXPECT_EQ(gradient.x1().value(), Lengthd(0, Lengthd::Unit::Percent));
  ASSERT_TRUE(gradient.x2().has_value());
  EXPECT_EQ(gradient.x2().value(), Lengthd(100, Lengthd::Unit::Percent));
}

// --- Radial gradient attributes ---

TEST(AttributeParserTest, RadialGradientAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <radialGradient id="g" cx="50%" cy="50%" r="40%" fx="30%" fy="30%">
          <stop offset="0" stop-color="white"/>
        </radialGradient>
      </defs>
    </svg>
  )");

  auto gradient = QueryElement<SVGRadialGradientElement>(document, "#g");
  ASSERT_TRUE(gradient.cx().has_value());
  EXPECT_EQ(gradient.cx().value(), Lengthd(50, Lengthd::Unit::Percent));
  ASSERT_TRUE(gradient.r().has_value());
  EXPECT_EQ(gradient.r().value(), Lengthd(40, Lengthd::Unit::Percent));
  ASSERT_TRUE(gradient.fx().has_value());
  EXPECT_EQ(gradient.fx().value(), Lengthd(30, Lengthd::Unit::Percent));
  ASSERT_TRUE(gradient.fy().has_value());
  EXPECT_EQ(gradient.fy().value(), Lengthd(30, Lengthd::Unit::Percent));
}

// --- Stop attributes ---

TEST(AttributeParserTest, StopOffsetNumber) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g">
          <stop id="s" offset="0.5" stop-color="red"/>
        </linearGradient>
      </defs>
    </svg>
  )");

  auto stop = QueryElement<SVGStopElement>(document, "#s");
  EXPECT_THAT(stop.offset(), FloatNear(0.5f, 0.001f));
}

TEST(AttributeParserTest, StopOffsetPercent) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g">
          <stop id="s" offset="75%" stop-color="blue"/>
        </linearGradient>
      </defs>
    </svg>
  )");

  auto stop = QueryElement<SVGStopElement>(document, "#s");
  EXPECT_THAT(stop.offset(), FloatNear(0.75f, 0.001f));
}

TEST(AttributeParserTest, StopOffsetClamped) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g">
          <stop id="s1" offset="2.0" stop-color="red"/>
          <stop id="s2" offset="-1.0" stop-color="blue"/>
        </linearGradient>
      </defs>
    </svg>
  )");

  auto stop1 = QueryElement<SVGStopElement>(document, "#s1");
  EXPECT_THAT(stop1.offset(), FloatNear(1.0f, 0.001f));

  auto stop2 = QueryElement<SVGStopElement>(document, "#s2");
  EXPECT_THAT(stop2.offset(), FloatNear(0.0f, 0.001f));
}

// --- Filter attributes ---

TEST(AttributeParserTest, FilterAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur stdDeviation="5"/>
        </filter>
      </defs>
    </svg>
  )");

  auto filter = QueryElement<SVGFilterElement>(document, "#f");
  EXPECT_EQ(filter.x(), Lengthd(-20, Lengthd::Unit::Percent));
  EXPECT_EQ(filter.y(), Lengthd(-20, Lengthd::Unit::Percent));
  EXPECT_EQ(filter.width(), Lengthd(140, Lengthd::Unit::Percent));
  EXPECT_EQ(filter.height(), Lengthd(140, Lengthd::Unit::Percent));
}

// --- Image attributes ---

TEST(AttributeParserTest, ImageAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
         viewBox="0 0 100 100">
      <image id="img" x="10" y="20" width="80" height="60" href="test.png"/>
    </svg>
  )");

  auto image = QueryElement<SVGImageElement>(document, "#img");
  EXPECT_EQ(image.x(), Lengthd(10));
  EXPECT_EQ(image.y(), Lengthd(20));
  ASSERT_TRUE(image.width().has_value());
  EXPECT_EQ(image.width().value(), Lengthd(80));
  ASSERT_TRUE(image.height().has_value());
  EXPECT_EQ(image.height().value(), Lengthd(60));
  EXPECT_EQ(image.href(), "test.png");
}

// --- Use element attributes ---

TEST(AttributeParserTest, UseAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <rect id="template" width="50" height="50"/>
      </defs>
      <use id="u" x="10" y="20" href="#template"/>
    </svg>
  )");

  auto use = QueryElement<SVGUseElement>(document, "#u");
  EXPECT_EQ(use.x(), Lengthd(10));
  EXPECT_EQ(use.y(), Lengthd(20));
  EXPECT_EQ(use.href(), "#template");
}

// --- ClipPath attributes ---

TEST(AttributeParserTest, ClipPathUnitsUserSpaceOnUse) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <clipPath id="cp" clipPathUnits="userSpaceOnUse">
          <rect width="50" height="50"/>
        </clipPath>
      </defs>
    </svg>
  )");

  auto clipPath = QueryElement<SVGClipPathElement>(document, "#cp");
  EXPECT_EQ(clipPath.clipPathUnits(), ClipPathUnits::UserSpaceOnUse);
}

TEST(AttributeParserTest, ClipPathUnitsObjectBoundingBox) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <clipPath id="cp" clipPathUnits="objectBoundingBox">
          <rect width="1" height="1"/>
        </clipPath>
      </defs>
    </svg>
  )");

  auto clipPath = QueryElement<SVGClipPathElement>(document, "#cp");
  EXPECT_EQ(clipPath.clipPathUnits(), ClipPathUnits::ObjectBoundingBox);
}

// --- Pattern attributes ---

TEST(AttributeParserTest, PatternAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <pattern id="p" x="0" y="0" width="20" height="20"
                 patternUnits="userSpaceOnUse">
          <rect width="10" height="10" fill="red"/>
        </pattern>
      </defs>
    </svg>
  )");

  auto pattern = QueryElement<SVGPatternElement>(document, "#p");
  EXPECT_EQ(pattern.x(), Lengthd(0));
  EXPECT_EQ(pattern.y(), Lengthd(0));
  ASSERT_TRUE(pattern.width().has_value());
  EXPECT_EQ(pattern.width().value(), Lengthd(20));
  ASSERT_TRUE(pattern.height().has_value());
  EXPECT_EQ(pattern.height().value(), Lengthd(20));
  EXPECT_EQ(pattern.patternUnits(), PatternUnits::UserSpaceOnUse);
}

// --- Marker attributes ---

TEST(AttributeParserTest, MarkerAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <marker id="m" markerWidth="12" markerHeight="8" refX="6" refY="4">
          <path d="M0,0 L12,4 L0,8 Z"/>
        </marker>
      </defs>
    </svg>
  )");

  auto marker = QueryElement<SVGMarkerElement>(document, "#m");
  EXPECT_THAT(marker.markerWidth(), DoubleNear(12.0, 0.001));
  EXPECT_THAT(marker.markerHeight(), DoubleNear(8.0, 0.001));
  EXPECT_THAT(marker.refX(), DoubleNear(6.0, 0.001));
  EXPECT_THAT(marker.refY(), DoubleNear(4.0, 0.001));
}

// --- viewBox attribute ---

TEST(AttributeParserTest, SvgViewBox) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="10 20 300 400">
    </svg>
  )");

  auto svg = document.svgElement();
  auto viewBox = svg.viewBox();
  ASSERT_TRUE(viewBox.has_value());
  EXPECT_NEAR(viewBox->topLeft.x, 10.0, 0.001);
  EXPECT_NEAR(viewBox->topLeft.y, 20.0, 0.001);
  EXPECT_NEAR(viewBox->bottomRight.x, 310.0, 0.001);
  EXPECT_NEAR(viewBox->bottomRight.y, 420.0, 0.001);
}

// --- Length units ---

TEST(AttributeParserTest, LengthUnitPx) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10px" y="0" width="50px" height="50"/>
    </svg>
  )");

  auto rect = QueryElement<SVGRectElement>(document, "#r");
  EXPECT_EQ(rect.x(), Lengthd(10, Lengthd::Unit::Px));
}

TEST(AttributeParserTest, LengthUnitEm) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="2em" y="0" width="50" height="50"/>
    </svg>
  )");

  auto rect = QueryElement<SVGRectElement>(document, "#r");
  EXPECT_EQ(rect.x(), Lengthd(2, Lengthd::Unit::Em));
}

TEST(AttributeParserTest, LengthUnitCm) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="3cm" y="0" width="50" height="50"/>
    </svg>
  )");

  auto rect = QueryElement<SVGRectElement>(document, "#r");
  EXPECT_EQ(rect.x(), Lengthd(3, Lengthd::Unit::Cm));
}

// --- Invalid attribute values ---

TEST(AttributeParserTest, InvalidLengthIgnored) {
  std::vector<ParseError> warnings;
  auto maybeResult = SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="abc" y="0" width="50" height="50"/>
    </svg>
  )", &warnings);
  ASSERT_TRUE(maybeResult.hasResult());

  auto document = std::move(maybeResult).result();
  auto rect = QueryElement<SVGRectElement>(document, "#r");
  // Invalid value should be ignored, falling back to default (0).
  EXPECT_EQ(rect.x(), Lengthd(0));
}

TEST(AttributeParserTest, ExtraDataAfterLength) {
  std::vector<ParseError> warnings;
  auto maybeResult = SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10xyz" y="0" width="50" height="50"/>
    </svg>
  )", &warnings);
  ASSERT_TRUE(maybeResult.hasResult());

  auto document = std::move(maybeResult).result();
  auto rect = QueryElement<SVGRectElement>(document, "#r");
  // Extra data after number should cause the value to be ignored.
  EXPECT_EQ(rect.x(), Lengthd(0));
}

// --- Generic attributes (id, class) ---

TEST(AttributeParserTest, IdAndClassAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="myRect" class="highlight" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#myRect");
  ASSERT_TRUE(element.has_value());
  EXPECT_EQ(element->id(), "myRect");
  EXPECT_EQ(element->className(), "highlight");
}

TEST(AttributeParserTest, ClassSelector) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect class="foo" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector(".foo");
  ASSERT_TRUE(element.has_value());
}

// --- Polygon and polyline ---

TEST(AttributeParserTest, PolygonPoints) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <polygon id="p" points="50,5 90,90 10,90"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  // Polygon should be parseable without error.
}

TEST(AttributeParserTest, PolylinePoints) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <polyline id="p" points="10,10 50,50 90,10"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
}

// --- SVG root attributes ---

TEST(AttributeParserTest, SvgWidthHeight) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="400" height="300" viewBox="0 0 400 300">
    </svg>
  )");

  auto svg = document.svgElement();
  ASSERT_TRUE(svg.width().has_value());
  EXPECT_EQ(svg.width().value(), Lengthd(400));
  ASSERT_TRUE(svg.height().has_value());
  EXPECT_EQ(svg.height().value(), Lengthd(300));
}

// --- xlink:href backward compat ---

TEST(AttributeParserTest, XlinkHrefOnUse) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
         viewBox="0 0 100 100">
      <defs>
        <rect id="r" width="50" height="50"/>
      </defs>
      <use id="u" xlink:href="#r"/>
    </svg>
  )");

  auto use = QueryElement<SVGUseElement>(document, "#u");
  EXPECT_EQ(use.href(), "#r");
}

// --- Negative numbers ---

TEST(AttributeParserTest, NegativeCoordinates) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="-50 -50 200 200">
      <rect id="r" x="-10" y="-20" width="30" height="40"/>
    </svg>
  )");

  auto rect = QueryElement<SVGRectElement>(document, "#r");
  EXPECT_EQ(rect.x(), Lengthd(-10));
  EXPECT_EQ(rect.y(), Lengthd(-20));
}

// --- Floating point attributes ---

TEST(AttributeParserTest, FloatingPointValues) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <circle id="c" cx="12.5" cy="34.75" r="6.25"/>
    </svg>
  )");

  auto circle = QueryElement<SVGCircleElement>(document, "#c");
  EXPECT_EQ(circle.cx(), Lengthd(12.5));
  EXPECT_EQ(circle.cy(), Lengthd(34.75));
  EXPECT_EQ(circle.r(), Lengthd(6.25));
}

}  // namespace donner::svg::parser
