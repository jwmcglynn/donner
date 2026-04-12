#include "donner/svg/components/paint/PatternComponent.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/CSS.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGStopElement.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/paint/StopComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {
namespace {

using css::Color;
using css::RGBA;
using testing::Optional;

TEST(StopComponentTest, ParseStopPresentationAttributes) {
  SVGDocument document;
  SVGStopElement stop = SVGStopElement::Create(document);

  EXPECT_THAT(ParseStopPresentationAttribute(
                  stop.entityHandle(), "stop-color",
                  parser::PropertyParseFnParams::CreateForAttribute("red")),
              ParseResultIs(true));
  EXPECT_THAT(ParseStopPresentationAttribute(
                  stop.entityHandle(), "stop-opacity",
                  parser::PropertyParseFnParams::CreateForAttribute("50%")),
              ParseResultIs(true));
  EXPECT_THAT(ParseStopPresentationAttribute(
                  stop.entityHandle(), "unknown",
                  parser::PropertyParseFnParams::CreateForAttribute("red")),
              ParseResultIs(false));

  const auto& properties = stop.entityHandle().get<StopComponent>().properties;
  EXPECT_THAT(properties.stopColor.get(), Optional(Color(RGBA(0xFF, 0, 0, 0xFF))));
  EXPECT_THAT(properties.stopOpacity.get(), Optional(0.5));
}

TEST(StopComponentTest, ParseStopPresentationAttributeErrors) {
  SVGDocument document;
  SVGStopElement stop = SVGStopElement::Create(document);

  EXPECT_THAT(ParseStopPresentationAttribute(
                  stop.entityHandle(), "stop-color",
                  parser::PropertyParseFnParams::CreateForAttribute("bogus")),
              ParseErrorIs("Invalid color 'bogus'"));
  EXPECT_THAT(ParseStopPresentationAttribute(
                  stop.entityHandle(), "stop-opacity",
                  parser::PropertyParseFnParams::CreateForAttribute("bogus")),
              ParseErrorIs("Invalid alpha value"));
}

TEST(StopComponentTest, ComputedStopComponentResolvesCurrentColorAndWarnings) {
  StopProperties input;
  ComputedStyleComponent style;
  style.properties.emplace();
  style.properties->color.set(Color(RGBA(0, 0xFF, 0, 0xFF)), css::Specificity::Override());

  css::Declaration stopColorDecl = css::CSS::ParseStyleAttribute("stop-color: currentColor").front();
  css::Declaration stopOpacityDecl = css::CSS::ParseStyleAttribute("stop-opacity: 0.25").front();
  css::Declaration invalidDecl = css::CSS::ParseStyleAttribute("stop-color: bogus").front();
  style.properties->unparsedProperties.emplace(
      "stop-color", parser::UnparsedProperty{invalidDecl, css::Specificity::StyleAttribute()});

  std::map<RcString, parser::UnparsedProperty> unparsed;
  unparsed.emplace("stop-color",
                   parser::UnparsedProperty{stopColorDecl, css::Specificity::StyleAttribute()});
  unparsed.emplace("stop-opacity",
                   parser::UnparsedProperty{stopOpacityDecl, css::Specificity::StyleAttribute()});
  unparsed.emplace("invalid",
                   parser::UnparsedProperty{invalidDecl, css::Specificity::StyleAttribute()});

  ParseWarningSink warningSink;
  ComputedStopComponent computed(input, style, unparsed, warningSink);

  EXPECT_THAT(computed.properties.stopColor.get(), Optional(Color(RGBA(0, 0xFF, 0, 0xFF))));
  EXPECT_THAT(computed.properties.stopOpacity.get(), Optional(0.25));
  EXPECT_FALSE(warningSink.hasWarnings());
}

TEST(StopComponentTest, ComputedStopComponentReportsInvalidRecognizedProperty) {
  StopProperties input;
  ComputedStyleComponent style;
  style.properties.emplace();

  css::Declaration invalidDecl =
      css::CSS::ParseStyleAttribute("stop-opacity: bogus").front();
  std::map<RcString, parser::UnparsedProperty> unparsed;
  unparsed.emplace("stop-opacity",
                   parser::UnparsedProperty{invalidDecl, css::Specificity::StyleAttribute()});

  ParseWarningSink warningSink;
  ComputedStopComponent computed(input, style, unparsed, warningSink);

  EXPECT_TRUE(warningSink.hasWarnings());
  EXPECT_THAT(warningSink.warnings().front(), ParseErrorIs("Invalid alpha value"));
  EXPECT_THAT(computed.properties.stopOpacity.get(), Optional(1.0));
}

TEST(PatternComponentTest, InheritAttributesFromBaseAndOverride) {
  SVGDocument document;
  Registry& registry = document.registry();

  const Entity baseEntity = registry.create();
  const Entity derivedEntity = registry.create();
  EntityHandle baseHandle(registry, baseEntity);
  EntityHandle derivedHandle(registry, derivedEntity);

  auto& baseComputed = registry.emplace<ComputedPatternComponent>(baseEntity);
  baseComputed.patternUnits = PatternUnits::UserSpaceOnUse;
  baseComputed.patternContentUnits = PatternContentUnits::ObjectBoundingBox;
  baseComputed.tileRect = Box2d(Vector2d(1, 2), Vector2d(3, 4));
  baseComputed.preserveAspectRatio = PreserveAspectRatio::None();
  baseComputed.viewBox = Box2d(Vector2d(5, 6), Vector2d(7, 8));
  baseComputed.sizeProperties.x.set(Lengthd(10, Lengthd::Unit::Px), css::Specificity::Override());
  baseComputed.sizeProperties.y.set(Lengthd(11, Lengthd::Unit::Px), css::Specificity::Override());
  baseComputed.sizeProperties.width.set(Lengthd(12, Lengthd::Unit::Px),
                                        css::Specificity::Override());
  baseComputed.sizeProperties.height.set(Lengthd(13, Lengthd::Unit::Px),
                                         css::Specificity::Override());

  auto& derivedPattern = registry.emplace<PatternComponent>(derivedEntity);
  derivedPattern.patternContentUnits = PatternContentUnits::UserSpaceOnUse;
  derivedPattern.sizeProperties.x.set(Lengthd(18, Lengthd::Unit::Px), css::Specificity::Override());
  derivedPattern.sizeProperties.y.set(Lengthd(19, Lengthd::Unit::Px), css::Specificity::Override());
  derivedPattern.sizeProperties.width.set(Lengthd(20, Lengthd::Unit::Px),
                                          css::Specificity::Override());
  derivedPattern.sizeProperties.height.set(Lengthd(21, Lengthd::Unit::Px),
                                           css::Specificity::Override());

  ComputedPatternComponent computed;
  computed.inheritAttributesFrom(derivedHandle, baseHandle);

  EXPECT_EQ(computed.patternUnits, PatternUnits::UserSpaceOnUse);
  EXPECT_EQ(computed.patternContentUnits, PatternContentUnits::UserSpaceOnUse);
  EXPECT_EQ(computed.tileRect, Box2d(Vector2d(1, 2), Vector2d(3, 4)));
  ASSERT_TRUE(computed.viewBox.has_value());
  EXPECT_EQ(*computed.viewBox, Box2d(Vector2d(5, 6), Vector2d(7, 8)));
  EXPECT_THAT(computed.sizeProperties.x.get(), Optional(Lengthd(18, Lengthd::Unit::Px)));
  EXPECT_THAT(computed.sizeProperties.y.get(), Optional(Lengthd(19, Lengthd::Unit::Px)));
  EXPECT_THAT(computed.sizeProperties.width.get(), Optional(Lengthd(20, Lengthd::Unit::Px)));
  EXPECT_THAT(computed.sizeProperties.height.get(), Optional(Lengthd(21, Lengthd::Unit::Px)));
}

TEST(RadialGradientComponentTest, InheritAttributesFromBaseAndOverride) {
  SVGDocument document;
  Registry& registry = document.registry();

  const Entity baseEntity = registry.create();
  const Entity derivedEntity = registry.create();
  EntityHandle baseHandle(registry, baseEntity);
  EntityHandle derivedHandle(registry, derivedEntity);

  auto& baseComputed = registry.emplace<ComputedRadialGradientComponent>(baseEntity);
  baseComputed.cx = Lengthd(1, Lengthd::Unit::Px);
  baseComputed.cy = Lengthd(2, Lengthd::Unit::Px);
  baseComputed.r = Lengthd(3, Lengthd::Unit::Px);
  baseComputed.fx = Lengthd(4, Lengthd::Unit::Px);
  baseComputed.fy = Lengthd(5, Lengthd::Unit::Px);
  baseComputed.fr = Lengthd(6, Lengthd::Unit::Px);

  auto& derived = registry.emplace<RadialGradientComponent>(derivedEntity);
  derived.cy = Lengthd(20, Lengthd::Unit::Px);
  derived.r = Lengthd(30, Lengthd::Unit::Px);
  derived.fx = Lengthd(40, Lengthd::Unit::Px);
  derived.fr = Lengthd(60, Lengthd::Unit::Px);

  ComputedRadialGradientComponent computed;
  computed.inheritAttributes(derivedHandle, baseHandle);

  EXPECT_EQ(computed.cx, Lengthd(1, Lengthd::Unit::Px));
  EXPECT_EQ(computed.cy, Lengthd(20, Lengthd::Unit::Px));
  EXPECT_EQ(computed.r, Lengthd(30, Lengthd::Unit::Px));
  EXPECT_THAT(computed.fx, Optional(Lengthd(40, Lengthd::Unit::Px)));
  EXPECT_THAT(computed.fy, Optional(Lengthd(5, Lengthd::Unit::Px)));
  EXPECT_EQ(computed.fr, Lengthd(60, Lengthd::Unit::Px));
}

TEST(RadialGradientComponentTest, InheritAttributesCreatesComputedComponentOnHandle) {
  SVGDocument document;
  Registry& registry = document.registry();

  const Entity entity = registry.create();
  EntityHandle handle(registry, entity);
  auto& radial = registry.emplace<RadialGradientComponent>(entity);
  radial.cx = Lengthd(7, Lengthd::Unit::Px);

  radial.inheritAttributes(handle, EntityHandle());

  ASSERT_TRUE(registry.all_of<ComputedRadialGradientComponent>(entity));
  EXPECT_EQ(registry.get<ComputedRadialGradientComponent>(entity).cx, Lengthd(7, Lengthd::Unit::Px));
}

}  // namespace
}  // namespace donner::svg::components
