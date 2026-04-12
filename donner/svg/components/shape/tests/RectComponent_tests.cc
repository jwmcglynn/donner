#include "donner/svg/components/shape/RectComponent.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <utility>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/CSS.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGRectElement.h"

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

}  // namespace

TEST(RectComponent, ComputedRectComponentAppliesUnparsedProperties) {
  ParseWarningSink warningSink;
  const auto unparsedProperties = ParseUnparsedProperties("x: 15; ry: auto");

  ComputedRectComponent computed(RectProperties(), unparsedProperties, warningSink);

  EXPECT_FALSE(warningSink.hasWarnings());
  EXPECT_THAT(computed.properties.x.get(), Optional(Lengthd(15, Lengthd::Unit::None)));
  EXPECT_TRUE(computed.properties.ry.hasValue());
  EXPECT_THAT(computed.properties.ry.get(), Eq(std::nullopt));
}

TEST(RectComponent, ComputedRectComponentReportsInvalidUnparsedProperty) {
  ParseWarningSink warningSink;
  const auto unparsedProperties = ParseUnparsedProperties("width: bogus; unknown: 10");

  ComputedRectComponent computed(RectProperties(), unparsedProperties, warningSink);

  ASSERT_TRUE(warningSink.hasWarnings());
  ASSERT_EQ(warningSink.warnings().size(), 1u);
  EXPECT_THAT(warningSink.warnings().front(), ParseErrorIs("Invalid length or percentage"));
  EXPECT_THAT(computed.properties.width.get(), Optional(Lengthd(0, Lengthd::Unit::None)));
}

TEST(RectComponent, ParseRectPresentationAttributeParsesKnownAttribute) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);

  EXPECT_THAT(
      ParseRectPresentationAttribute(rect.entityHandle(), "height",
                                     parser::PropertyParseFnParams::CreateForAttribute("12%")),
      ParseResultIs(true));

  const RectProperties& properties = rect.entityHandle().get<RectComponent>().properties;
  EXPECT_THAT(properties.height.get(), Optional(Lengthd(12, Lengthd::Unit::Percent)));
}

TEST(RectComponent, ParseRectPresentationAttributeReturnsErrorForInvalidAttributeValue) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);

  EXPECT_THAT(ParseRectPresentationAttribute(
                  rect.entityHandle(), "rx",
                  parser::PropertyParseFnParams::CreateForAttribute("bogus")),
              ParseErrorIs("Invalid length or percentage"));

  const RectProperties& properties = rect.entityHandle().get<RectComponent>().properties;
  EXPECT_FALSE(properties.rx.hasValue());
}

TEST(RectComponent, ParseRectPresentationAttributeReturnsFalseForUnknownAttribute) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);

  EXPECT_THAT(ParseRectPresentationAttribute(
                  rect.entityHandle(), "cx",
                  parser::PropertyParseFnParams::CreateForAttribute("10")),
              ParseResultIs(false));
  EXPECT_FALSE(rect.entityHandle().all_of<RectComponent>());
}

}  // namespace donner::svg::components
