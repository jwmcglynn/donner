#include "donner/svg/SVGUseElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGCircleElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;
using testing::Eq;
using testing::Ne;
using testing::Optional;

namespace donner::svg {
namespace {

auto XEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("x", &SVGUseElement::x, LengthIs(valueMatcher, unitMatcher));
}

auto YEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("y", &SVGUseElement::y, LengthIs(valueMatcher, unitMatcher));
}

auto WidthEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("width", &SVGUseElement::width,
                           Optional(LengthIs(valueMatcher, unitMatcher)));
}

auto HeightEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("height", &SVGUseElement::height,
                           Optional(LengthIs(valueMatcher, unitMatcher)));
}

auto WidthIsAuto() {
  return testing::Property("width", &SVGUseElement::width, Eq(std::nullopt));
}

auto HeightIsAuto() {
  return testing::Property("height", &SVGUseElement::height, Eq(std::nullopt));
}

auto HrefEq(auto matcher) {
  return testing::Property("href", &SVGUseElement::href, matcher);
}

MATCHER_P(UseHas, matchers, "") {
  return testing::ExplainMatchResult(matchers, arg.element, result_listener);
}

/// Test that a <use> element has the expected default values.
TEST(SVGUseElementTests, Defaults) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGUseElement>(  //
                  "<use />"),
              UseHas(AllOf(XEq(0.0, Lengthd::Unit::None),  //
                           YEq(0.0, Lengthd::Unit::None),  //
                           WidthIsAuto(),                  //
                           HeightIsAuto(),                 //
                           HrefEq(RcString()))));
}

/// Test parsing a <use> element with href and position attributes.
TEST(SVGUseElementTests, ParseWithHref) {
  auto doc = instantiateSubtree(R"(
    <svg>
      <defs>
        <circle id="myCircle" cx="10" cy="10" r="5" />
      </defs>
      <use href="#myCircle" x="20" y="30" />
    </svg>
  )");

  auto maybeUse = doc.querySelector("use");
  ASSERT_THAT(maybeUse, Ne(std::nullopt));
  auto use = maybeUse->cast<SVGUseElement>();

  EXPECT_THAT(use.href(), Eq(RcString("#myCircle")));
  EXPECT_THAT(use.x(), LengthIs(20.0, Lengthd::Unit::None));
  EXPECT_THAT(use.y(), LengthIs(30.0, Lengthd::Unit::None));
  EXPECT_THAT(use.width(), Eq(std::nullopt));
  EXPECT_THAT(use.height(), Eq(std::nullopt));
}

/// Test parsing a <use> element that references a <rect>.
TEST(SVGUseElementTests, HrefToRect) {
  auto doc = instantiateSubtree(R"(
    <svg>
      <defs>
        <rect id="myRect" x="0" y="0" width="50" height="30" />
      </defs>
      <use href="#myRect" x="10" y="15" />
    </svg>
  )");

  auto use = doc.querySelector("use")->cast<SVGUseElement>();
  EXPECT_THAT(use.href(), Eq(RcString("#myRect")));
  EXPECT_THAT(use.x(), LengthIs(10.0, Lengthd::Unit::None));
  EXPECT_THAT(use.y(), LengthIs(15.0, Lengthd::Unit::None));
}

/// Test parsing a <use> element that references a <circle>.
TEST(SVGUseElementTests, HrefToCircle) {
  auto doc = instantiateSubtree(R"(
    <svg>
      <defs>
        <circle id="c1" cx="5" cy="5" r="3" />
      </defs>
      <use href="#c1" />
    </svg>
  )");

  auto use = doc.querySelector("use")->cast<SVGUseElement>();
  EXPECT_THAT(use.href(), Eq(RcString("#c1")));
  EXPECT_THAT(use.x(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(use.y(), LengthIs(0.0, Lengthd::Unit::None));
}

/// Test the legacy xlink:href attribute.
TEST(SVGUseElementTests, XlinkHref) {
  auto doc = instantiateSubtree(R"(
    <svg xmlns:xlink="http://www.w3.org/1999/xlink">
      <defs>
        <rect id="r1" width="10" height="10" />
      </defs>
      <use xlink:href="#r1" x="5" y="5" />
    </svg>
  )");

  auto use = doc.querySelector("use")->cast<SVGUseElement>();
  EXPECT_THAT(use.href(), Eq(RcString("#r1")));
}

/// Test setting href programmatically.
TEST(SVGUseElementTests, SetHref) {
  auto fragment = instantiateSubtreeElementAs<SVGUseElement>("<use />");

  EXPECT_THAT(fragment->href(), Eq(RcString()));

  fragment->setHref(RcString("#newTarget"));
  EXPECT_THAT(fragment->href(), Eq(RcString("#newTarget")));

  fragment->setHref(RcString("#anotherTarget"));
  EXPECT_THAT(fragment->href(), Eq(RcString("#anotherTarget")));
}

constexpr std::string_view kTwoTargets =
    R"(<defs><rect id="a" width="1" height="1"/><rect id="b" width="1" height="1"/></defs>)";

/// Parse a document whose only `<use>` element carries \p useAttributes, and return that use.
SVGUseElement ParseUse(SVGDocument& document, std::string_view useAttributes) {
  document = instantiateSubtree(std::string(R"(<svg xmlns:xlink="http://www.w3.org/1999/xlink">)") +
                                std::string(kTwoTargets) + "<use " + std::string(useAttributes) +
                                "/></svg>");
  return document.querySelector("use")->cast<SVGUseElement>();
}

/// SVG 2: `href` takes precedence over `xlink:href`, whichever comes first in the source.
TEST(SVGUseElementTests, HrefWinsOverXlinkHrefInEitherOrder) {
  SVGDocument document;
  EXPECT_THAT(ParseUse(document, R"(href="#a" xlink:href="#b")"), HrefEq(RcString("#a")));
  EXPECT_THAT(ParseUse(document, R"(xlink:href="#b" href="#a")"), HrefEq(RcString("#a")));
}

TEST(SVGUseElementTests, RemovingHrefFallsBackToXlinkHref) {
  SVGDocument document;
  SVGUseElement use = ParseUse(document, R"(href="#a" xlink:href="#b")");
  use.removeAttribute(xml::XMLQualifiedNameRef("href"));
  EXPECT_THAT(use, HrefEq(RcString("#b")));
  use.removeAttribute(xml::XMLQualifiedNameRef("xlink", "href"));
  EXPECT_THAT(use, HrefEq(RcString()));
}

TEST(SVGUseElementTests, SettingXlinkHrefKeepsHrefTarget) {
  SVGDocument document;
  SVGUseElement use = ParseUse(document, R"(href="#a")");
  use.setAttribute(xml::XMLQualifiedNameRef("xlink", "href"), "#b");
  EXPECT_THAT(use, HrefEq(RcString("#a")));
  use.setAttribute(xml::XMLQualifiedNameRef("href"), "#b");
  EXPECT_THAT(use, HrefEq(RcString("#b")));
}

/// An empty href removes the reference, so rendering reports no unresolved target.
TEST(SVGUseElementTests, SetEmptyHrefRemovesReference) {
  SVGDocument document;
  SVGUseElement use = ParseUse(document, R"(href="#a")");
  ASSERT_THAT(use.entityHandle().all_of<components::ShadowTreeComponent>(), testing::IsTrue());

  use.setHref(RcString());
  EXPECT_THAT(use, HrefEq(RcString()));
  EXPECT_THAT(use.entityHandle().all_of<components::ShadowTreeComponent>(), testing::IsFalse());

  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warnings);
  EXPECT_THAT(warnings.warnings(), testing::IsEmpty());
}

/// Test that `<use>` is a graphics element, as in SVG 2, with an authored transform.
TEST(SVGUseElementTests, IsAGraphicsElementWithTransform) {
  auto fragment = instantiateSubtreeElementAs<SVGUseElement>("<use transform=\"translate(5 7)\"/>");
  const std::optional<SVGGraphicsElement> graphics = fragment->tryCast<SVGGraphicsElement>();
  ASSERT_THAT(graphics, Ne(std::nullopt));
  EXPECT_THAT(graphics->transform(), TransformEq(Transform2d::Translate(5.0, 7.0)));
}

/// Test setting x and y programmatically.
TEST(SVGUseElementTests, SetXY) {
  auto fragment = instantiateSubtreeElementAs<SVGUseElement>(R"(<use x="10" y="20" />)");

  EXPECT_THAT(fragment->x(), LengthIs(10.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->y(), LengthIs(20.0, Lengthd::Unit::None));

  fragment->setX(Lengthd(50.0, Lengthd::Unit::Px));
  fragment->setY(Lengthd(60.0, Lengthd::Unit::Em));

  EXPECT_THAT(fragment->x(), LengthIs(50.0, Lengthd::Unit::Px));
  EXPECT_THAT(fragment->y(), LengthIs(60.0, Lengthd::Unit::Em));
}

/// Test setting width and height programmatically.
TEST(SVGUseElementTests, SetWidthHeight) {
  auto fragment = instantiateSubtreeElementAs<SVGUseElement>("<use />");

  // Defaults are auto (nullopt).
  EXPECT_THAT(fragment->width(), Eq(std::nullopt));
  EXPECT_THAT(fragment->height(), Eq(std::nullopt));

  // Set explicit values.
  fragment->setWidth(Lengthd(100.0, Lengthd::Unit::Px));
  fragment->setHeight(Lengthd(200.0, Lengthd::Unit::Percent));

  ASSERT_THAT(fragment->width(), Ne(std::nullopt));
  ASSERT_THAT(fragment->height(), Ne(std::nullopt));
  EXPECT_THAT(*fragment->width(), LengthIs(100.0, Lengthd::Unit::Px));
  EXPECT_THAT(*fragment->height(), LengthIs(200.0, Lengthd::Unit::Percent));

  // Reset to auto.
  fragment->setWidth(std::nullopt);
  fragment->setHeight(std::nullopt);

  EXPECT_THAT(fragment->width(), Eq(std::nullopt));
  EXPECT_THAT(fragment->height(), Eq(std::nullopt));
}

/// Test parsing width and height with different units.
TEST(SVGUseElementTests, WidthHeightUnits) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGUseElement>(  //
                  R"(<use width="100px" height="50em" />)"),
              UseHas(AllOf(WidthEq(100.0, Lengthd::Unit::Px),  //
                           HeightEq(50.0, Lengthd::Unit::Em))));

  EXPECT_THAT(instantiateSubtreeElementAs<SVGUseElement>(  //
                  R"(<use width="75%" height="25pt" />)"),
              UseHas(AllOf(WidthEq(75.0, Lengthd::Unit::Percent),  //
                           HeightEq(25.0, Lengthd::Unit::Pt))));
}

/// Test parsing x and y with different units.
TEST(SVGUseElementTests, XYUnits) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGUseElement>(  //
                  R"(<use x="10px" y="20em" />)"),
              UseHas(AllOf(XEq(10.0, Lengthd::Unit::Px),  //
                           YEq(20.0, Lengthd::Unit::Em))));
}

/// Test that the use element can be cast to its type hierarchy.
TEST(SVGUseElementTests, Cast) {
  auto use = instantiateSubtreeElementAs<SVGUseElement>("<use />");

  EXPECT_THAT(use->tryCast<SVGElement>(), Ne(std::nullopt));
  EXPECT_THAT(use->tryCast<SVGUseElement>(), Ne(std::nullopt));
  // Ensure that an unrelated type does not match.
  EXPECT_THAT(use->tryCast<SVGCircleElement>(), Eq(std::nullopt));
  EXPECT_THAT(use->tryCast<SVGRectElement>(), Eq(std::nullopt));
}

/// Test that all attributes can be parsed together.
TEST(SVGUseElementTests, AllAttributes) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGUseElement>(  //
                  R"(<use href="#target" x="5" y="10" width="200" height="100" />)"),
              UseHas(AllOf(HrefEq(RcString("#target")),          //
                           XEq(5.0, Lengthd::Unit::None),        //
                           YEq(10.0, Lengthd::Unit::None),       //
                           WidthEq(200.0, Lengthd::Unit::None),  //
                           HeightEq(100.0, Lengthd::Unit::None))));
}

}  // namespace
}  // namespace donner::svg
