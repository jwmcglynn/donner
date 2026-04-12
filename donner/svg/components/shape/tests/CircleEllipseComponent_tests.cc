#include "donner/svg/components/shape/CircleComponent.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/CSS.h"
#include "donner/svg/SVGCircleElement.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGEllipseElement.h"
#include "donner/svg/components/shape/EllipseComponent.h"

namespace donner::svg::components {
namespace {

using testing::Eq;
using testing::Optional;

std::map<RcString, parser::UnparsedProperty> ParseUnparsedProperties(std::string_view style) {
  std::map<RcString, parser::UnparsedProperty> properties;

  for (const auto& declaration : css::CSS::ParseStyleAttribute(style)) {
    properties.emplace(
        declaration.name,
        parser::UnparsedProperty{declaration, css::Specificity::StyleAttribute()});
  }

  return properties;
}

TEST(CircleComponent, ComputedCircleComponentAppliesUnparsedProperties) {
  ParseWarningSink warningSink;
  const auto unparsedProperties = ParseUnparsedProperties("cx: 15; r: 12%");

  ComputedCircleComponent computed(CircleProperties(), unparsedProperties, warningSink);

  EXPECT_FALSE(warningSink.hasWarnings());
  EXPECT_THAT(computed.properties.cx.get(), Optional(Lengthd(15, Lengthd::Unit::None)));
  EXPECT_THAT(computed.properties.r.get(), Optional(Lengthd(12, Lengthd::Unit::Percent)));
}

TEST(CircleComponent, ComputedCircleComponentReportsInvalidUnparsedProperty) {
  ParseWarningSink warningSink;
  const auto unparsedProperties = ParseUnparsedProperties("cy: bogus; unknown: 10");

  ComputedCircleComponent computed(CircleProperties(), unparsedProperties, warningSink);

  ASSERT_TRUE(warningSink.hasWarnings());
  ASSERT_EQ(warningSink.warnings().size(), 1u);
  EXPECT_THAT(warningSink.warnings().front(), ParseErrorIs("Invalid length or percentage"));
  EXPECT_THAT(computed.properties.cy.get(), Optional(Lengthd(0, Lengthd::Unit::None)));
}

TEST(CircleComponent, ParseCirclePresentationAttributeParsesKnownAttribute) {
  SVGDocument document;
  SVGCircleElement circle = SVGCircleElement::Create(document);

  EXPECT_THAT(
      ParseCirclePresentationAttribute(circle.entityHandle(), "r",
                                       parser::PropertyParseFnParams::CreateForAttribute("8")),
      ParseResultIs(true));

  const CircleProperties& properties = circle.entityHandle().get<CircleComponent>().properties;
  EXPECT_THAT(properties.r.get(), Optional(Lengthd(8, Lengthd::Unit::None)));
}

TEST(CircleComponent, ParseCirclePresentationAttributeReturnsErrorForInvalidValue) {
  SVGDocument document;
  SVGCircleElement circle = SVGCircleElement::Create(document);

  EXPECT_THAT(ParseCirclePresentationAttribute(
                  circle.entityHandle(), "cy",
                  parser::PropertyParseFnParams::CreateForAttribute("bogus")),
              ParseErrorIs("Invalid length or percentage"));

  const CircleProperties& properties = circle.entityHandle().get<CircleComponent>().properties;
  EXPECT_THAT(properties.cy.get(), Optional(Lengthd(0, Lengthd::Unit::None)));
}

TEST(CircleComponent, ParseCirclePresentationAttributeReturnsFalseForUnknownAttribute) {
  SVGDocument document;
  SVGCircleElement circle = SVGCircleElement::Create(document);

  EXPECT_THAT(ParseCirclePresentationAttribute(
                  circle.entityHandle(), "width",
                  parser::PropertyParseFnParams::CreateForAttribute("10")),
              ParseResultIs(false));
  EXPECT_FALSE(circle.entityHandle().all_of<CircleComponent>());
}

TEST(EllipseComponent, ComputedEllipseComponentAppliesUnparsedProperties) {
  ParseWarningSink warningSink;
  const auto unparsedProperties = ParseUnparsedProperties("cx: 15; ry: auto");

  ComputedEllipseComponent computed(EllipseProperties(), unparsedProperties, warningSink);

  EXPECT_FALSE(warningSink.hasWarnings());
  EXPECT_THAT(computed.properties.cx.get(), Optional(Lengthd(15, Lengthd::Unit::None)));
  EXPECT_TRUE(computed.properties.ry.hasValue());
  EXPECT_THAT(computed.properties.ry.get(), Eq(std::nullopt));
}

TEST(EllipseComponent, ComputedEllipseComponentReportsInvalidUnparsedProperty) {
  ParseWarningSink warningSink;
  const auto unparsedProperties = ParseUnparsedProperties("rx: bogus; unknown: 10");

  ComputedEllipseComponent computed(EllipseProperties(), unparsedProperties, warningSink);

  ASSERT_TRUE(warningSink.hasWarnings());
  ASSERT_EQ(warningSink.warnings().size(), 1u);
  EXPECT_THAT(warningSink.warnings().front(), ParseErrorIs("Invalid length or percentage"));
  EXPECT_FALSE(computed.properties.rx.hasValue());
}

TEST(EllipseComponent, ParseEllipsePresentationAttributeParsesKnownAttribute) {
  SVGDocument document;
  SVGEllipseElement ellipse = SVGEllipseElement::Create(document);

  EXPECT_THAT(ParseEllipsePresentationAttribute(
                  ellipse.entityHandle(), "rx",
                  parser::PropertyParseFnParams::CreateForAttribute("12%")),
              ParseResultIs(true));

  const EllipseProperties& properties = ellipse.entityHandle().get<EllipseComponent>().properties;
  EXPECT_THAT(properties.rx.get(), Optional(Lengthd(12, Lengthd::Unit::Percent)));
}

TEST(EllipseComponent, ParseEllipsePresentationAttributeReturnsErrorForInvalidValue) {
  SVGDocument document;
  SVGEllipseElement ellipse = SVGEllipseElement::Create(document);

  EXPECT_THAT(ParseEllipsePresentationAttribute(
                  ellipse.entityHandle(), "ry",
                  parser::PropertyParseFnParams::CreateForAttribute("bogus")),
              ParseErrorIs("Invalid length or percentage"));

  const EllipseProperties& properties = ellipse.entityHandle().get<EllipseComponent>().properties;
  EXPECT_FALSE(properties.ry.hasValue());
}

TEST(EllipseComponent, ParseEllipsePresentationAttributeReturnsFalseForUnknownAttribute) {
  SVGDocument document;
  SVGEllipseElement ellipse = SVGEllipseElement::Create(document);

  EXPECT_THAT(ParseEllipsePresentationAttribute(
                  ellipse.entityHandle(), "width",
                  parser::PropertyParseFnParams::CreateForAttribute("10")),
              ParseResultIs(false));
  EXPECT_FALSE(ellipse.entityHandle().all_of<EllipseComponent>());
}

}  // namespace
}  // namespace donner::svg::components
