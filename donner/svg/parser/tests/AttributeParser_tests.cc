/**
 * @file Tests for AttributeParser: verifies that XML attributes are correctly parsed and applied
 * to SVG elements across all element types.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "donner/base/Length.h"
#include "donner/base/ParseWarningSink.h"
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
#include "donner/svg/SVGTSpanElement.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/parser/SVGParser.h"

using testing::DoubleNear;
using testing::FloatNear;

namespace donner::svg::parser {

namespace {

SVGDocument ParseSVG(std::string_view input) {
  ParseWarningSink parseSink;
  auto maybeResult = SVGParser::ParseSVG(input, parseSink);
  EXPECT_THAT(maybeResult, NoParseError());
  return std::move(maybeResult).result();
}

SVGDocument ParseSVGExperimental(std::string_view input) {
  SVGParser::Options options;
  options.enableExperimental = true;
  ParseWarningSink parseSink;
  auto maybeResult = SVGParser::ParseSVG(input, parseSink, options);
  EXPECT_THAT(maybeResult, NoParseError());
  return std::move(maybeResult).result();
}

template <typename T>
T QueryElement(SVGDocument& document, const char* selector) {
  auto element = document.querySelector(selector);
  EXPECT_TRUE(element.has_value()) << "Missing: " << selector;
  return element->cast<T>();
}

template <typename T>
const T& QueryComponent(SVGDocument& document, const char* selector) {
  auto element = document.querySelector(selector);
  EXPECT_TRUE(element.has_value()) << "Missing: " << selector;
  return element->entityHandle().get<T>();
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

TEST(AttributeParserTest, LinearGradientCommonAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">
      <defs>
        <linearGradient id="base">
          <stop offset="0" stop-color="red"/>
        </linearGradient>
        <linearGradient id="g" gradientUnits="userSpaceOnUse" spreadMethod="reflect"
                        xlink:href="#base"/>
      </defs>
    </svg>
  )");

  auto gradient = QueryElement<SVGLinearGradientElement>(document, "#g");
  EXPECT_EQ(gradient.gradientUnits(), GradientUnits::UserSpaceOnUse);
  EXPECT_EQ(gradient.spreadMethod(), GradientSpreadMethod::Reflect);
  ASSERT_TRUE(gradient.href().has_value());
  EXPECT_EQ(gradient.href().value(), "#base");
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
  auto document = ParseSVGExperimental(R"(
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

TEST(AttributeParserTest, FilterCommonAttributes) {
  auto document = ParseSVGExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <defs>
        <filter id="base">
          <feGaussianBlur stdDeviation="1"/>
        </filter>
        <filter id="f" filterUnits="userSpaceOnUse" primitiveUnits="objectBoundingBox"
                href="#base"/>
      </defs>
    </svg>
  )");

  auto filter = QueryElement<SVGFilterElement>(document, "#f");
  EXPECT_EQ(filter.filterUnits(), FilterUnits::UserSpaceOnUse);
  EXPECT_EQ(filter.primitiveUnits(), PrimitiveUnits::ObjectBoundingBox);
  ASSERT_TRUE(filter.href().has_value());
  EXPECT_EQ(filter.href().value(), "#base");
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

TEST(AttributeParserTest, ImagePreserveAspectRatio) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <image id="img" width="80" height="60" href="test.png"
             preserveAspectRatio="none"/>
    </svg>
  )");

  auto image = QueryElement<SVGImageElement>(document, "#img");
  EXPECT_EQ(image.preserveAspectRatio(), PreserveAspectRatio::None());
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

TEST(AttributeParserTest, PatternCommonAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <defs>
        <pattern id="base" width="10" height="10"/>
        <pattern id="p" width="20" height="20" viewBox="0 0 20 20"
                 preserveAspectRatio="xMaxYMin slice"
                 patternContentUnits="objectBoundingBox" href="#base"/>
      </defs>
    </svg>
  )");

  auto pattern = QueryElement<SVGPatternElement>(document, "#p");
  EXPECT_EQ(pattern.patternContentUnits(), PatternContentUnits::ObjectBoundingBox);
  EXPECT_EQ(pattern.preserveAspectRatio(),
            (PreserveAspectRatio{PreserveAspectRatio::Align::XMaxYMin,
                                 PreserveAspectRatio::MeetOrSlice::Slice}));
  ASSERT_TRUE(pattern.href().has_value());
  EXPECT_EQ(pattern.href().value(), "#base");
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

TEST(AttributeParserTest, MarkerCommonAttributes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <defs>
        <marker id="m" viewBox="0 0 12 8" preserveAspectRatio="xMinYMax meet"
                markerUnits="userSpaceOnUse" orient="auto-start-reverse">
          <path d="M0,0 L12,4 L0,8 Z"/>
        </marker>
      </defs>
    </svg>
  )");

  auto marker = QueryElement<SVGMarkerElement>(document, "#m");
  EXPECT_EQ(marker.markerUnits(), MarkerUnits::UserSpaceOnUse);
  EXPECT_EQ(marker.orient(), MarkerOrient::AutoStartReverse());
  EXPECT_EQ(marker.preserveAspectRatio(),
            (PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMax,
                                 PreserveAspectRatio::MeetOrSlice::Meet}));
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
  ParseWarningSink warningSink;
  auto maybeResult = SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="abc" y="0" width="50" height="50"/>
    </svg>
  )", warningSink);
  ASSERT_TRUE(maybeResult.hasResult());

  auto document = std::move(maybeResult).result();
  auto rect = QueryElement<SVGRectElement>(document, "#r");
  // Invalid value should be ignored, falling back to default (0).
  EXPECT_EQ(rect.x(), Lengthd(0));
}

TEST(AttributeParserTest, ExtraDataAfterLength) {
  ParseWarningSink warningSink;
  auto maybeResult = SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10xyz" y="0" width="50" height="50"/>
    </svg>
  )", warningSink);
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

TEST(AttributeParserTest, ExperimentalFilterPrimitiveComponents) {
  auto document = ParseSVGExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <defs>
        <filter id="f" filterUnits="objectBoundingBox" primitiveUnits="userSpaceOnUse"
                color-interpolation-filters="linearRGB" href="#base">
          <feGaussianBlur id="blur" x="1" y="2" width="3" height="4" in="StrokePaint"
                          result="blurred" stdDeviation="2 3" edgeMode="wrap"/>
          <feComponentTransfer id="ct" in="SourceAlpha" result="transfer">
            <feFuncR id="funcR" type="table" tableValues="0, 0.5 1"/>
            <feFuncG id="funcG" type="discrete"/>
            <feFuncB id="funcB" type="linear" slope="2" intercept="3"/>
            <feFuncA id="funcA" type="gamma" amplitude="4" exponent="5" offset="6"/>
          </feComponentTransfer>
          <feColorMatrix id="cm" type="hueRotate" values="1 2 3"/>
          <feComposite id="comp" operator="arithmetic" in="SourceGraphic" in2="named"
                       k1="1" k2="2" k3="3" k4="4"/>
          <feDropShadow id="shadow" dx="7" dy="8" stdDeviation="9 10"/>
          <feMorphology id="morph" operator="dilate" radius="11 12"/>
          <feDisplacementMap id="disp" scale="13" xChannelSelector="R" yChannelSelector="G"/>
          <feImage id="img" href="test.png" preserveAspectRatio="none"/>
          <feDiffuseLighting id="diffuse" surfaceScale="14" diffuseConstant="-15">
            <feDistantLight id="distant" azimuth="16" elevation="17"/>
          </feDiffuseLighting>
          <feSpecularLighting id="spec" surfaceScale="18" specularConstant="-19"
                              specularExponent="20">
            <fePointLight id="point" x="21" y="22" z="23"/>
          </feSpecularLighting>
          <feSpotLight id="spot" x="24" y="25" z="26" pointsAtX="27" pointsAtY="28"
                       pointsAtZ="29" specularExponent="30" limitingConeAngle="31"/>
          <feConvolveMatrix id="conv" order="2 3" kernelMatrix="1 2 3 4 5 6" divisor="7"
                            bias="8" targetX="9" targetY="10" edgeMode="none"
                            preserveAlpha="true"/>
          <feTurbulence id="turb" baseFrequency="0.1 0.2" numOctaves="3" seed="4"
                        type="fractalNoise" stitchTiles="stitch"/>
          <feTile id="tile" in="FillPaint"/>
          <feOffset id="offset" dx="32" dy="33"/>
          <feMerge id="merge" result="merged">
            <feMergeNode id="mergeNode" in="FillPaint"/>
          </feMerge>
        </filter>
      </defs>
    </svg>
  )");

  auto filter = QueryElement<SVGFilterElement>(document, "#f");
  EXPECT_EQ(filter.filterUnits(), FilterUnits::ObjectBoundingBox);
  EXPECT_EQ(filter.primitiveUnits(), PrimitiveUnits::UserSpaceOnUse);
  ASSERT_TRUE(filter.href().has_value());
  EXPECT_EQ(filter.href().value(), "#base");
  EXPECT_EQ(QueryComponent<components::FilterComponent>(document, "#f").colorInterpolationFilters,
            ColorInterpolationFilters::LinearRGB);

  const auto& blurPrimitive = QueryComponent<components::FilterPrimitiveComponent>(document, "#blur");
  ASSERT_TRUE(blurPrimitive.in.has_value());
  EXPECT_EQ(std::get<components::FilterStandardInput>(blurPrimitive.in->value),
            components::FilterStandardInput::StrokePaint);
  ASSERT_TRUE(blurPrimitive.result.has_value());
  EXPECT_EQ(blurPrimitive.result.value(), "blurred");
  EXPECT_EQ(blurPrimitive.x, Lengthd(1));
  EXPECT_EQ(blurPrimitive.y, Lengthd(2));
  EXPECT_EQ(blurPrimitive.width, Lengthd(3));
  EXPECT_EQ(blurPrimitive.height, Lengthd(4));

  const auto& blur = QueryComponent<components::FEGaussianBlurComponent>(document, "#blur");
  EXPECT_DOUBLE_EQ(blur.stdDeviationX, 2.0);
  EXPECT_DOUBLE_EQ(blur.stdDeviationY, 3.0);
  EXPECT_EQ(blur.edgeMode, components::FEGaussianBlurComponent::EdgeMode::Wrap);

  const auto& componentTransfer =
      QueryComponent<components::FilterPrimitiveComponent>(document, "#ct");
  ASSERT_TRUE(componentTransfer.in.has_value());
  EXPECT_EQ(std::get<components::FilterStandardInput>(componentTransfer.in->value),
            components::FilterStandardInput::SourceAlpha);
  ASSERT_TRUE(componentTransfer.result.has_value());
  EXPECT_EQ(componentTransfer.result.value(), "transfer");

  const auto funcView = document.registry().view<components::FEFuncComponent>();
  const components::FEFuncComponent* funcR = nullptr;
  const components::FEFuncComponent* funcG = nullptr;
  const components::FEFuncComponent* funcB = nullptr;
  const components::FEFuncComponent* funcA = nullptr;
  for (const Entity entity : funcView) {
    const auto& func = funcView.get<components::FEFuncComponent>(entity);
    switch (func.channel) {
      case components::FEFuncComponent::Channel::R:
        funcR = &func;
        break;
      case components::FEFuncComponent::Channel::G:
        funcG = &func;
        break;
      case components::FEFuncComponent::Channel::B:
        funcB = &func;
        break;
      case components::FEFuncComponent::Channel::A:
        funcA = &func;
        break;
    }
  }

  ASSERT_NE(funcR, nullptr);
  EXPECT_EQ(funcR->type, components::FEFuncComponent::FuncType::Table);
  EXPECT_THAT(funcR->tableValues, testing::ElementsAre(0.0, 0.5, 1.0));

  ASSERT_NE(funcG, nullptr);
  EXPECT_EQ(funcG->type, components::FEFuncComponent::FuncType::Discrete);

  ASSERT_NE(funcB, nullptr);
  EXPECT_EQ(funcB->type, components::FEFuncComponent::FuncType::Linear);
  EXPECT_DOUBLE_EQ(funcB->slope, 2.0);
  EXPECT_DOUBLE_EQ(funcB->intercept, 3.0);

  ASSERT_NE(funcA, nullptr);
  EXPECT_EQ(funcA->type, components::FEFuncComponent::FuncType::Gamma);
  EXPECT_DOUBLE_EQ(funcA->amplitude, 4.0);
  EXPECT_DOUBLE_EQ(funcA->exponent, 5.0);
  EXPECT_DOUBLE_EQ(funcA->offset, 6.0);

  const auto& colorMatrix = QueryComponent<components::FEColorMatrixComponent>(document, "#cm");
  EXPECT_EQ(colorMatrix.type, components::FEColorMatrixComponent::Type::HueRotate);
  EXPECT_THAT(colorMatrix.values, testing::ElementsAre(1.0, 2.0, 3.0));

  const auto& compositePrimitive =
      QueryComponent<components::FilterPrimitiveComponent>(document, "#comp");
  ASSERT_TRUE(compositePrimitive.in.has_value());
  EXPECT_EQ(std::get<components::FilterStandardInput>(compositePrimitive.in->value),
            components::FilterStandardInput::SourceGraphic);
  ASSERT_TRUE(compositePrimitive.in2.has_value());
  EXPECT_EQ(std::get<components::FilterInput::Named>(compositePrimitive.in2->value).name, "named");

  const auto& composite = QueryComponent<components::FECompositeComponent>(document, "#comp");
  EXPECT_EQ(composite.op, components::FECompositeComponent::Operator::Arithmetic);
  EXPECT_DOUBLE_EQ(composite.k1, 1.0);
  EXPECT_DOUBLE_EQ(composite.k2, 2.0);
  EXPECT_DOUBLE_EQ(composite.k3, 3.0);
  EXPECT_DOUBLE_EQ(composite.k4, 4.0);

  const auto& shadow = QueryComponent<components::FEDropShadowComponent>(document, "#shadow");
  EXPECT_DOUBLE_EQ(shadow.dx, 7.0);
  EXPECT_DOUBLE_EQ(shadow.dy, 8.0);
  EXPECT_DOUBLE_EQ(shadow.stdDeviationX, 9.0);
  EXPECT_DOUBLE_EQ(shadow.stdDeviationY, 10.0);

  const auto& morphology = QueryComponent<components::FEMorphologyComponent>(document, "#morph");
  EXPECT_EQ(morphology.op, components::FEMorphologyComponent::Operator::Dilate);
  EXPECT_DOUBLE_EQ(morphology.radiusX, 11.0);
  EXPECT_DOUBLE_EQ(morphology.radiusY, 12.0);

  const auto& displacement =
      QueryComponent<components::FEDisplacementMapComponent>(document, "#disp");
  EXPECT_DOUBLE_EQ(displacement.scale, 13.0);
  EXPECT_EQ(displacement.xChannelSelector, components::FEDisplacementMapComponent::Channel::R);
  EXPECT_EQ(displacement.yChannelSelector, components::FEDisplacementMapComponent::Channel::G);

  const auto& image = QueryComponent<components::FEImageComponent>(document, "#img");
  EXPECT_EQ(image.href, "test.png");
  EXPECT_EQ(image.preserveAspectRatio, PreserveAspectRatio::None());
  EXPECT_EQ(QueryComponent<components::ImageComponent>(document, "#img").href, "test.png");

  const auto& diffuse = QueryComponent<components::FEDiffuseLightingComponent>(document, "#diffuse");
  EXPECT_DOUBLE_EQ(diffuse.surfaceScale, 14.0);
  EXPECT_DOUBLE_EQ(diffuse.diffuseConstant, 0.0);

  const auto& distant = QueryComponent<components::LightSourceComponent>(document, "#distant");
  EXPECT_DOUBLE_EQ(distant.azimuth, 16.0);
  EXPECT_DOUBLE_EQ(distant.elevation, 17.0);

  const auto& specular =
      QueryComponent<components::FESpecularLightingComponent>(document, "#spec");
  EXPECT_DOUBLE_EQ(specular.surfaceScale, 18.0);
  EXPECT_DOUBLE_EQ(specular.specularConstant, 0.0);
  EXPECT_DOUBLE_EQ(specular.specularExponent, 20.0);

  const auto& point = QueryComponent<components::LightSourceComponent>(document, "#point");
  EXPECT_DOUBLE_EQ(point.x, 21.0);
  EXPECT_DOUBLE_EQ(point.y, 22.0);
  EXPECT_DOUBLE_EQ(point.z, 23.0);

  const auto& spot = QueryComponent<components::LightSourceComponent>(document, "#spot");
  EXPECT_DOUBLE_EQ(spot.x, 24.0);
  EXPECT_DOUBLE_EQ(spot.y, 25.0);
  EXPECT_DOUBLE_EQ(spot.z, 26.0);
  EXPECT_DOUBLE_EQ(spot.pointsAtX, 27.0);
  EXPECT_DOUBLE_EQ(spot.pointsAtY, 28.0);
  EXPECT_DOUBLE_EQ(spot.pointsAtZ, 29.0);
  EXPECT_DOUBLE_EQ(spot.spotExponent, 30.0);
  ASSERT_TRUE(spot.limitingConeAngle.has_value());
  EXPECT_DOUBLE_EQ(spot.limitingConeAngle.value(), 31.0);

  const auto& convolve = QueryComponent<components::FEConvolveMatrixComponent>(document, "#conv");
  EXPECT_EQ(convolve.orderX, 2);
  EXPECT_EQ(convolve.orderY, 3);
  EXPECT_THAT(convolve.kernelMatrix, testing::ElementsAre(1.0, 2.0, 3.0, 4.0, 5.0, 6.0));
  ASSERT_TRUE(convolve.divisor.has_value());
  EXPECT_DOUBLE_EQ(convolve.divisor.value(), 7.0);
  EXPECT_DOUBLE_EQ(convolve.bias, 8.0);
  ASSERT_TRUE(convolve.targetX.has_value());
  ASSERT_TRUE(convolve.targetY.has_value());
  EXPECT_EQ(convolve.targetX.value(), 9);
  EXPECT_EQ(convolve.targetY.value(), 10);
  EXPECT_EQ(convolve.edgeMode, components::FEConvolveMatrixComponent::EdgeMode::None);
  EXPECT_TRUE(convolve.preserveAlpha);

  const auto& turbulence = QueryComponent<components::FETurbulenceComponent>(document, "#turb");
  EXPECT_DOUBLE_EQ(turbulence.baseFrequencyX, 0.1);
  EXPECT_DOUBLE_EQ(turbulence.baseFrequencyY, 0.2);
  EXPECT_EQ(turbulence.numOctaves, 3);
  EXPECT_DOUBLE_EQ(turbulence.seed, 4.0);
  EXPECT_EQ(turbulence.type, components::FETurbulenceComponent::Type::FractalNoise);
  EXPECT_TRUE(turbulence.stitchTiles);

  const auto& tilePrimitive = QueryComponent<components::FilterPrimitiveComponent>(document, "#tile");
  ASSERT_TRUE(tilePrimitive.in.has_value());
  EXPECT_EQ(std::get<components::FilterStandardInput>(tilePrimitive.in->value),
            components::FilterStandardInput::FillPaint);

  const auto& offset = QueryComponent<components::FEOffsetComponent>(document, "#offset");
  EXPECT_DOUBLE_EQ(offset.dx, 32.0);
  EXPECT_DOUBLE_EQ(offset.dy, 33.0);

  const auto& mergePrimitive =
      QueryComponent<components::FilterPrimitiveComponent>(document, "#merge");
  ASSERT_TRUE(mergePrimitive.result.has_value());
  EXPECT_EQ(mergePrimitive.result.value(), "merged");

  const auto& mergeNode = QueryComponent<components::FEMergeNodeComponent>(document, "#mergeNode");
  ASSERT_TRUE(mergeNode.in.has_value());
  EXPECT_EQ(std::get<components::FilterStandardInput>(mergeNode.in->value),
            components::FilterStandardInput::FillPaint);
}

TEST(AttributeParserTest, ExperimentalFilterPrimitiveVariantMatricesAndWarnings) {
  const std::pair<const char*, components::FEBlendComponent::Mode> blendCases[] = {
      {"normal", components::FEBlendComponent::Mode::Normal},
      {"multiply", components::FEBlendComponent::Mode::Multiply},
      {"screen", components::FEBlendComponent::Mode::Screen},
      {"darken", components::FEBlendComponent::Mode::Darken},
      {"lighten", components::FEBlendComponent::Mode::Lighten},
      {"overlay", components::FEBlendComponent::Mode::Overlay},
      {"color-dodge", components::FEBlendComponent::Mode::ColorDodge},
      {"color-burn", components::FEBlendComponent::Mode::ColorBurn},
      {"hard-light", components::FEBlendComponent::Mode::HardLight},
      {"soft-light", components::FEBlendComponent::Mode::SoftLight},
      {"difference", components::FEBlendComponent::Mode::Difference},
      {"exclusion", components::FEBlendComponent::Mode::Exclusion},
      {"hue", components::FEBlendComponent::Mode::Hue},
      {"saturation", components::FEBlendComponent::Mode::Saturation},
      {"color", components::FEBlendComponent::Mode::Color},
      {"luminosity", components::FEBlendComponent::Mode::Luminosity},
  };

  for (const auto& [value, expected] : blendCases) {
    const std::string xml =
        std::string(R"(<svg xmlns="http://www.w3.org/2000/svg"><defs><filter>)") +
        R"(<feBlend id="blend" mode=")" + value + R"("/></filter></defs></svg>)";
    auto document = ParseSVGExperimental(xml);
    EXPECT_EQ(QueryComponent<components::FEBlendComponent>(document, "#blend").mode, expected);
  }

  const std::pair<const char*, components::FECompositeComponent::Operator> compositeCases[] = {
      {"over", components::FECompositeComponent::Operator::Over},
      {"in", components::FECompositeComponent::Operator::In},
      {"out", components::FECompositeComponent::Operator::Out},
      {"atop", components::FECompositeComponent::Operator::Atop},
      {"xor", components::FECompositeComponent::Operator::Xor},
      {"lighter", components::FECompositeComponent::Operator::Lighter},
      {"arithmetic", components::FECompositeComponent::Operator::Arithmetic},
  };

  for (const auto& [value, expected] : compositeCases) {
    const std::string xml =
        std::string(R"(<svg xmlns="http://www.w3.org/2000/svg"><defs><filter>)") +
        R"(<feComposite id="comp" operator=")" + value + R"("/></filter></defs></svg>)";
    auto document = ParseSVGExperimental(xml);
    EXPECT_EQ(QueryComponent<components::FECompositeComponent>(document, "#comp").op, expected);
  }

  const std::pair<const char*, components::FEColorMatrixComponent::Type> colorMatrixCases[] = {
      {"matrix", components::FEColorMatrixComponent::Type::Matrix},
      {"saturate", components::FEColorMatrixComponent::Type::Saturate},
      {"hueRotate", components::FEColorMatrixComponent::Type::HueRotate},
      {"luminanceToAlpha", components::FEColorMatrixComponent::Type::LuminanceToAlpha},
  };

  for (const auto& [value, expected] : colorMatrixCases) {
    const std::string xml =
        std::string(R"(<svg xmlns="http://www.w3.org/2000/svg"><defs><filter>)") +
        R"(<feColorMatrix id="cm" type=")" + value + R"("/></filter></defs></svg>)";
    auto document = ParseSVGExperimental(xml);
    EXPECT_EQ(QueryComponent<components::FEColorMatrixComponent>(document, "#cm").type, expected);
  }

  const std::pair<const char*, components::FEGaussianBlurComponent::EdgeMode> blurCases[] = {
      {"none", components::FEGaussianBlurComponent::EdgeMode::None},
      {"duplicate", components::FEGaussianBlurComponent::EdgeMode::Duplicate},
      {"wrap", components::FEGaussianBlurComponent::EdgeMode::Wrap},
  };

  for (const auto& [value, expected] : blurCases) {
    const std::string xml =
        std::string(R"(<svg xmlns="http://www.w3.org/2000/svg"><defs><filter>)") +
        R"(<feGaussianBlur id="blur" edgeMode=")" + value + R"("/></filter></defs></svg>)";
    auto document = ParseSVGExperimental(xml);
    EXPECT_EQ(QueryComponent<components::FEGaussianBlurComponent>(document, "#blur").edgeMode,
              expected);
  }

  ParseWarningSink warningSink;
  SVGParser::Options options;
  options.enableExperimental = true;
  auto maybeDocument = SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <defs>
        <filter id="bad-filter" filterUnits="bad" primitiveUnits="bad">
          <feGaussianBlur id="bad-blur-1" stdDeviation="1 2 extra"/>
          <feGaussianBlur id="bad-blur-2" stdDeviation="oops"/>
        </filter>
        <marker id="bad-marker" markerWidth="1px" markerHeight="oops" refX="10px" refY="oops"
                orient="" markerUnits="bad"/>
        <pattern id="bad-pattern" patternUnits="bad" patternContentUnits="bad"
                 viewBox="oops" preserveAspectRatio="bad"/>
      </defs>
    </svg>
  )",
                                         warningSink, options);
  ASSERT_TRUE(maybeDocument.hasResult());
  EXPECT_THAT(warningSink.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("Invalid filterUnits value"))));
  EXPECT_THAT(warningSink.warnings(),
              testing::Contains(testing::Field(
                  &ParseDiagnostic::reason,
                  testing::HasSubstr("Unexpected additional data in stdDeviation"))));
  EXPECT_THAT(warningSink.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("Invalid stdDeviation value"))));
  EXPECT_THAT(warningSink.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("Invalid markerWidth value"))));
  EXPECT_THAT(warningSink.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("Invalid angle value"))));
  EXPECT_THAT(warningSink.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("Invalid patternUnits value"))));
}

TEST(AttributeParserTest, ExperimentalWarningAndFallbackBranches) {
  ParseWarningSink warningSink;
  SVGParser::Options options;
  options.enableExperimental = true;
  auto maybeDocument = SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <defs>
        <linearGradient id="g">
          <stop id="bad-stop-a" offset="oops"/>
          <stop id="bad-stop-b" offset="10% foo"/>
        </linearGradient>
        <filter id="f" color-interpolation-filters="sRGB">
          <feColorMatrix id="cm-empty" values=" "/>
          <feColorMatrix id="cm-invalid" values="1 bad"/>
          <feDisplacementMap id="disp-ba" xChannelSelector="B" yChannelSelector="A"/>
          <feConvolveMatrix id="conv-wrap" kernelMatrix="1 bad" edgeMode="wrap"/>
          <feTurbulence id="turb-default" type="turbulence"/>
          <feImage id="img-invalid" preserveAspectRatio="bad"/>
        </filter>
        <pattern id="p" patternUnits="objectBoundingBox" patternContentUnits="userSpaceOnUse"/>
        <polygon id="poly" points="0,0 10,10 bad"/>
        <polyline id="line" points="0,0 10,10 bad"/>
        <style id="style" type="text/plain"/>
        <symbol id="sym" refX="bad" refY="bad"/>
        <text id="text" lengthAdjust="spacingAndGlyphs" x="1 bad" y="2 bad" dx="3 bad"
              dy="4 bad" rotate="5deg bad">a</text>
        <text><tspan id="span" x="1 bad" y="2 bad" dx="3 bad" dy="4 bad" rotate="5deg bad"
                     textLength="6" lengthAdjust="spacingAndGlyphs">b</tspan></text>
        <marker id="marker" markerUnits="strokeWidth" orient="45deg"/>
      </defs>
      <rect id="r" x="oops" width="10 foo" height="5"/>
    </svg>
  )",
                                         warningSink, options);
  ASSERT_TRUE(maybeDocument.hasResult());
  auto document = std::move(maybeDocument).result();

  EXPECT_EQ(QueryComponent<components::FilterComponent>(document, "#f").colorInterpolationFilters,
            ColorInterpolationFilters::SRGB);
  EXPECT_EQ(QueryComponent<components::FEDisplacementMapComponent>(document, "#disp-ba")
                .xChannelSelector,
            components::FEDisplacementMapComponent::Channel::B);
  EXPECT_EQ(QueryComponent<components::FEDisplacementMapComponent>(document, "#disp-ba")
                .yChannelSelector,
            components::FEDisplacementMapComponent::Channel::A);
  EXPECT_EQ(QueryComponent<components::FEConvolveMatrixComponent>(document, "#conv-wrap").edgeMode,
            components::FEConvolveMatrixComponent::EdgeMode::Wrap);
  EXPECT_EQ(QueryComponent<components::FETurbulenceComponent>(document, "#turb-default").type,
            components::FETurbulenceComponent::Type::Turbulence);

  auto pattern = QueryElement<SVGPatternElement>(document, "#p");
  EXPECT_EQ(pattern.patternUnits(), PatternUnits::ObjectBoundingBox);
  EXPECT_EQ(pattern.patternContentUnits(), PatternContentUnits::UserSpaceOnUse);

  auto marker = QueryElement<SVGMarkerElement>(document, "#marker");
  EXPECT_EQ(marker.markerUnits(), MarkerUnits::StrokeWidth);
  EXPECT_EQ(marker.orient(), MarkerOrient::AngleRadians(MathConstants<double>::kPi / 4.0));

  auto span = document.querySelector("#span");
  ASSERT_TRUE(span.has_value());
  EXPECT_THAT(span->cast<SVGTSpanElement>().textLength(), testing::Optional(Lengthd(6)));
  EXPECT_EQ(span->cast<SVGTSpanElement>().lengthAdjust(), LengthAdjust::SpacingAndGlyphs);

  EXPECT_THAT(warningSink.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("Failed to parse number"))));
  EXPECT_THAT(warningSink.warnings(),
              testing::Contains(testing::Field(
                  &ParseDiagnostic::reason,
                  testing::HasSubstr("Unexpected data at end of attribute"))));
  EXPECT_THAT(warningSink.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("Invalid <style> element type"))));
  EXPECT_THAT(warningSink.warnings(),
              testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                               testing::HasSubstr("Invalid refX value"))));
}

}  // namespace donner::svg::parser
