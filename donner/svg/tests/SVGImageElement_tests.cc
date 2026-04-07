#include "donner/svg/SVGImageElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;
using testing::Eq;
using testing::Ne;
using testing::Optional;

namespace donner::svg {
namespace {

auto XEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("x", &SVGImageElement::x, LengthIs(valueMatcher, unitMatcher));
}

auto YEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("y", &SVGImageElement::y, LengthIs(valueMatcher, unitMatcher));
}

auto WidthEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("width", &SVGImageElement::width,
                           Optional(LengthIs(valueMatcher, unitMatcher)));
}

auto HeightEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("height", &SVGImageElement::height,
                           Optional(LengthIs(valueMatcher, unitMatcher)));
}

auto WidthIsAuto() {
  return testing::Property("width", &SVGImageElement::width, Eq(std::nullopt));
}

auto HeightIsAuto() {
  return testing::Property("height", &SVGImageElement::height, Eq(std::nullopt));
}

MATCHER_P(ImageHas, matchers, "") {
  return testing::ExplainMatchResult(matchers, arg.element, result_listener);
}

// ---------------------------------------------------------------------------
// Type casting
// ---------------------------------------------------------------------------

TEST(SVGImageElementTests, CreateAndCast) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>("<image />");

  EXPECT_THAT(img->tryCast<SVGGraphicsElement>(), Ne(std::nullopt));
  EXPECT_THAT(img->tryCast<SVGImageElement>(), Ne(std::nullopt));
}

// ---------------------------------------------------------------------------
// Default values
// ---------------------------------------------------------------------------

TEST(SVGImageElementTests, Defaults) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>("<image />");

  EXPECT_THAT(img->href(), Eq(""));
  EXPECT_THAT(img->x(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(img->y(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(img->width(), Eq(std::nullopt));
  EXPECT_THAT(img->height(), Eq(std::nullopt));
  EXPECT_THAT(img->preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGImageElementTests, DefaultsWithMatchers) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGImageElement>("<image />"),
              ImageHas(AllOf(XEq(0.0, Lengthd::Unit::None),  //
                             YEq(0.0, Lengthd::Unit::None),  //
                             WidthIsAuto(),                   //
                             HeightIsAuto())));
}

// ---------------------------------------------------------------------------
// Parsing from XML
// ---------------------------------------------------------------------------

TEST(SVGImageElementTests, ParseBasicAttributes) {
  EXPECT_THAT(
      instantiateSubtreeElementAs<SVGImageElement>(
          R"(<image href="test.png" x="10" y="20" width="100" height="50" />)"),
      ImageHas(AllOf(XEq(10.0, Lengthd::Unit::None),       //
                     YEq(20.0, Lengthd::Unit::None),       //
                     WidthEq(100.0, Lengthd::Unit::None),  //
                     HeightEq(50.0, Lengthd::Unit::None))));
}

TEST(SVGImageElementTests, ParseHref) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>(
      R"(<image href="test.png" />)");

  EXPECT_THAT(img->href(), Eq("test.png"));
}

TEST(SVGImageElementTests, ParseXlinkHref) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>(
      R"(<image xlink:href="legacy.png" />)");

  EXPECT_THAT(img->href(), Eq("legacy.png"));
}

TEST(SVGImageElementTests, ParseDataUri) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>(
      R"(<image href="data:image/png;base64,iVBORw0KGgo=" />)");

  EXPECT_THAT(img->href(), Eq("data:image/png;base64,iVBORw0KGgo="));
}

TEST(SVGImageElementTests, ParseWidthHeightOmitted) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>(
      R"(<image href="test.png" x="5" y="10" />)");

  EXPECT_THAT(img->x(), LengthIs(5.0, Lengthd::Unit::None));
  EXPECT_THAT(img->y(), LengthIs(10.0, Lengthd::Unit::None));
  EXPECT_THAT(img->width(), Eq(std::nullopt));
  EXPECT_THAT(img->height(), Eq(std::nullopt));
}

// ---------------------------------------------------------------------------
// Width/height with units
// ---------------------------------------------------------------------------

TEST(SVGImageElementTests, UnitsOnDimensions) {
  EXPECT_THAT(
      instantiateSubtreeElementAs<SVGImageElement>(
          R"(<image x="50px" y="3em" width="30pt" height="2cm" />)"),
      ImageHas(AllOf(XEq(50.0, Lengthd::Unit::Px),      //
                     YEq(3.0, Lengthd::Unit::Em),        //
                     WidthEq(30.0, Lengthd::Unit::Pt),   //
                     HeightEq(2.0, Lengthd::Unit::Cm))));
}

TEST(SVGImageElementTests, PercentageUnits) {
  EXPECT_THAT(
      instantiateSubtreeElementAs<SVGImageElement>(
          R"(<image width="50%" height="75%" />)"),
      ImageHas(AllOf(WidthEq(50.0, Lengthd::Unit::Percent),   //
                     HeightEq(75.0, Lengthd::Unit::Percent))));
}

// ---------------------------------------------------------------------------
// Setting values programmatically
// ---------------------------------------------------------------------------

TEST(SVGImageElementTests, SetHref) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>("<image />");

  img->setHref("new_image.png");
  EXPECT_THAT(img->href(), Eq("new_image.png"));

  img->setHref("another.svg");
  EXPECT_THAT(img->href(), Eq("another.svg"));
}

TEST(SVGImageElementTests, SetXY) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>("<image />");

  img->setX(Lengthd(15.0, Lengthd::Unit::Px));
  img->setY(Lengthd(25.0, Lengthd::Unit::Em));

  EXPECT_THAT(img->x(), LengthIs(15.0, Lengthd::Unit::Px));
  EXPECT_THAT(img->y(), LengthIs(25.0, Lengthd::Unit::Em));
}

TEST(SVGImageElementTests, SetWidthHeight) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>("<image />");

  // Initially auto.
  EXPECT_THAT(img->width(), Eq(std::nullopt));
  EXPECT_THAT(img->height(), Eq(std::nullopt));

  // Set explicit values.
  img->setWidth(Lengthd(200.0, Lengthd::Unit::None));
  img->setHeight(Lengthd(150.0, Lengthd::Unit::Pt));

  ASSERT_TRUE(img->width().has_value());
  ASSERT_TRUE(img->height().has_value());
  EXPECT_THAT(*img->width(), LengthIs(200.0, Lengthd::Unit::None));
  EXPECT_THAT(*img->height(), LengthIs(150.0, Lengthd::Unit::Pt));

  // Reset back to auto.
  img->setWidth(std::nullopt);
  img->setHeight(std::nullopt);

  EXPECT_THAT(img->width(), Eq(std::nullopt));
  EXPECT_THAT(img->height(), Eq(std::nullopt));
}

TEST(SVGImageElementTests, SetOverridesParsed) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>(
      R"(<image href="old.png" x="1" y="2" width="3" height="4" />)");

  img->setHref("replaced.png");
  img->setX(Lengthd(99.0, Lengthd::Unit::None));
  img->setY(Lengthd(88.0, Lengthd::Unit::None));
  img->setWidth(Lengthd(77.0, Lengthd::Unit::None));
  img->setHeight(Lengthd(66.0, Lengthd::Unit::None));

  EXPECT_THAT(img->href(), Eq("replaced.png"));
  EXPECT_THAT(img->x(), LengthIs(99.0, Lengthd::Unit::None));
  EXPECT_THAT(img->y(), LengthIs(88.0, Lengthd::Unit::None));
  ASSERT_TRUE(img->width().has_value());
  ASSERT_TRUE(img->height().has_value());
  EXPECT_THAT(*img->width(), LengthIs(77.0, Lengthd::Unit::None));
  EXPECT_THAT(*img->height(), LengthIs(66.0, Lengthd::Unit::None));
}

// ---------------------------------------------------------------------------
// preserveAspectRatio
// ---------------------------------------------------------------------------

TEST(SVGImageElementTests, PreserveAspectRatioDefault) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>("<image />");

  EXPECT_THAT(img->preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGImageElementTests, PreserveAspectRatioParsedNone) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>(
      R"(<image preserveAspectRatio="none" />)");

  EXPECT_THAT(img->preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::None,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGImageElementTests, PreserveAspectRatioParsedSlice) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>(
      R"(<image preserveAspectRatio="xMinYMin slice" />)");

  EXPECT_THAT(img->preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMin,
                                     PreserveAspectRatio::MeetOrSlice::Slice}));
}

TEST(SVGImageElementTests, PreserveAspectRatioParsedMeet) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>(
      R"(<image preserveAspectRatio="xMaxYMax meet" />)");

  EXPECT_THAT(img->preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMaxYMax,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

TEST(SVGImageElementTests, PreserveAspectRatioSet) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>("<image />");

  img->setPreserveAspectRatio(
      PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMax,
                           PreserveAspectRatio::MeetOrSlice::Slice});

  EXPECT_THAT(img->preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMax,
                                     PreserveAspectRatio::MeetOrSlice::Slice}));
}

TEST(SVGImageElementTests, PreserveAspectRatioSetOverridesParsed) {
  auto img = instantiateSubtreeElementAs<SVGImageElement>(
      R"(<image preserveAspectRatio="xMinYMin slice" />)");

  img->setPreserveAspectRatio(PreserveAspectRatio::None());

  EXPECT_THAT(img->preserveAspectRatio(),
              Eq(PreserveAspectRatio{PreserveAspectRatio::Align::None,
                                     PreserveAspectRatio::MeetOrSlice::Meet}));
}

// ---------------------------------------------------------------------------
// Create via SVGDocument API
// ---------------------------------------------------------------------------

TEST(SVGImageElementTests, CreateViaDocument) {
  SVGDocument document;
  auto img = SVGImageElement::Create(document);

  EXPECT_THAT(img.href(), Eq(""));
  EXPECT_THAT(img.x(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(img.y(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(img.width(), Eq(std::nullopt));
  EXPECT_THAT(img.height(), Eq(std::nullopt));

  img.setHref("created.png");
  img.setX(Lengthd(10.0, Lengthd::Unit::None));
  img.setY(Lengthd(20.0, Lengthd::Unit::None));
  img.setWidth(Lengthd(100.0, Lengthd::Unit::None));
  img.setHeight(Lengthd(50.0, Lengthd::Unit::None));

  EXPECT_THAT(img.href(), Eq("created.png"));
  EXPECT_THAT(img.x(), LengthIs(10.0, Lengthd::Unit::None));
  EXPECT_THAT(img.y(), LengthIs(20.0, Lengthd::Unit::None));
  ASSERT_TRUE(img.width().has_value());
  ASSERT_TRUE(img.height().has_value());
  EXPECT_THAT(*img.width(), LengthIs(100.0, Lengthd::Unit::None));
  EXPECT_THAT(*img.height(), LengthIs(50.0, Lengthd::Unit::None));
}

}  // namespace
}  // namespace donner::svg
