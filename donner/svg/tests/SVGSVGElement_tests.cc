#include "donner/svg/SVGSVGElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGCircleElement.h"  // For a negative cast test.
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;
using testing::Eq;
using testing::Ne;
using testing::Optional;

namespace donner::svg {
namespace {

/// Parse a complete SVG string directly so we can test the root element's parsed attributes.
SVGDocument parseSvg(std::string_view input) {
  ParseWarningSink warningSink;
  auto maybeResult = parser::SVGParser::ParseSVG(input, warningSink);
  EXPECT_THAT(maybeResult, NoParseError());
  return std::move(maybeResult).result();
}

// Helper matchers to compare SVG element attributes.
auto XEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("x", &SVGSVGElement::x, LengthIs(valueMatcher, unitMatcher));
}

auto YEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("y", &SVGSVGElement::y, LengthIs(valueMatcher, unitMatcher));
}

auto WidthEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("width", &SVGSVGElement::width,
                           Optional(LengthIs(valueMatcher, unitMatcher)));
}

auto HeightEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("height", &SVGSVGElement::height,
                           Optional(LengthIs(valueMatcher, unitMatcher)));
}

/// Matcher that extracts the root SVG element from a document and applies a given matcher.
MATCHER_P(SvgHas, matchers, "") {
  return testing::ExplainMatchResult(matchers, arg.svgElement(), result_listener);
}

}  // namespace

// --------------------------------------------------------------------------
// Default values (using instantiateSubtree, which sets width/height on the wrapper SVG).
// --------------------------------------------------------------------------

TEST(SVGSVGElementTests, Defaults) {
  // instantiateSubtree wraps content in <svg width="16" height="16">.
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.x(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(svg.y(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(svg.viewBox(), Eq(std::nullopt));
  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGSVGElementTests, DefaultsWithMatchers) {
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");

  EXPECT_THAT(document,
              SvgHas(AllOf(XEq(0.0, Lengthd::Unit::None), YEq(0.0, Lengthd::Unit::None))));
}

// --------------------------------------------------------------------------
// Parsing attributes from XML.
// --------------------------------------------------------------------------

TEST(SVGSVGElementTests, ParseWidthAndHeight) {
  auto document =
      parseSvg(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50" />)");

  EXPECT_THAT(document,
              SvgHas(AllOf(XEq(0.0, Lengthd::Unit::None),  //
                           YEq(0.0, Lengthd::Unit::None),  //
                           WidthEq(100.0, Lengthd::Unit::None),
                           HeightEq(50.0, Lengthd::Unit::None))));
}

TEST(SVGSVGElementTests, ParseXAndY) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" x="10" y="20" width="100" height="100" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.x(), LengthIs(10.0, Lengthd::Unit::None));
  EXPECT_THAT(svg.y(), LengthIs(20.0, Lengthd::Unit::None));
}

TEST(SVGSVGElementTests, ParseAllAttributes) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" x="5" y="10" width="200" height="100" />)");

  EXPECT_THAT(document,
              SvgHas(AllOf(XEq(5.0, Lengthd::Unit::None),  //
                           YEq(10.0, Lengthd::Unit::None),
                           WidthEq(200.0, Lengthd::Unit::None),
                           HeightEq(100.0, Lengthd::Unit::None))));
}

// --------------------------------------------------------------------------
// Casting.
// --------------------------------------------------------------------------

TEST(SVGSVGElementTests, Cast) {
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");
  auto svg = document.svgElement();

  EXPECT_TRUE(svg.isa<SVGSVGElement>());
  EXPECT_THAT(svg.tryCast<SVGElement>(), Ne(std::nullopt));
  EXPECT_THAT(svg.tryCast<SVGGraphicsElement>(), Ne(std::nullopt));
  EXPECT_THAT(svg.tryCast<SVGSVGElement>(), Ne(std::nullopt));
  // Ensure that an unrelated type does not match.
  EXPECT_THAT(svg.tryCast<SVGCircleElement>(), Eq(std::nullopt));
}

// --------------------------------------------------------------------------
// Setting values programmatically.
// --------------------------------------------------------------------------

TEST(SVGSVGElementTests, SetX) {
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.x(), LengthIs(0.0, Lengthd::Unit::None));

  svg.setX(Lengthd(25.0, Lengthd::Unit::Px));
  EXPECT_THAT(svg.x(), LengthIs(25.0, Lengthd::Unit::Px));

  svg.setX(Lengthd(50.0, Lengthd::Unit::Percent));
  EXPECT_THAT(svg.x(), LengthIs(50.0, Lengthd::Unit::Percent));
}

TEST(SVGSVGElementTests, SetY) {
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.y(), LengthIs(0.0, Lengthd::Unit::None));

  svg.setY(Lengthd(30.0, Lengthd::Unit::Em));
  EXPECT_THAT(svg.y(), LengthIs(30.0, Lengthd::Unit::Em));
}

TEST(SVGSVGElementTests, SetWidth) {
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");
  auto svg = document.svgElement();

  svg.setWidth(Lengthd(300.0, Lengthd::Unit::Px));
  ASSERT_TRUE(svg.width().has_value());
  EXPECT_THAT(*svg.width(), LengthIs(300.0, Lengthd::Unit::Px));

  // Update width to a different value.
  svg.setWidth(Lengthd(500.0, Lengthd::Unit::Em));
  ASSERT_TRUE(svg.width().has_value());
  EXPECT_THAT(*svg.width(), LengthIs(500.0, Lengthd::Unit::Em));
}

TEST(SVGSVGElementTests, SetHeight) {
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");
  auto svg = document.svgElement();

  svg.setHeight(Lengthd(150.0, Lengthd::Unit::None));
  ASSERT_TRUE(svg.height().has_value());
  EXPECT_THAT(*svg.height(), LengthIs(150.0, Lengthd::Unit::None));

  // Update height to a different value.
  svg.setHeight(Lengthd(250.0, Lengthd::Unit::Percent));
  ASSERT_TRUE(svg.height().has_value());
  EXPECT_THAT(*svg.height(), LengthIs(250.0, Lengthd::Unit::Percent));
}

TEST(SVGSVGElementTests, UpdateCoordinates) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" x="10" y="20" width="300" height="200" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.x(), LengthIs(10.0, Lengthd::Unit::None));
  EXPECT_THAT(svg.y(), LengthIs(20.0, Lengthd::Unit::None));
  ASSERT_TRUE(svg.width().has_value());
  EXPECT_THAT(*svg.width(), LengthIs(300.0, Lengthd::Unit::None));
  ASSERT_TRUE(svg.height().has_value());
  EXPECT_THAT(*svg.height(), LengthIs(200.0, Lengthd::Unit::None));

  // Update all coordinates.
  svg.setX(Lengthd(15.0, Lengthd::Unit::Px));
  svg.setY(Lengthd(25.0, Lengthd::Unit::Px));
  svg.setWidth(Lengthd(400.0, Lengthd::Unit::Px));
  svg.setHeight(Lengthd(350.0, Lengthd::Unit::Px));

  EXPECT_THAT(svg.x(), LengthIs(15.0, Lengthd::Unit::Px));
  EXPECT_THAT(svg.y(), LengthIs(25.0, Lengthd::Unit::Px));
  ASSERT_TRUE(svg.width().has_value());
  EXPECT_THAT(*svg.width(), LengthIs(400.0, Lengthd::Unit::Px));
  ASSERT_TRUE(svg.height().has_value());
  EXPECT_THAT(*svg.height(), LengthIs(350.0, Lengthd::Unit::Px));
}

// --------------------------------------------------------------------------
// Width/height with different units.
// --------------------------------------------------------------------------

TEST(SVGSVGElementTests, WidthHeightWithPxUnits) {
  auto document =
      parseSvg(R"(<svg xmlns="http://www.w3.org/2000/svg" width="50px" height="75px" />)");
  auto svg = document.svgElement();

  ASSERT_TRUE(svg.width().has_value());
  EXPECT_THAT(*svg.width(), LengthIs(50.0, Lengthd::Unit::Px));

  ASSERT_TRUE(svg.height().has_value());
  EXPECT_THAT(*svg.height(), LengthIs(75.0, Lengthd::Unit::Px));
}

TEST(SVGSVGElementTests, WidthHeightWithPercentUnits) {
  auto document =
      parseSvg(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100%" height="50%" />)");
  auto svg = document.svgElement();

  ASSERT_TRUE(svg.width().has_value());
  EXPECT_THAT(*svg.width(), LengthIs(100.0, Lengthd::Unit::Percent));

  ASSERT_TRUE(svg.height().has_value());
  EXPECT_THAT(*svg.height(), LengthIs(50.0, Lengthd::Unit::Percent));
}

TEST(SVGSVGElementTests, WidthHeightWithEmUnits) {
  auto document =
      parseSvg(R"(<svg xmlns="http://www.w3.org/2000/svg" width="10em" height="5em" />)");
  auto svg = document.svgElement();

  ASSERT_TRUE(svg.width().has_value());
  EXPECT_THAT(*svg.width(), LengthIs(10.0, Lengthd::Unit::Em));

  ASSERT_TRUE(svg.height().has_value());
  EXPECT_THAT(*svg.height(), LengthIs(5.0, Lengthd::Unit::Em));
}

TEST(SVGSVGElementTests, XYWithPxUnits) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" x="10px" y="20px" width="100" height="100" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.x(), LengthIs(10.0, Lengthd::Unit::Px));
  EXPECT_THAT(svg.y(), LengthIs(20.0, Lengthd::Unit::Px));
}

TEST(SVGSVGElementTests, XYWithPercentUnits) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" x="10%" y="20%" width="100" height="100" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.x(), LengthIs(10.0, Lengthd::Unit::Percent));
  EXPECT_THAT(svg.y(), LengthIs(20.0, Lengthd::Unit::Percent));
}

// --------------------------------------------------------------------------
// viewBox.
// --------------------------------------------------------------------------

TEST(SVGSVGElementTests, ViewBoxParsing) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50" viewBox="0 0 100 50" />)");
  auto svg = document.svgElement();

  ASSERT_TRUE(svg.viewBox().has_value());
  const auto box = *svg.viewBox();
  EXPECT_DOUBLE_EQ(box.topLeft.x, 0.0);
  EXPECT_DOUBLE_EQ(box.topLeft.y, 0.0);
  EXPECT_DOUBLE_EQ(box.width(), 100.0);
  EXPECT_DOUBLE_EQ(box.height(), 50.0);
}

TEST(SVGSVGElementTests, ViewBoxWithOffset) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="300" height="200" viewBox="10 20 300 200" />)");
  auto svg = document.svgElement();

  ASSERT_TRUE(svg.viewBox().has_value());
  const auto box = *svg.viewBox();
  EXPECT_DOUBLE_EQ(box.topLeft.x, 10.0);
  EXPECT_DOUBLE_EQ(box.topLeft.y, 20.0);
  EXPECT_DOUBLE_EQ(box.width(), 300.0);
  EXPECT_DOUBLE_EQ(box.height(), 200.0);
}

TEST(SVGSVGElementTests, ViewBoxNotSet) {
  auto document =
      parseSvg(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.viewBox(), Eq(std::nullopt));
}

TEST(SVGSVGElementTests, SetViewBox) {
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.viewBox(), Eq(std::nullopt));

  svg.setViewBox(Box2d::FromXYWH(0, 0, 200, 100));
  ASSERT_TRUE(svg.viewBox().has_value());
  const auto box = *svg.viewBox();
  EXPECT_DOUBLE_EQ(box.topLeft.x, 0.0);
  EXPECT_DOUBLE_EQ(box.topLeft.y, 0.0);
  EXPECT_DOUBLE_EQ(box.width(), 200.0);
  EXPECT_DOUBLE_EQ(box.height(), 100.0);
}

TEST(SVGSVGElementTests, SetViewBoxConvenience) {
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");
  auto svg = document.svgElement();

  svg.setViewBox(5.0, 10.0, 400.0, 300.0);
  ASSERT_TRUE(svg.viewBox().has_value());
  const auto box = *svg.viewBox();
  EXPECT_DOUBLE_EQ(box.topLeft.x, 5.0);
  EXPECT_DOUBLE_EQ(box.topLeft.y, 10.0);
  EXPECT_DOUBLE_EQ(box.width(), 400.0);
  EXPECT_DOUBLE_EQ(box.height(), 300.0);
}

TEST(SVGSVGElementTests, ClearViewBox) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100" />)");
  auto svg = document.svgElement();

  ASSERT_TRUE(svg.viewBox().has_value());

  svg.setViewBox(std::nullopt);
  EXPECT_THAT(svg.viewBox(), Eq(std::nullopt));
}

// --------------------------------------------------------------------------
// preserveAspectRatio.
// --------------------------------------------------------------------------

TEST(SVGSVGElementTests, PreserveAspectRatioDefault) {
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGSVGElementTests, PreserveAspectRatioXMidYMidMeet) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" preserveAspectRatio="xMidYMid meet" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGSVGElementTests, PreserveAspectRatioXMinYMinSlice) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" preserveAspectRatio="xMinYMin slice" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMin,
                                     PreserveAspectRatio::MeetOrSlice::Slice}));
}

TEST(SVGSVGElementTests, PreserveAspectRatioNone) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" preserveAspectRatio="none" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::None,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGSVGElementTests, PreserveAspectRatioXMaxYMax) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" preserveAspectRatio="xMaxYMax meet" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMaxYMax,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGSVGElementTests, PreserveAspectRatioXMinYMax) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" preserveAspectRatio="xMinYMax slice" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMax,
                                     PreserveAspectRatio::MeetOrSlice::Slice}));
}

TEST(SVGSVGElementTests, PreserveAspectRatioXMaxYMid) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" preserveAspectRatio="xMaxYMid meet" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMaxYMid,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGSVGElementTests, PreserveAspectRatioXMidYMin) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" preserveAspectRatio="xMidYMin meet" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMin,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGSVGElementTests, PreserveAspectRatioXMinYMid) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" preserveAspectRatio="xMinYMid slice" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMid,
                                     PreserveAspectRatio::MeetOrSlice::Slice}));
}

TEST(SVGSVGElementTests, PreserveAspectRatioXMidYMax) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" preserveAspectRatio="xMidYMax meet" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMax,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGSVGElementTests, SetPreserveAspectRatio) {
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");
  auto svg = document.svgElement();

  svg.setPreserveAspectRatio(
      PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMin,
                           PreserveAspectRatio::MeetOrSlice::Slice});

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMin,
                                     PreserveAspectRatio::MeetOrSlice::Slice}));
}

TEST(SVGSVGElementTests, SetPreserveAspectRatioNone) {
  auto document = instantiateSubtree(R"(<rect width="1" height="1"/>)");
  auto svg = document.svgElement();

  svg.setPreserveAspectRatio(PreserveAspectRatio::None());

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::None,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

// --------------------------------------------------------------------------
// Full attribute combination.
// --------------------------------------------------------------------------

TEST(SVGSVGElementTests, FullAttributeCombination) {
  auto document = parseSvg(
      R"(<svg xmlns="http://www.w3.org/2000/svg" x="5" y="10" width="800" height="600" viewBox="0 0 400 300" preserveAspectRatio="xMinYMin slice" />)");
  auto svg = document.svgElement();

  EXPECT_THAT(svg.x(), LengthIs(5.0, Lengthd::Unit::None));
  EXPECT_THAT(svg.y(), LengthIs(10.0, Lengthd::Unit::None));

  ASSERT_TRUE(svg.width().has_value());
  EXPECT_THAT(*svg.width(), LengthIs(800.0, Lengthd::Unit::None));

  ASSERT_TRUE(svg.height().has_value());
  EXPECT_THAT(*svg.height(), LengthIs(600.0, Lengthd::Unit::None));

  ASSERT_TRUE(svg.viewBox().has_value());
  const auto box = *svg.viewBox();
  EXPECT_DOUBLE_EQ(box.topLeft.x, 0.0);
  EXPECT_DOUBLE_EQ(box.topLeft.y, 0.0);
  EXPECT_DOUBLE_EQ(box.width(), 400.0);
  EXPECT_DOUBLE_EQ(box.height(), 300.0);

  EXPECT_THAT(svg.preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMin,
                                     PreserveAspectRatio::MeetOrSlice::Slice}));
}

}  // namespace donner::svg
