#include "donner/svg/properties/PropertyRegistry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/CSS.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/components/layout/TransformComponent.h"

namespace donner::svg {

using css::Color;
using css::ComponentValue;
using css::Declaration;
using css::RGBA;
using css::Specificity;
using css::Token;

using testing::Contains;
using testing::Eq;
using testing::Ne;
using testing::Optional;

namespace {

MATCHER_P2(LengthValueIs, value, unit, "") {
  return testing::ExplainMatchResult(LengthIs(testing::DoubleEq(value), Eq(unit)), arg,
                                     result_listener);
}

MATCHER_P(StrokeDasharrayIs, expected, "") {
  if (arg.size() != expected.size()) {
    *result_listener << "size is " << arg.size();
    return false;
  }

  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (arg[i] != expected[i]) {
      *result_listener << "dasharray[" << i << "] is " << testing::PrintToString(arg[i]);
      return false;
    }
  }

  return true;
}

MATCHER_P(FilterReferenceIs, href, "") {
  if (!arg.template is<FilterEffect::ElementReference>()) {
    *result_listener << "filter effect is " << testing::PrintToString(arg);
    return false;
  }

  return testing::ExplainMatchResult(Eq(Reference(href)),
                                     arg.template get<FilterEffect::ElementReference>().reference,
                                     result_listener);
}

MATCHER_P2(FilterBlurIs, value, unit, "") {
  if (!arg.template is<FilterEffect::Blur>()) {
    *result_listener << "filter effect is " << testing::PrintToString(arg);
    return false;
  }

  const auto& blur = arg.template get<FilterEffect::Blur>();
  return testing::ExplainMatchResult(LengthValueIs(value, unit), blur.stdDeviationX,
                                     result_listener) &&
         testing::ExplainMatchResult(LengthValueIs(value, unit), blur.stdDeviationY,
                                     result_listener);
}

#define DONNER_FILTER_AMOUNT_MATCHER(name, effect_type)                                          \
  MATCHER_P(name, expected, "") {                                                                \
    if (!arg.template is<effect_type>()) {                                                       \
      *result_listener << "filter effect is " << testing::PrintToString(arg);                    \
      return false;                                                                              \
    }                                                                                            \
                                                                                                 \
    return testing::ExplainMatchResult(testing::DoubleEq(expected),                              \
                                       arg.template get<effect_type>().amount, result_listener); \
  }

DONNER_FILTER_AMOUNT_MATCHER(FilterBrightnessIs, FilterEffect::Brightness)
DONNER_FILTER_AMOUNT_MATCHER(FilterContrastIs, FilterEffect::Contrast)
DONNER_FILTER_AMOUNT_MATCHER(FilterGrayscaleIs, FilterEffect::Grayscale)
DONNER_FILTER_AMOUNT_MATCHER(FilterInvertIs, FilterEffect::Invert)
DONNER_FILTER_AMOUNT_MATCHER(FilterOpacityIs, FilterEffect::FilterOpacity)
DONNER_FILTER_AMOUNT_MATCHER(FilterSaturateIs, FilterEffect::Saturate)
DONNER_FILTER_AMOUNT_MATCHER(FilterSepiaIs, FilterEffect::Sepia)

#undef DONNER_FILTER_AMOUNT_MATCHER

MATCHER_P2(FilterHueRotateIs, expected, tolerance, "") {
  if (!arg.template is<FilterEffect::HueRotate>()) {
    *result_listener << "filter effect is " << testing::PrintToString(arg);
    return false;
  }

  return testing::ExplainMatchResult(testing::DoubleNear(expected, tolerance),
                                     arg.template get<FilterEffect::HueRotate>().angleDegrees,
                                     result_listener);
}

MATCHER_P4(FilterDropShadowIs, offsetX, offsetY, stdDeviation, color, "") {
  if (!arg.template is<FilterEffect::DropShadow>()) {
    *result_listener << "filter effect is " << testing::PrintToString(arg);
    return false;
  }

  const auto& shadow = arg.template get<FilterEffect::DropShadow>();
  return testing::ExplainMatchResult(Eq(offsetX), shadow.offsetX, result_listener) &&
         testing::ExplainMatchResult(Eq(offsetY), shadow.offsetY, result_listener) &&
         testing::ExplainMatchResult(Eq(stdDeviation), shadow.stdDeviation, result_listener) &&
         testing::ExplainMatchResult(Eq(color), shadow.color, result_listener);
}

}  // namespace

TEST(PropertyRegistry, Set) {
  PropertyRegistry registry;
  EXPECT_GE(registry.numProperties(), 0);
  EXPECT_EQ(registry.numPropertiesSet(), 0);

  registry.color.set(Color(RGBA(0xFF, 0, 0, 0xFF)), Specificity::FromABC(0, 0, 1));
  EXPECT_EQ(registry.numPropertiesSet(), 1);

  // Test printing to string.
  EXPECT_THAT(registry, ToStringIs(R"(PropertyRegistry {
  color: rgba(255, 0, 0, 255) (set) @ Specificity(0, 0, 1)
}
)"));
}

TEST(PropertyRegistry, PropertyNamesExposeCssRegistryKeys) {
  EXPECT_THAT(PropertyRegistry::propertyNames(), Contains(std::string_view("fill")));
  EXPECT_THAT(PropertyRegistry::propertyNames(), Contains(std::string_view("mix-blend-mode")));
  EXPECT_THAT(PropertyRegistry::propertyNames(), Contains(std::string_view("marker")));

  EXPECT_TRUE(PropertyRegistry::isPresentationAttributeName("fill"));
  EXPECT_TRUE(PropertyRegistry::isPresentationAttributeName("transform-origin"));
  EXPECT_FALSE(PropertyRegistry::isPresentationAttributeName("mix-blend-mode"));
  EXPECT_FALSE(PropertyRegistry::isPresentationAttributeName("marker"));
}

TEST(Property, GetStoredValueOnlyReturnsExplicitConcreteValue) {
  Property<int> property("integer", []() -> std::optional<int> { return 5; });
  EXPECT_EQ(property.get(), 5);
  EXPECT_EQ(property.getStoredValue(), nullptr);

  property.set(7, Specificity::FromABC(0, 1, 0));
  ASSERT_NE(property.getStoredValue(), nullptr);
  EXPECT_EQ(*property.getStoredValue(), 7);

  property.set(std::optional<int>(), Specificity::FromABC(0, 1, 0));
  EXPECT_EQ(property.get(), std::nullopt);
  EXPECT_EQ(property.getStoredValue(), nullptr);

  property.set(PropertyState::Inherit, Specificity::FromABC(0, 1, 0));
  EXPECT_EQ(property.getStoredValue(), nullptr);
}

TEST(Property, InheritFromCoversStateAndSpecificityBranches) {
  using InheritingProperty = Property<int, PropertyCascade::Inherit>;

  InheritingProperty parent("integer");
  parent.set(10, Specificity::FromABC(0, 2, 0));

  {
    InheritingProperty child("integer");
    const InheritingProperty inherited = child.inheritFrom(parent);
    EXPECT_EQ(inherited.get(), 10);
    EXPECT_EQ(inherited.state, PropertyState::Set);
  }

  {
    InheritingProperty child("integer");
    child.set(PropertyState::Inherit, Specificity::FromABC(0, 0, 1));
    const InheritingProperty inherited = child.inheritFrom(parent);
    EXPECT_EQ(inherited.get(), 10);
    EXPECT_EQ(inherited.specificity, Specificity::FromABC(0, 0, 1));
  }

  {
    InheritingProperty child("integer");
    child.set(PropertyState::ExplicitUnset, Specificity::FromABC(0, 0, 1));
    const InheritingProperty inherited = child.inheritFrom(parent);
    EXPECT_EQ(inherited.get(), 10);
  }

  {
    InheritingProperty child("integer");
    child.set(1, Specificity::FromABC(0, 1, 0));
    const InheritingProperty inherited = child.inheritFrom(parent);
    EXPECT_EQ(inherited.get(), 10);
    EXPECT_EQ(inherited.specificity, Specificity::FromABC(0, 2, 0));
  }

  {
    InheritingProperty child("integer");
    child.set(1, Specificity::FromABC(0, 3, 0));
    const InheritingProperty inherited = child.inheritFrom(parent);
    EXPECT_EQ(inherited.get(), 1);
    EXPECT_EQ(inherited.specificity, Specificity::FromABC(0, 3, 0));
  }

  {
    InheritingProperty unspecifiedParent("integer");
    InheritingProperty child("integer");
    const InheritingProperty inherited = child.inheritFrom(unspecifiedParent);
    EXPECT_EQ(inherited.get(), std::nullopt);
  }

  {
    InheritingProperty child("integer");
    const InheritingProperty inherited = child.inheritFrom(parent, PropertyInheritOptions::NoPaint);
    EXPECT_EQ(inherited.get(), 10);
  }
}

TEST(Property, NonInheritingPropertyOnlyInheritsExplicitInheritState) {
  Property<int> parent("integer");
  parent.set(10, Specificity::FromABC(0, 2, 0));

  Property<int> child("integer");
  EXPECT_EQ(child.inheritFrom(parent).get(), std::nullopt);

  child.set(PropertyState::Inherit, Specificity::FromABC(0, 1, 0));
  const Property<int> inherited = child.inheritFrom(parent);
  EXPECT_EQ(inherited.get(), 10);
  EXPECT_EQ(inherited.specificity, Specificity::FromABC(0, 1, 0));
}

TEST(Property, OstreamOutputCoversAllStates) {
  Property<int> property("integer");
  EXPECT_THAT(property, ToStringIs("integer: (not set)"));

  property.set(7, Specificity::FromABC(0, 1, 0));
  EXPECT_THAT(property, ToStringIs("integer: 7 (set) @ Specificity(0, 1, 0)"));

  property.set(std::optional<int>(), Specificity::FromABC(0, 1, 0));
  EXPECT_THAT(property, ToStringIs("integer: nullopt (set) @ Specificity(0, 1, 0)"));

  property.set(PropertyState::Inherit, Specificity::FromABC(0, 1, 0));
  EXPECT_THAT(property, ToStringIs("integer: (inherit) @ Specificity(0, 1, 0)"));

  property.set(PropertyState::ExplicitInitial, Specificity::FromABC(0, 1, 0));
  EXPECT_THAT(property, ToStringIs("integer: (explicit initial) @ Specificity(0, 1, 0)"));

  property.set(PropertyState::ExplicitUnset, Specificity::FromABC(0, 1, 0));
  EXPECT_THAT(property, ToStringIs("integer: (explicit unset) @ Specificity(0, 1, 0)"));
}

TEST(PropertyRegistry, ParseDeclaration) {
  css::Declaration declaration("color", {ComponentValue(Token(Token::Ident("lime"), 0))});

  PropertyRegistry registry;
  EXPECT_THAT(registry.parseProperty(declaration, Specificity()), Eq(std::nullopt));
  EXPECT_THAT(registry.color.get(), Optional(Color(RGBA(0, 0xFF, 0, 0xFF))));

  // Test printing to string.
  EXPECT_THAT(registry, ToStringIs(R"(PropertyRegistry {
  color: rgba(0, 255, 0, 255) (set) @ Specificity(0, 0, 0)
}
)"));
}

TEST(PropertyRegistry, ParseDeclarationError) {
  std::optional<ParseDiagnostic> parseProperty(const css::Declaration& declaration);

  css::Declaration declaration("color", {ComponentValue(Token(Token::Ident("invalid-color"), 0))});

  PropertyRegistry registry;
  EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
              ParseErrorIs("Invalid color 'invalid-color'"));
}

TEST(PropertyRegistry, ParseDeclarationHash) {
  css::Declaration declaration(
      "color", {ComponentValue(Token(Token::Hash(Token::Hash::Type::Id, "FFF"), 0))});

  PropertyRegistry registry;
  EXPECT_THAT(registry.parseProperty(declaration, Specificity()), Eq(std::nullopt));
  EXPECT_THAT(registry.color.get(), Optional(Color(RGBA(0xFF, 0xFF, 0xFF, 0xFF))));
}

TEST(PropertyRegistry, UnsupportedProperty) {
  css::Declaration declaration("not-supported", {ComponentValue(Token(Token::Ident("test"), 0))});

  PropertyRegistry registry;
  EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
              Optional(ParseErrorIs("Unknown property 'not-supported'")));
}

TEST(PropertyRegistry, ParseErrorsAreIgnored) {
  {
    PropertyRegistry registry;
    registry.parseStyle("color: red");
    EXPECT_THAT(registry.color.get(), Optional(Color(RGBA(0xFF, 0, 0, 0xFF))));

    // If an invalid value is set, the previous value is retained and the new property is ignored.
    registry.parseStyle("color: invalid");
    EXPECT_THAT(registry.color.get(), Optional(Color(RGBA(0xFF, 0, 0, 0xFF))))
        << "Invalid property should be ignored";
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("color: invalid");
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))))
        << "Invalid property should be ignored";
  }
}

TEST(PropertyRegistry, BuiltinKeywords) {
  {
    PropertyRegistry registry;
    registry.parseStyle("color: initial");
    EXPECT_EQ(registry.color.state, PropertyState::ExplicitInitial);
    EXPECT_TRUE(registry.color.isSpecified());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));

    registry.parseStyle("color: inherit");
    EXPECT_EQ(registry.color.state, PropertyState::Inherit);
    EXPECT_TRUE(registry.color.isSpecified());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));

    registry.parseStyle("color: unset");
    EXPECT_EQ(registry.color.state, PropertyState::ExplicitUnset);
    EXPECT_TRUE(registry.color.isSpecified());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("color: inherit invalid");
    EXPECT_FALSE(registry.color.isSpecified());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));
  }
}

TEST(PropertyRegistry, ParseColor) {
  PropertyRegistry registry;
  registry.parseStyle("color: red");
  EXPECT_TRUE(registry.color.isSpecified());
  EXPECT_THAT(registry.color.get(), Optional(Color(RGBA(0xFF, 0, 0, 0xFF))));
}

TEST(PropertyRegistry, RejectsCommaSeparatedTextDecoration) {
  PropertyRegistry registry;

  EXPECT_THAT(
      registry.parsePresentationAttribute("text-decoration", "underline,overline,line-through"),
      ParseErrorIs("Invalid text-decoration value"));
  EXPECT_EQ(registry.textDecoration.get(), TextDecoration::None);
}

TEST(PropertyRegistry, ParsePresentationAttribute) {
  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("color", "red"), ParseResultIs(true));
    EXPECT_THAT(registry.color.get(), Optional(Color(RGBA(0xFF, 0, 0, 0xFF))));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("not_supported", "red"), ParseResultIs(false));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("color", ""), ParseErrorIs("No color found"));
    EXPECT_FALSE(registry.color.isSpecified());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("color", "invalid"),
                ParseErrorIs("Invalid color 'invalid'"));
    EXPECT_FALSE(registry.color.isSpecified());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("color", "red !important"),
                ParseErrorIs("Expected a single color"));
    EXPECT_FALSE(registry.color.isSpecified()) << "!important is not supported";
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("color", " /*comment*/ red "),
                ParseResultIs(true));
    EXPECT_TRUE(registry.color.isSpecified()) << "Comments and whitespace should be ignored";
    EXPECT_THAT(registry.color.get(), Optional(Color(RGBA(0xFF, 0, 0, 0xFF))));
  }
}

TEST(PropertyRegistry, Fill) {
  // Initial value of fill is black.
  const PaintServer kInitialFill(PaintServer::Solid(Color(RGBA(0, 0, 0, 0xFF))));

  {
    PropertyRegistry registry;
    EXPECT_FALSE(registry.fill.isSpecified());
    EXPECT_THAT(registry.fill.get(), Optional(kInitialFill));

    registry.parseStyle("fill: none");
    EXPECT_TRUE(registry.fill.isSpecified());
    EXPECT_THAT(registry.fill.get(), Optional(PaintServer(PaintServer::None())));

    registry.parseStyle("fill: red  ");
    EXPECT_THAT(registry.fill.get(),
                Optional(PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0, 0, 0xFF))))));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("fill", ""),
                ParseErrorIs("Invalid paint server value"));
    EXPECT_FALSE(registry.fill.isSpecified());
    EXPECT_THAT(registry.fill.get(), Optional(kInitialFill));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("fill: red asdf");
    EXPECT_FALSE(registry.fill.isSpecified());
    EXPECT_THAT(registry.fill.get(), Optional(kInitialFill));

    registry.parseStyle("fill: asdf");
    EXPECT_FALSE(registry.fill.isSpecified());
    EXPECT_THAT(registry.fill.get(), Optional(kInitialFill));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("fill: context-fill");
    EXPECT_THAT(registry.fill.get(), Optional(PaintServer(PaintServer::ContextFill())));
    registry.parseStyle("fill: \t context-stroke");
    EXPECT_THAT(registry.fill.get(), Optional(PaintServer(PaintServer::ContextStroke())));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("fill: context-stroke invalid");
    EXPECT_FALSE(registry.fill.isSpecified());
    EXPECT_THAT(registry.fill.get(), Optional(kInitialFill));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("fill: url(#test)");
    EXPECT_THAT(registry.fill.get(), Optional(PaintServer(PaintServer::ElementReference("#test"))));

    registry.parseStyle("fill: url(#test) none");
    EXPECT_THAT(registry.fill.get(), Optional(PaintServer(PaintServer::ElementReference("#test"))));

    registry.parseStyle("fill: url(#test) green");
    EXPECT_THAT(registry.fill.get(), Optional(PaintServer(PaintServer::ElementReference(
                                         "#test", Color(RGBA(0, 128, 0, 0xFF))))));

    registry.parseStyle("fill: url(#test)   lime\t  ");
    EXPECT_THAT(registry.fill.get(), Optional(PaintServer(PaintServer::ElementReference(
                                         "#test", Color(RGBA(0, 0xFF, 0, 0xFF))))));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("fill: url(#test) invalid");
    EXPECT_FALSE(registry.fill.isSpecified());
    EXPECT_THAT(registry.fill.get(), Optional(kInitialFill));
  }
}

TEST(PropertyRegistry, Stroke) {
  // Initial value of stroke is none.
  const PaintServer kInitialFill(PaintServer::None{});

  {
    PropertyRegistry registry;
    EXPECT_FALSE(registry.stroke.isSpecified());
    EXPECT_THAT(registry.stroke.get(), Optional(kInitialFill));

    registry.parseStyle("stroke: none");
    EXPECT_TRUE(registry.stroke.isSpecified());
    EXPECT_THAT(registry.stroke.get(), Optional(PaintServer(PaintServer::None())));

    registry.parseStyle("stroke: red  ");
    EXPECT_THAT(registry.stroke.get(),
                Optional(PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0, 0, 0xFF))))));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("stroke: red asdf");
    EXPECT_FALSE(registry.stroke.isSpecified());
    EXPECT_THAT(registry.stroke.get(), Optional(kInitialFill));

    registry.parseStyle("stroke: asdf");
    EXPECT_FALSE(registry.stroke.isSpecified());
    EXPECT_THAT(registry.stroke.get(), Optional(kInitialFill));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("stroke: context-stroke");
    EXPECT_THAT(registry.stroke.get(), Optional(PaintServer(PaintServer::ContextStroke())));
    registry.parseStyle("stroke: \t context-stroke");
    EXPECT_THAT(registry.stroke.get(), Optional(PaintServer(PaintServer::ContextStroke())));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("stroke: context-stroke invalid");
    EXPECT_FALSE(registry.stroke.isSpecified());
    EXPECT_THAT(registry.stroke.get(), Optional(kInitialFill));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("stroke: url(#test)");
    EXPECT_THAT(registry.stroke.get(),
                Optional(PaintServer(PaintServer::ElementReference("#test"))));

    registry.parseStyle("stroke: url(#test) none");
    EXPECT_THAT(registry.stroke.get(),
                Optional(PaintServer(PaintServer::ElementReference("#test"))));

    registry.parseStyle("stroke: url(#test) green");
    EXPECT_THAT(registry.stroke.get(), Optional(PaintServer(PaintServer::ElementReference(
                                           "#test", Color(RGBA(0, 128, 0, 0xFF))))));

    registry.parseStyle("stroke: url(#test)   lime\t  ");
    EXPECT_THAT(registry.stroke.get(), Optional(PaintServer(PaintServer::ElementReference(
                                           "#test", Color(RGBA(0, 0xFF, 0, 0xFF))))));
  }
}

TEST(PropertyRegistry, FontStyle) {
  {
    PropertyRegistry registry;
    EXPECT_FALSE(registry.fontStyle.isSpecified());
    EXPECT_THAT(registry.fontStyle.get(), Optional(FontStyle::Normal));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-style: normal");
    EXPECT_THAT(registry.fontStyle.get(), Optional(FontStyle::Normal));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-style: italic");
    EXPECT_THAT(registry.fontStyle.get(), Optional(FontStyle::Italic));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-style: oblique");
    EXPECT_THAT(registry.fontStyle.get(), Optional(FontStyle::Oblique));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-style: invalid");
    EXPECT_FALSE(registry.fontStyle.isSpecified());
  }

  // Presentation attribute.
  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("font-style", "italic"), ParseResultIs(true));
    EXPECT_THAT(registry.fontStyle.get(), Optional(FontStyle::Italic));
  }
}

TEST(PropertyRegistry, FontStretch) {
  {
    PropertyRegistry registry;
    EXPECT_FALSE(registry.fontStretch.isSpecified());
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::Normal)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: normal");
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::Normal)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: ultra-condensed");
    EXPECT_THAT(registry.fontStretch.get(),
                Optional(static_cast<int>(FontStretch::UltraCondensed)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: extra-condensed");
    EXPECT_THAT(registry.fontStretch.get(),
                Optional(static_cast<int>(FontStretch::ExtraCondensed)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: condensed");
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::Condensed)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: semi-condensed");
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::SemiCondensed)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: semi-expanded");
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::SemiExpanded)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: expanded");
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::Expanded)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: extra-expanded");
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::ExtraExpanded)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: ultra-expanded");
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::UltraExpanded)));
  }

  // SVG 1.1 relative keywords stored as sentinel values.
  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: narrower");
    EXPECT_THAT(registry.fontStretch.get(), Optional(PropertyRegistry::kFontStretchNarrower));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: wider");
    EXPECT_THAT(registry.fontStretch.get(), Optional(PropertyRegistry::kFontStretchWider));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: invalid");
    EXPECT_FALSE(registry.fontStretch.isSpecified());
  }

  // Presentation attribute.
  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("font-stretch", "condensed"),
                ParseResultIs(true));
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::Condensed)));
  }
}

TEST(PropertyRegistry, FontVariant) {
  {
    PropertyRegistry registry;
    EXPECT_FALSE(registry.fontVariant.isSpecified());
    EXPECT_THAT(registry.fontVariant.get(), Optional(FontVariant::Normal));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-variant: normal");
    EXPECT_THAT(registry.fontVariant.get(), Optional(FontVariant::Normal));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-variant: small-caps");
    EXPECT_THAT(registry.fontVariant.get(), Optional(FontVariant::SmallCaps));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-variant: invalid");
    EXPECT_FALSE(registry.fontVariant.isSpecified());
  }

  // Presentation attribute.
  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("font-variant", "small-caps"),
                ParseResultIs(true));
    EXPECT_THAT(registry.fontVariant.get(), Optional(FontVariant::SmallCaps));
  }
}

TEST(PropertyRegistry, FontSizeAbsoluteKeywords) {
  auto parseFontSize = [](const char* value) {
    PropertyRegistry registry;
    registry.parseStyle(std::string("font-size: ") + value);
    return registry.fontSize.get();
  };

  // Absolute keywords use CSS Fonts Level 4 §2.5.1 scaling factors from medium=12px (UA default).
  // https://www.w3.org/TR/css-fonts-4/#absolute-size-mapping
  EXPECT_TRUE(parseFontSize("medium").has_value());
  EXPECT_DOUBLE_EQ(parseFontSize("medium")->value, 12.0);

  EXPECT_TRUE(parseFontSize("large").has_value());
  EXPECT_DOUBLE_EQ(parseFontSize("large")->value, 12.0 * 6.0 / 5.0);  // 14.4

  EXPECT_TRUE(parseFontSize("x-large").has_value());
  EXPECT_DOUBLE_EQ(parseFontSize("x-large")->value, 18.0);  // 12 * 3/2

  EXPECT_TRUE(parseFontSize("xx-large").has_value());
  EXPECT_DOUBLE_EQ(parseFontSize("xx-large")->value, 24.0);  // 12 * 2/1

  EXPECT_TRUE(parseFontSize("small").has_value());
  EXPECT_DOUBLE_EQ(parseFontSize("small")->value, 12.0 * 8.0 / 9.0);  // ~10.67

  EXPECT_TRUE(parseFontSize("x-small").has_value());
  EXPECT_DOUBLE_EQ(parseFontSize("x-small")->value, 9.0);  // 12 * 3/4

  EXPECT_TRUE(parseFontSize("xx-small").has_value());
  EXPECT_DOUBLE_EQ(parseFontSize("xx-small")->value, 12.0 * 3.0 / 5.0);  // 7.2

  // All absolute keywords are stored as Px units (already resolved).
  EXPECT_EQ(parseFontSize("medium")->unit, Lengthd::Unit::Px);
  EXPECT_EQ(parseFontSize("xx-large")->unit, Lengthd::Unit::Px);
  EXPECT_EQ(parseFontSize("xx-small")->unit, Lengthd::Unit::Px);
}

TEST(PropertyRegistry, FontSizeRelativeKeywordsAndResolution) {
  {
    PropertyRegistry registry;
    registry.parseStyle("font-size: larger");
    EXPECT_THAT(registry.fontSize.get(), Optional(Lengthd(120, Lengthd::Unit::Percent)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-size: smaller");
    ASSERT_TRUE(registry.fontSize.get().has_value());
    EXPECT_DOUBLE_EQ(registry.fontSize.get()->value, 100.0 / 1.2);
    EXPECT_EQ(registry.fontSize.get()->unit, Lengthd::Unit::Percent);
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-size: 150%");
    registry.resolveFontSize(20.0);
    EXPECT_THAT(registry.fontSize.get(), Optional(Lengthd(30, Lengthd::Unit::Px)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-size: 2em");
    registry.resolveFontSize(18.0);
    EXPECT_THAT(registry.fontSize.get(), Optional(Lengthd(36, Lengthd::Unit::Px)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-size: 3ex");
    registry.resolveFontSize(10.0);
    EXPECT_THAT(registry.fontSize.get(), Optional(Lengthd(15, Lengthd::Unit::Px)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-size: 2rem");
    registry.resolveFontSize(99.0);
    EXPECT_THAT(registry.fontSize.get(), Optional(Lengthd(24, Lengthd::Unit::Px)));
  }
}

TEST(PropertyRegistry, FontWeight) {
  {
    PropertyRegistry registry;
    registry.parseStyle("font-weight: normal");
    EXPECT_THAT(registry.fontWeight.get(), Optional(400));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-weight: bold");
    EXPECT_THAT(registry.fontWeight.get(), Optional(700));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-weight: 550");
    EXPECT_THAT(registry.fontWeight.get(), Optional(550));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-weight: bolder");
    EXPECT_THAT(registry.fontWeight.get(), Optional(PropertyRegistry::kFontWeightBolder));
    registry.resolveFontWeight(300);
    EXPECT_THAT(registry.fontWeight.get(), Optional(400));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-weight: bolder");
    registry.resolveFontWeight(500);
    EXPECT_THAT(registry.fontWeight.get(), Optional(700));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-weight: bolder");
    registry.resolveFontWeight(800);
    EXPECT_THAT(registry.fontWeight.get(), Optional(900));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-weight: lighter");
    EXPECT_THAT(registry.fontWeight.get(), Optional(PropertyRegistry::kFontWeightLighter));
    registry.resolveFontWeight(500);
    EXPECT_THAT(registry.fontWeight.get(), Optional(100));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-weight: lighter");
    registry.resolveFontWeight(600);
    EXPECT_THAT(registry.fontWeight.get(), Optional(400));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-weight: lighter");
    registry.resolveFontWeight(900);
    EXPECT_THAT(registry.fontWeight.get(), Optional(700));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("font-weight: invalid");
    EXPECT_FALSE(registry.fontWeight.isSpecified());
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("font-weight", "bold"), ParseResultIs(true));
    EXPECT_THAT(registry.fontWeight.get(), Optional(700));
  }
}

TEST(PropertyRegistry, InheritFromNoPaintSkipsPaintServers) {
  PropertyRegistry parent;
  parent.parseStyle("color: red; fill: lime");

  PropertyRegistry child;
  const PropertyRegistry inherited = child.inheritFrom(parent, PropertyInheritOptions::NoPaint);

  EXPECT_THAT(inherited.color.get(), Optional(Color(RGBA(0xFF, 0, 0, 0xFF))));
  EXPECT_FALSE(inherited.fill.isSpecified());
  EXPECT_THAT(inherited.fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0, 0, 0xFF))))));
}

TEST(PropertyRegistry, IsPresentationAttributeInherited) {
  EXPECT_TRUE(PropertyRegistry::isPresentationAttributeInherited("color"));
  EXPECT_TRUE(PropertyRegistry::isPresentationAttributeInherited("stroke-width"));
  EXPECT_FALSE(PropertyRegistry::isPresentationAttributeInherited("clip-path"));
  EXPECT_FALSE(PropertyRegistry::isPresentationAttributeInherited("transform"));
  EXPECT_FALSE(PropertyRegistry::isPresentationAttributeInherited("not-a-property"));
}

TEST(PropertyRegistry, AdditionalKeywordProperties) {
  {
    const std::pair<const char*, Isolation> cases[] = {
        {"auto", Isolation::Auto},
        {"isolate", Isolation::Isolate},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("isolation: ") + value);
      EXPECT_THAT(registry.isolation.get(), Optional(expected));
    }
  }

  {
    const std::pair<const char*, DominantBaseline> cases[] = {
        {"auto", DominantBaseline::Auto},
        {"text-bottom", DominantBaseline::TextBottom},
        {"text-after-edge", DominantBaseline::TextBottom},
        {"alphabetic", DominantBaseline::Alphabetic},
        {"ideographic", DominantBaseline::Ideographic},
        {"middle", DominantBaseline::Middle},
        {"central", DominantBaseline::Central},
        {"mathematical", DominantBaseline::Mathematical},
        {"hanging", DominantBaseline::Hanging},
        {"text-top", DominantBaseline::TextTop},
        {"text-before-edge", DominantBaseline::TextTop},
        {"use-script", DominantBaseline::UseScript},
        {"no-change", DominantBaseline::NoChange},
        {"reset-size", DominantBaseline::ResetSize},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("dominant-baseline: ") + value);
      EXPECT_THAT(registry.dominantBaseline.get(), Optional(expected));
    }
  }

  {
    const std::pair<const char*, DominantBaseline> cases[] = {
        {"auto", DominantBaseline::Auto},
        {"baseline", DominantBaseline::Auto},
        {"before-edge", DominantBaseline::TextTop},
        {"text-before-edge", DominantBaseline::TextTop},
        {"text-top", DominantBaseline::TextTop},
        {"middle", DominantBaseline::Middle},
        {"central", DominantBaseline::Central},
        {"after-edge", DominantBaseline::TextBottom},
        {"text-after-edge", DominantBaseline::TextBottom},
        {"text-bottom", DominantBaseline::TextBottom},
        {"ideographic", DominantBaseline::Ideographic},
        {"alphabetic", DominantBaseline::Alphabetic},
        {"hanging", DominantBaseline::Hanging},
        {"mathematical", DominantBaseline::Mathematical},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("alignment-baseline: ") + value);
      EXPECT_THAT(registry.alignmentBaseline.get(), Optional(expected));
    }

    {
      // `use-script` / `no-change` / `reset-size` are dominant-baseline-only keywords.
      PropertyRegistry registry;
      registry.parseStyle("alignment-baseline: use-script");
      EXPECT_THAT(registry.alignmentBaseline.get(), Optional(DominantBaseline::Auto));
    }
  }

  {
    const std::pair<const char*, MixBlendMode> cases[] = {
        {"normal", MixBlendMode::Normal},
        {"multiply", MixBlendMode::Multiply},
        {"screen", MixBlendMode::Screen},
        {"overlay", MixBlendMode::Overlay},
        {"darken", MixBlendMode::Darken},
        {"lighten", MixBlendMode::Lighten},
        {"color-dodge", MixBlendMode::ColorDodge},
        {"color-burn", MixBlendMode::ColorBurn},
        {"hard-light", MixBlendMode::HardLight},
        {"soft-light", MixBlendMode::SoftLight},
        {"difference", MixBlendMode::Difference},
        {"exclusion", MixBlendMode::Exclusion},
        {"hue", MixBlendMode::Hue},
        {"saturation", MixBlendMode::Saturation},
        {"color", MixBlendMode::Color},
        {"luminosity", MixBlendMode::Luminosity},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("mix-blend-mode: ") + value);
      EXPECT_THAT(registry.mixBlendMode.get(), Optional(expected));
    }
  }

  {
    const std::pair<const char*, WritingMode> cases[] = {
        {"horizontal-tb", WritingMode::HorizontalTb},
        {"vertical-rl", WritingMode::VerticalRl},
        {"vertical-lr", WritingMode::VerticalLr},
        {"lr-tb", WritingMode::HorizontalTb},
        {"lr", WritingMode::HorizontalTb},
        {"rl-tb", WritingMode::HorizontalTb},
        {"rl", WritingMode::HorizontalTb},
        {"tb-rl", WritingMode::VerticalRl},
        {"tb", WritingMode::VerticalRl},
        {"tb-lr", WritingMode::VerticalLr},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("writing-mode: ") + value);
      EXPECT_THAT(registry.writingMode.get(), Optional(expected));
    }
  }

  {
    // image-rendering accepts the CSS Images 3 keywords plus the SVG 1.1
    // legacy aliases. CSS keyword matching is case-insensitive, so the
    // camelCase SVG 1.1 spellings AND their minifier-lowercased forms
    // both resolve to the same enum.
    const std::pair<const char*, ImageRendering> cases[] = {
        {"auto", ImageRendering::Auto},
        {"smooth", ImageRendering::Smooth},
        {"crisp-edges", ImageRendering::CrispEdges},
        {"pixelated", ImageRendering::Pixelated},
        {"optimizeSpeed", ImageRendering::OptimizeSpeed},
        {"optimizespeed", ImageRendering::OptimizeSpeed},
        {"OPTIMIZESPEED", ImageRendering::OptimizeSpeed},
        {"optimizeQuality", ImageRendering::OptimizeQuality},
        {"optimizequality", ImageRendering::OptimizeQuality},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("image-rendering: ") + value);
      EXPECT_THAT(registry.imageRendering.get(), Optional(expected));
    }
  }

  {
    const std::pair<const char*, FillRule> cases[] = {
        {"nonzero", FillRule::NonZero},
        {"evenodd", FillRule::EvenOdd},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("fill-rule: ") + value);
      EXPECT_THAT(registry.fillRule.get(), Optional(expected));
    }
  }

  {
    const std::pair<const char*, ClipRule> cases[] = {
        {"nonzero", ClipRule::NonZero},
        {"evenodd", ClipRule::EvenOdd},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("clip-rule: ") + value);
      EXPECT_THAT(registry.clipRule.get(), Optional(expected));
    }
  }

  {
    const std::pair<const char*, ColorInterpolationFilters> cases[] = {
        {"sRGB", ColorInterpolationFilters::SRGB},
        {"linearRGB", ColorInterpolationFilters::LinearRGB},
        {"auto", ColorInterpolationFilters::LinearRGB},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("color-interpolation-filters: ") + value);
      EXPECT_THAT(registry.colorInterpolationFilters.get(), Optional(expected));
    }
  }

  {
    const std::pair<const char*, StrokeLinecap> cases[] = {
        {"butt", StrokeLinecap::Butt},
        {"round", StrokeLinecap::Round},
        {"square", StrokeLinecap::Square},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("stroke-linecap: ") + value);
      EXPECT_THAT(registry.strokeLinecap.get(), Optional(expected));
    }
  }

  {
    const std::pair<const char*, StrokeLinejoin> cases[] = {
        {"miter", StrokeLinejoin::Miter}, {"miter-clip", StrokeLinejoin::MiterClip},
        {"round", StrokeLinejoin::Round}, {"bevel", StrokeLinejoin::Bevel},
        {"arcs", StrokeLinejoin::Arcs},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("stroke-linejoin: ") + value);
      EXPECT_THAT(registry.strokeLinejoin.get(), Optional(expected));
    }
  }

  {
    const std::pair<const char*, PointerEvents> cases[] = {
        {"none", PointerEvents::None},
        {"bounding-box", PointerEvents::BoundingBox},
        {"visiblePainted", PointerEvents::VisiblePainted},
        {"visible", PointerEvents::Visible},
        {"painted", PointerEvents::Painted},
        {"fill", PointerEvents::Fill},
        {"stroke", PointerEvents::Stroke},
        {"all", PointerEvents::All},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("pointer-events: ") + value);
      EXPECT_THAT(registry.pointerEvents.get(), Optional(expected));
    }
  }
}

TEST(PropertyRegistry, DisplayAnchorVisibilityOverflowAndPointerEventsErrors) {
  {
    const std::pair<const char*, Display> cases[] = {
        {"inline", Display::Inline},
        {"block", Display::Block},
        {"list-item", Display::ListItem},
        {"inline-block", Display::InlineBlock},
        {"table", Display::Table},
        {"inline-table", Display::InlineTable},
        {"table-row-group", Display::TableRowGroup},
        {"table-header-group", Display::TableHeaderGroup},
        {"table-footer-group", Display::TableFooterGroup},
        {"table-row", Display::TableRow},
        {"table-column-group", Display::TableColumnGroup},
        {"table-column", Display::TableColumn},
        {"table-cell", Display::TableCell},
        {"table-caption", Display::TableCaption},
        {"none", Display::None},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("display: ") + value);
      EXPECT_THAT(registry.display.get(), Optional(expected));
    }
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("display: invalid").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid display value"));
  }

  {
    const std::pair<const char*, TextAnchor> cases[] = {
        {"start", TextAnchor::Start},
        {"middle", TextAnchor::Middle},
        {"end", TextAnchor::End},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("text-anchor: ") + value);
      EXPECT_THAT(registry.textAnchor.get(), Optional(expected));
    }
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("text-anchor: invalid").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid text-anchor value"));
  }

  {
    const std::pair<const char*, Visibility> cases[] = {
        {"visible", Visibility::Visible},
        {"hidden", Visibility::Hidden},
        {"collapse", Visibility::Collapse},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("visibility: ") + value);
      EXPECT_THAT(registry.visibility.get(), Optional(expected));
    }
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("visibility: invalid").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid display value"));
  }

  {
    const std::pair<const char*, Overflow> cases[] = {
        {"visible", Overflow::Visible},
        {"hidden", Overflow::Hidden},
        {"scroll", Overflow::Scroll},
        {"auto", Overflow::Auto},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("overflow: ") + value);
      EXPECT_THAT(registry.overflow.get(), Optional(expected));
    }
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("overflow: invalid").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid overflow value"));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("pointer-events: visibleFill");
    EXPECT_THAT(registry.pointerEvents.get(), Optional(PointerEvents::VisibleFill));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("pointer-events: visibleStroke");
    EXPECT_THAT(registry.pointerEvents.get(), Optional(PointerEvents::VisibleStroke));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("pointer-events: invalid").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid pointer-events"));
  }

  {
    const auto invalidCases = {
        "isolation: invalid",       "dominant-baseline: invalid",
        "mix-blend-mode: invalid",  "writing-mode: invalid",
        "fill-rule: invalid",       "color-interpolation-filters: invalid",
        "clip-rule: invalid",       "stroke-linecap: invalid",
        "stroke-linejoin: invalid",
    };

    for (const char* property : invalidCases) {
      PropertyRegistry registry;
      css::Declaration declaration = css::CSS::ParseStyleAttribute(property).at(0);
      EXPECT_TRUE(registry.parseProperty(declaration, Specificity()).has_value());
    }
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("text-decoration: overline line-through");
    EXPECT_THAT(registry.textDecoration.get(),
                Optional(TextDecoration::Overline | TextDecoration::LineThrough));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("text-decoration: none");
    EXPECT_THAT(registry.textDecoration.get(), Optional(TextDecoration::None));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("baseline-shift: 2px");
    EXPECT_THAT(registry.baselineShift.get(), Optional(Lengthd(2, Lengthd::Unit::Px)));
  }
}

TEST(PropertyRegistry, EmptyComponentDeclarationsReportPropertySpecificErrors) {
  const std::pair<const char*, const char*> cases[] = {
      {"display", "Invalid display value"},
      {"text-anchor", "Invalid text-anchor value"},
      {"isolation", "Invalid isolation value"},
      {"image-rendering", "Invalid image-rendering value"},
      {"paint-order", "Invalid paint-order value"},
      {"dominant-baseline", "Invalid dominant-baseline value"},
      {"alignment-baseline", "Invalid alignment-baseline value"},
      {"mix-blend-mode", "Invalid mix-blend-mode value"},
      {"writing-mode", "Invalid writing-mode value"},
      {"visibility", "Invalid display value"},
      {"overflow", "Invalid overflow value"},
      {"fill-rule", "Invalid fill rule"},
      {"color-interpolation-filters", "Invalid color-interpolation-filters value"},
      {"clip-rule", "Invalid clip-rule value"},
      {"stroke-linecap", "Invalid linecap"},
      {"stroke-linejoin", "Invalid linejoin"},
      {"stroke-miterlimit", "Invalid number"},
      {"pointer-events", "Invalid pointer-events"},
  };

  for (const auto& [property, reason] : cases) {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parseProperty(css::Declaration(property, {}), Specificity()),
                ParseErrorIs(reason))
        << property;
  }
}

TEST(PropertyRegistry, NonIdentSingleTokenDeclarationsReportPropertySpecificErrors) {
  const std::pair<const char*, const char*> cases[] = {
      {"display", "Invalid display value"},
      {"text-anchor", "Invalid text-anchor value"},
      {"isolation", "Invalid isolation value"},
      {"image-rendering", "Invalid image-rendering value"},
      {"paint-order", "Invalid paint-order value"},
      {"dominant-baseline", "Invalid dominant-baseline value"},
      {"alignment-baseline", "Invalid alignment-baseline value"},
      {"mix-blend-mode", "Invalid mix-blend-mode value"},
      {"writing-mode", "Invalid writing-mode value"},
      {"visibility", "Invalid display value"},
      {"overflow", "Invalid overflow value"},
      {"fill-rule", "Invalid fill rule"},
      {"color-interpolation-filters", "Invalid color-interpolation-filters value"},
      {"clip-rule", "Invalid clip-rule value"},
      {"stroke-linecap", "Invalid linecap"},
      {"stroke-linejoin", "Invalid linejoin"},
      {"pointer-events", "Invalid pointer-events"},
  };

  const ComponentValue number(Token(Token::Number(1.0, "1", css::NumberType::Integer), 0));
  for (const auto& [property, reason] : cases) {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parseProperty(css::Declaration(property, {number}), Specificity()),
                ParseErrorIs(reason))
        << property;
  }
}

TEST(PropertyRegistry, PaintOrderGrammar) {
  {
    const std::pair<const char*, PaintOrder> cases[] = {
        {"normal", PaintOrder{}},
        {"stroke",
         PaintOrder{{PaintComponent::Stroke, PaintComponent::Fill, PaintComponent::Markers}}},
        {"markers fill",
         PaintOrder{{PaintComponent::Markers, PaintComponent::Fill, PaintComponent::Stroke}}},
        {"stroke markers fill",
         PaintOrder{{PaintComponent::Stroke, PaintComponent::Markers, PaintComponent::Fill}}},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("paint-order: ") + value);
      EXPECT_THAT(registry.paintOrder.get(), Optional(expected)) << value;
    }
  }

  {
    const char* invalidCases[] = {
        "paint-order: ",
        "paint-order: fill fill",
        "paint-order: stroke stroke",
        "paint-order: markers markers",
        "paint-order: fill stroke markers fill",
        "paint-order: fill / stroke",
        "paint-order: bogus",
        "paint-order: normal fill",
    };

    for (const char* property : invalidCases) {
      PropertyRegistry registry;
      css::Declaration declaration = css::CSS::ParseStyleAttribute(property).at(0);
      EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                  ParseErrorIs("Invalid paint-order value"))
          << property;
    }
  }
}

TEST(PropertyRegistry, PaintReferenceTransformOriginAndFilterFunctionEdges) {
  {
    PropertyRegistry registry;
    registry.parseStyle("fill: url(#paint)");
    ASSERT_TRUE(registry.fill.isSpecified());
    EXPECT_THAT(registry.fill.get(),
                Optional(PaintServer(PaintServer::ElementReference("#paint"))));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("fill: url(#paint) none");
    ASSERT_TRUE(registry.fill.isSpecified());
    EXPECT_THAT(registry.fill.get(),
                Optional(PaintServer(PaintServer::ElementReference("#paint"))));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("fill: url(#paint) red");
    ASSERT_TRUE(registry.fill.isSpecified());
    EXPECT_THAT(registry.fill.get(), Optional(PaintServer(PaintServer::ElementReference(
                                         "#paint", Color(RGBA(0xFF, 0x00, 0x00, 0xFF))))));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("fill: context-fill red").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Unexpected tokens after paint server value"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("fill: url(#paint) bogus").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid paint server url, failed to parse fallback"));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("clip-path: url(#clip)");
    ASSERT_TRUE(registry.clipPath.isSpecified());
    EXPECT_THAT(registry.clipPath.get(), Optional(Reference("#clip")));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("clip-path: invalid").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid url reference"));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("clip-path", ""),
                ParseErrorIs("Empty clip-path value"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("transform-origin: invalid top").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid length or percentage"));
  }

  {
    // After #514, single-keyword transform-origin is valid. "left,top" parses
    // "left" as a valid single keyword; the comma is a declaration separator
    // and "top" is a separate (ignored) token. This is correct per CSS spec.
    PropertyRegistry registry;
    registry.parseStyle("transform-origin: left,top");
    ASSERT_TRUE(registry.transformOrigin.isSpecified());
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("transform-origin: left invalid").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid length or percentage"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("transform-origin: left top 1px").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Unexpected token in transform-origin"));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: blur()");
    const auto& blur = (*registry.filter.getStoredValue()).front().get<FilterEffect::Blur>();
    EXPECT_EQ(blur.stdDeviationX, Lengthd(0, Lengthd::Unit::Px));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: blur( )");
    const auto& blur = (*registry.filter.getStoredValue()).front().get<FilterEffect::Blur>();
    EXPECT_EQ(blur.stdDeviationX, Lengthd(0, Lengthd::Unit::Px));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: blur(0)");
    const auto& blur = (*registry.filter.getStoredValue()).front().get<FilterEffect::Blur>();
    EXPECT_EQ(blur.stdDeviationX, Lengthd(0, Lengthd::Unit::Px));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: hue-rotate()");
    EXPECT_DOUBLE_EQ(
        (*registry.filter.getStoredValue()).front().get<FilterEffect::HueRotate>().angleDegrees,
        0.0);
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: hue-rotate( )");
    EXPECT_DOUBLE_EQ(
        (*registry.filter.getStoredValue()).front().get<FilterEffect::HueRotate>().angleDegrees,
        0.0);
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: hue-rotate(45deg)");
    EXPECT_DOUBLE_EQ(
        (*registry.filter.getStoredValue()).front().get<FilterEffect::HueRotate>().angleDegrees,
        45.0);
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("filter: hue-rotate(1px)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid angle unit for hue-rotate"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("filter: hue-rotate(invalid)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid hue-rotate value"));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: brightness( )");
    EXPECT_DOUBLE_EQ(
        (*registry.filter.getStoredValue()).front().get<FilterEffect::Brightness>().amount, 1.0);
  }

  {
    const char* invalidFilters[] = {
        "brightness(invalid)", "contrast(invalid)", "grayscale(invalid)", "invert(invalid)",
        "opacity(invalid)",    "saturate(invalid)", "sepia(invalid)"};

    for (const char* filter : invalidFilters) {
      PropertyRegistry registry;
      css::Declaration declaration =
          css::CSS::ParseStyleAttribute(std::string("filter: ") + filter).at(0);
      EXPECT_TRUE(registry.parseProperty(declaration, Specificity()).has_value());
    }
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: drop-shadow(1 2)");
    const auto& shadow =
        (*registry.filter.getStoredValue()).front().get<FilterEffect::DropShadow>();
    EXPECT_EQ(shadow.offsetX, Lengthd(1, Lengthd::Unit::None));
    EXPECT_EQ(shadow.offsetY, Lengthd(2, Lengthd::Unit::None));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("filter: drop-shadow(red foo)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Expected offset-x for drop-shadow"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("filter: drop-shadow(1px foo)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Expected offset-y for drop-shadow"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("filter: unknown(1)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Unknown filter function 'unknown'"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("filter: blur(1foo)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid unit on blur length"));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("filter", ""),
                ParseErrorIs("Invalid filter value"));
    EXPECT_THAT(registry.parsePresentationAttribute("filter", " "),
                ParseErrorIs("Invalid filter value"));
    EXPECT_THAT(registry.parsePresentationAttribute("filter", ","),
                ParseErrorIs("Invalid filter value"));
    EXPECT_THAT(registry.parsePresentationAttribute("filter", "/"),
                ParseErrorIs("Invalid filter value"));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: url(#a), url(#b)");
    const auto& effects = (*registry.filter.getStoredValue());
    EXPECT_THAT(effects, testing::ElementsAre(FilterReferenceIs("#a"), FilterReferenceIs("#b")));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: url(#a) ");
    EXPECT_THAT(*registry.filter.getStoredValue(), testing::ElementsAre(FilterReferenceIs("#a")));
  }
}

TEST(PropertyRegistry, BaselineShiftSpacingAndDasharray) {
  {
    PropertyRegistry registry;
    registry.parseStyle("baseline-shift: baseline");
    EXPECT_THAT(registry.baselineShift.get(), Optional(Lengthd(0, Lengthd::Unit::None)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("baseline-shift: sub");
    EXPECT_THAT(registry.baselineShift.get(), Optional(Lengthd(-0.33, Lengthd::Unit::Em)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("baseline-shift: super");
    EXPECT_THAT(registry.baselineShift.get(), Optional(Lengthd(0.4, Lengthd::Unit::Em)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("baseline-shift: 50%");
    EXPECT_THAT(registry.baselineShift.get(), Optional(Lengthd(0.5, Lengthd::Unit::Em)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("letter-spacing: normal; word-spacing: 2px");
    EXPECT_THAT(registry.letterSpacing.get(), Optional(Lengthd(0, Lengthd::Unit::None)));
    EXPECT_THAT(registry.wordSpacing.get(), Optional(Lengthd(2, Lengthd::Unit::Px)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("stroke-dasharray: 1 2, 3%");
    ASSERT_TRUE(registry.strokeDasharray.isSpecified());
    const auto& dasharray = (*registry.strokeDasharray.getStoredValue());
    EXPECT_THAT(dasharray, StrokeDasharrayIs(std::vector<Lengthd>{
                               Lengthd(1, Lengthd::Unit::None), Lengthd(2, Lengthd::Unit::None),
                               Lengthd(3, Lengthd::Unit::Percent)}));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("stroke-dasharray: 1px foo").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Unexpected token in dasharray"));
  }
}

TEST(PropertyRegistry, FilterParsing) {
  {
    PropertyRegistry registry;
    registry.parseStyle("filter: none");
    ASSERT_TRUE(registry.filter.isSpecified());
    EXPECT_THAT(*registry.filter.getStoredValue(), testing::IsEmpty());
  }

  {
    PropertyRegistry registry;
    registry.parseStyle(
        "filter: blur(2px) hue-rotate(0.5turn) brightness(50%) contrast(2) grayscale() "
        "invert(25%) opacity(0.4) saturate(3) sepia() url(#f)");
    ASSERT_TRUE(registry.filter.isSpecified());
    const auto& effects = (*registry.filter.getStoredValue());
    EXPECT_THAT(effects, testing::ElementsAre(
                             FilterBlurIs(2.0, Lengthd::Unit::Px), FilterHueRotateIs(180.0, 0.0),
                             FilterBrightnessIs(0.5), FilterContrastIs(2.0), FilterGrayscaleIs(1.0),
                             FilterInvertIs(0.25), FilterOpacityIs(0.4), FilterSaturateIs(3.0),
                             FilterSepiaIs(1.0), FilterReferenceIs("#f")));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: drop-shadow(red 1px 2px 3px)");
    ASSERT_TRUE(registry.filter.isSpecified());
    const auto& effects = (*registry.filter.getStoredValue());
    EXPECT_THAT(effects, testing::ElementsAre(FilterDropShadowIs(
                             Lengthd(1, Lengthd::Unit::Px), Lengthd(2, Lengthd::Unit::Px),
                             Lengthd(3, Lengthd::Unit::Px), Color(RGBA(0xFF, 0, 0, 0xFF)))));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("filter: blur(1%)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid blur value"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("filter: brightness(-1)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Negative value not allowed for brightness()"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("filter: drop-shadow(1px)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Expected offset-y for drop-shadow"));
  }
}

TEST(PropertyRegistry, TransformPresentationAttribute) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  PropertyRegistry registry;

  EXPECT_THAT(
      registry.parsePresentationAttribute("transform", "translate(10 20)", rect.entityHandle()),
      ParseResultIs(true));
  ASSERT_TRUE(rect.entityHandle().all_of<components::TransformComponent>());
  EXPECT_TRUE(rect.entityHandle().get<components::TransformComponent>().transform.isSpecified());
}

TEST(PropertyRegistry, TransformOriginStrokeMiterlimitAndFilterEdgeCases) {
  {
    PropertyRegistry registry;
    registry.parseStyle("transform-origin: left top");
    const TransformOrigin origin = registry.transformOrigin.get().value();
    EXPECT_EQ(origin.x, Lengthd(0, Lengthd::Unit::Percent));
    EXPECT_EQ(origin.y, Lengthd(0, Lengthd::Unit::Percent));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("transform-origin: right bottom");
    const TransformOrigin origin = registry.transformOrigin.get().value();
    EXPECT_EQ(origin.x, Lengthd(100, Lengthd::Unit::Percent));
    EXPECT_EQ(origin.y, Lengthd(100, Lengthd::Unit::Percent));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("transform-origin: center center");
    const TransformOrigin origin = registry.transformOrigin.get().value();
    EXPECT_EQ(origin.x, Lengthd(50, Lengthd::Unit::Percent));
    EXPECT_EQ(origin.y, Lengthd(50, Lengthd::Unit::Percent));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("stroke-miterlimit: 7");
    EXPECT_THAT(registry.strokeMiterlimit.get(), Optional(7.0));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("stroke-miterlimit: invalid").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()), ParseErrorIs("Invalid number"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("stroke-miterlimit: 1 2").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()), ParseErrorIs("Invalid number"));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: hue-rotate(200grad) hue-rotate(3.141592653589793rad)");
    const auto& effects = (*registry.filter.getStoredValue());
    EXPECT_THAT(effects, testing::ElementsAre(FilterHueRotateIs(180.0, 0.0),
                                              FilterHueRotateIs(180.0, 1e-9)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: hue-rotate(0)");
    const auto& effects = (*registry.filter.getStoredValue());
    EXPECT_THAT(effects, testing::ElementsAre(FilterHueRotateIs(0.0, 0.0)));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: url(\"#quoted\")");
    const auto& effects = (*registry.filter.getStoredValue());
    EXPECT_THAT(effects, testing::ElementsAre(FilterReferenceIs("#quoted")));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: drop-shadow(1px 2px red)");
    const auto& shadow =
        (*registry.filter.getStoredValue()).front().get<FilterEffect::DropShadow>();
    EXPECT_EQ(shadow.color, Color(RGBA(0xFF, 0, 0, 0xFF)));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("filter: drop-shadow(1% 2px)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Expected offset-x for drop-shadow"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("filter: drop-shadow(1px 2%)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Expected offset-y for drop-shadow"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("filter: drop-shadow(1px 2px 3%)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Unexpected extra values in drop-shadow()"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("filter: contrast(-1)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Negative value not allowed for contrast()"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("filter: saturate(-1)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Negative value not allowed for saturate()"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("filter: drop-shadow(1px 2px 3px red extra)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Unexpected extra values in drop-shadow()"));
  }
}

TEST(PropertyRegistry, TextDecorationAndDasharrayErrorPaths) {
  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("text-decoration: red").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid text-decoration value"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("text-decoration: underline, overline").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid text-decoration value"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("stroke-dasharray: 1px/2px").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Unexpected tokens after dasharray value"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("stroke-dasharray: 1foo").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid unit on length"));
  }
}

TEST(PropertyRegistry, ResolveFontStretch) {
  // narrower from Normal → SemiCondensed.
  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: narrower");
    registry.resolveFontStretch(static_cast<int>(FontStretch::Normal));
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::SemiCondensed)));
  }

  // wider from Normal → SemiExpanded.
  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: wider");
    registry.resolveFontStretch(static_cast<int>(FontStretch::Normal));
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::SemiExpanded)));
  }

  // narrower clamps at UltraCondensed.
  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: narrower");
    registry.resolveFontStretch(static_cast<int>(FontStretch::UltraCondensed));
    EXPECT_THAT(registry.fontStretch.get(),
                Optional(static_cast<int>(FontStretch::UltraCondensed)));
  }

  // wider clamps at UltraExpanded.
  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: wider");
    registry.resolveFontStretch(static_cast<int>(FontStretch::UltraExpanded));
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::UltraExpanded)));
  }

  // Non-sentinel values are not modified.
  {
    PropertyRegistry registry;
    registry.parseStyle("font-stretch: condensed");
    registry.resolveFontStretch(static_cast<int>(FontStretch::Normal));
    EXPECT_THAT(registry.fontStretch.get(), Optional(static_cast<int>(FontStretch::Condensed)));
  }
}

}  // namespace donner::svg
