#include "donner/svg/properties/PropertyRegistry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

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

using testing::Eq;
using testing::Ne;
using testing::Optional;

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
    EXPECT_TRUE(registry.color.hasValue());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));

    registry.parseStyle("color: inherit");
    EXPECT_EQ(registry.color.state, PropertyState::Inherit);
    EXPECT_TRUE(registry.color.hasValue());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));

    registry.parseStyle("color: unset");
    EXPECT_EQ(registry.color.state, PropertyState::ExplicitUnset);
    EXPECT_TRUE(registry.color.hasValue());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("color: inherit invalid");
    EXPECT_FALSE(registry.color.hasValue());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));
  }
}

TEST(PropertyRegistry, ParseColor) {
  PropertyRegistry registry;
  registry.parseStyle("color: red");
  EXPECT_TRUE(registry.color.hasValue());
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
    EXPECT_FALSE(registry.color.hasValue());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("color", "invalid"),
                ParseErrorIs("Invalid color 'invalid'"));
    EXPECT_FALSE(registry.color.hasValue());
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("color", "red !important"),
                ParseErrorIs("Expected a single color"));
    EXPECT_FALSE(registry.color.hasValue()) << "!important is not supported";
    EXPECT_THAT(registry.color.get(), Eq(Color(RGBA(0, 0, 0, 0xFF))));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("color", " /*comment*/ red "),
                ParseResultIs(true));
    EXPECT_TRUE(registry.color.hasValue()) << "Comments and whitespace should be ignored";
    EXPECT_THAT(registry.color.get(), Optional(Color(RGBA(0xFF, 0, 0, 0xFF))));
  }
}

TEST(PropertyRegistry, Fill) {
  // Initial value of fill is black.
  const PaintServer kInitialFill(PaintServer::Solid(Color(RGBA(0, 0, 0, 0xFF))));

  {
    PropertyRegistry registry;
    EXPECT_FALSE(registry.fill.hasValue());
    EXPECT_THAT(registry.fill.get(), Optional(kInitialFill));

    registry.parseStyle("fill: none");
    EXPECT_TRUE(registry.fill.hasValue());
    EXPECT_THAT(registry.fill.get(), Optional(PaintServer(PaintServer::None())));

    registry.parseStyle("fill: red  ");
    EXPECT_THAT(registry.fill.get(),
                Optional(PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0, 0, 0xFF))))));
  }

  {
    PropertyRegistry registry;
    EXPECT_THAT(registry.parsePresentationAttribute("fill", ""),
                ParseErrorIs("Invalid paint server value"));
    EXPECT_FALSE(registry.fill.hasValue());
    EXPECT_THAT(registry.fill.get(), Optional(kInitialFill));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("fill: red asdf");
    EXPECT_FALSE(registry.fill.hasValue());
    EXPECT_THAT(registry.fill.get(), Optional(kInitialFill));

    registry.parseStyle("fill: asdf");
    EXPECT_FALSE(registry.fill.hasValue());
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
    EXPECT_FALSE(registry.fill.hasValue());
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
    EXPECT_FALSE(registry.fill.hasValue());
    EXPECT_THAT(registry.fill.get(), Optional(kInitialFill));
  }
}

TEST(PropertyRegistry, Stroke) {
  // Initial value of stroke is none.
  const PaintServer kInitialFill(PaintServer::None{});

  {
    PropertyRegistry registry;
    EXPECT_FALSE(registry.stroke.hasValue());
    EXPECT_THAT(registry.stroke.get(), Optional(kInitialFill));

    registry.parseStyle("stroke: none");
    EXPECT_TRUE(registry.stroke.hasValue());
    EXPECT_THAT(registry.stroke.get(), Optional(PaintServer(PaintServer::None())));

    registry.parseStyle("stroke: red  ");
    EXPECT_THAT(registry.stroke.get(),
                Optional(PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0, 0, 0xFF))))));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("stroke: red asdf");
    EXPECT_FALSE(registry.stroke.hasValue());
    EXPECT_THAT(registry.stroke.get(), Optional(kInitialFill));

    registry.parseStyle("stroke: asdf");
    EXPECT_FALSE(registry.stroke.hasValue());
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
    EXPECT_FALSE(registry.stroke.hasValue());
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
    EXPECT_FALSE(registry.fontStyle.hasValue());
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
    EXPECT_FALSE(registry.fontStyle.hasValue());
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
    EXPECT_FALSE(registry.fontStretch.hasValue());
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
    EXPECT_FALSE(registry.fontStretch.hasValue());
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
    EXPECT_FALSE(registry.fontVariant.hasValue());
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
    EXPECT_FALSE(registry.fontVariant.hasValue());
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
    EXPECT_FALSE(registry.fontWeight.hasValue());
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
  EXPECT_FALSE(inherited.fill.hasValue());
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
        {"alphabetic", DominantBaseline::Alphabetic},
        {"ideographic", DominantBaseline::Ideographic},
        {"middle", DominantBaseline::Middle},
        {"central", DominantBaseline::Central},
        {"mathematical", DominantBaseline::Mathematical},
        {"hanging", DominantBaseline::Hanging},
        {"text-top", DominantBaseline::TextTop},
    };

    for (const auto& [value, expected] : cases) {
      PropertyRegistry registry;
      registry.parseStyle(std::string("dominant-baseline: ") + value);
      EXPECT_THAT(registry.dominantBaseline.get(), Optional(expected));
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
        {"miter", StrokeLinejoin::Miter},
        {"miter-clip", StrokeLinejoin::MiterClip},
        {"round", StrokeLinejoin::Round},
        {"bevel", StrokeLinejoin::Bevel},
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
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("pointer-events: invalid").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Invalid pointer-events"));
  }

  {
    const auto invalidCases = {
        "isolation: invalid",
        "dominant-baseline: invalid",
        "mix-blend-mode: invalid",
        "writing-mode: invalid",
        "fill-rule: invalid",
        "color-interpolation-filters: invalid",
        "clip-rule: invalid",
        "stroke-linecap: invalid",
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

TEST(PropertyRegistry, PaintReferenceTransformOriginAndFilterFunctionEdges) {
  {
    PropertyRegistry registry;
    registry.parseStyle("fill: url(#paint)");
    ASSERT_TRUE(registry.fill.hasValue());
    EXPECT_THAT(registry.fill.get(), Optional(PaintServer(PaintServer::ElementReference("#paint"))));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("fill: url(#paint) none");
    ASSERT_TRUE(registry.fill.hasValue());
    EXPECT_THAT(registry.fill.get(), Optional(PaintServer(PaintServer::ElementReference("#paint"))));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("fill: url(#paint) red");
    ASSERT_TRUE(registry.fill.hasValue());
    EXPECT_THAT(registry.fill.get(),
                Optional(PaintServer(PaintServer::ElementReference(
                    "#paint", Color(RGBA(0xFF, 0x00, 0x00, 0xFF))))));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("fill: context-fill red").at(0);
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
    ASSERT_TRUE(registry.clipPath.hasValue());
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
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("transform-origin: left,top").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Unexpected token in transform-origin"));
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
    const auto& blur = registry.filter.getRequiredRef().front().get<FilterEffect::Blur>();
    EXPECT_EQ(blur.stdDeviationX, Lengthd(0, Lengthd::Unit::Px));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: blur( )");
    const auto& blur = registry.filter.getRequiredRef().front().get<FilterEffect::Blur>();
    EXPECT_EQ(blur.stdDeviationX, Lengthd(0, Lengthd::Unit::Px));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: blur(0)");
    const auto& blur = registry.filter.getRequiredRef().front().get<FilterEffect::Blur>();
    EXPECT_EQ(blur.stdDeviationX, Lengthd(0, Lengthd::Unit::Px));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: hue-rotate()");
    EXPECT_DOUBLE_EQ(
        registry.filter.getRequiredRef().front().get<FilterEffect::HueRotate>().angleDegrees, 0.0);
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: hue-rotate( )");
    EXPECT_DOUBLE_EQ(
        registry.filter.getRequiredRef().front().get<FilterEffect::HueRotate>().angleDegrees, 0.0);
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: hue-rotate(45deg)");
    EXPECT_DOUBLE_EQ(
        registry.filter.getRequiredRef().front().get<FilterEffect::HueRotate>().angleDegrees, 45.0);
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
    EXPECT_DOUBLE_EQ(registry.filter.getRequiredRef().front().get<FilterEffect::Brightness>().amount,
                     1.0);
  }

  {
    const char* invalidFilters[] = {"brightness(invalid)", "contrast(invalid)",
                                    "grayscale(invalid)",  "invert(invalid)",
                                    "opacity(invalid)",    "saturate(invalid)",
                                    "sepia(invalid)"};

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
    const auto& shadow = registry.filter.getRequiredRef().front().get<FilterEffect::DropShadow>();
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
    EXPECT_THAT(registry.parsePresentationAttribute("filter", ""), ParseErrorIs("Invalid filter value"));
    EXPECT_THAT(registry.parsePresentationAttribute("filter", " "), ParseErrorIs("Invalid filter value"));
    EXPECT_THAT(registry.parsePresentationAttribute("filter", ","), ParseErrorIs("Invalid filter value"));
    EXPECT_THAT(registry.parsePresentationAttribute("filter", "/"),
                ParseErrorIs("Invalid filter value"));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: url(#a), url(#b)");
    const auto& effects = registry.filter.getRequiredRef();
    ASSERT_EQ(effects.size(), 2u);
    EXPECT_EQ(effects[0].get<FilterEffect::ElementReference>().reference, Reference("#a"));
    EXPECT_EQ(effects[1].get<FilterEffect::ElementReference>().reference, Reference("#b"));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: url(#a) ");
    ASSERT_EQ(registry.filter.getRequiredRef().size(), 1u);
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
    ASSERT_TRUE(registry.strokeDasharray.hasValue());
    const auto& dasharray = registry.strokeDasharray.getRequiredRef();
    EXPECT_EQ(dasharray.size(), 3u);
    EXPECT_EQ(dasharray[0], Lengthd(1, Lengthd::Unit::None));
    EXPECT_EQ(dasharray[1], Lengthd(2, Lengthd::Unit::None));
    EXPECT_EQ(dasharray[2], Lengthd(3, Lengthd::Unit::Percent));
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
    ASSERT_TRUE(registry.filter.hasValue());
    EXPECT_TRUE(registry.filter.getRequiredRef().empty());
  }

  {
    PropertyRegistry registry;
    registry.parseStyle(
        "filter: blur(2px) hue-rotate(0.5turn) brightness(50%) contrast(2) grayscale() "
        "invert(25%) opacity(0.4) saturate(3) sepia() url(#f)");
    ASSERT_TRUE(registry.filter.hasValue());
    const auto& effects = registry.filter.getRequiredRef();
    ASSERT_EQ(effects.size(), 10u);
    EXPECT_TRUE(effects[0].is<FilterEffect::Blur>());
    EXPECT_EQ(effects[0].get<FilterEffect::Blur>().stdDeviationX, Lengthd(2, Lengthd::Unit::Px));
    EXPECT_TRUE(effects[1].is<FilterEffect::HueRotate>());
    EXPECT_DOUBLE_EQ(effects[1].get<FilterEffect::HueRotate>().angleDegrees, 180.0);
    EXPECT_TRUE(effects[2].is<FilterEffect::Brightness>());
    EXPECT_DOUBLE_EQ(effects[2].get<FilterEffect::Brightness>().amount, 0.5);
    EXPECT_TRUE(effects[3].is<FilterEffect::Contrast>());
    EXPECT_DOUBLE_EQ(effects[3].get<FilterEffect::Contrast>().amount, 2.0);
    EXPECT_TRUE(effects[4].is<FilterEffect::Grayscale>());
    EXPECT_DOUBLE_EQ(effects[4].get<FilterEffect::Grayscale>().amount, 1.0);
    EXPECT_TRUE(effects[5].is<FilterEffect::Invert>());
    EXPECT_DOUBLE_EQ(effects[5].get<FilterEffect::Invert>().amount, 0.25);
    EXPECT_TRUE(effects[6].is<FilterEffect::FilterOpacity>());
    EXPECT_DOUBLE_EQ(effects[6].get<FilterEffect::FilterOpacity>().amount, 0.4);
    EXPECT_TRUE(effects[7].is<FilterEffect::Saturate>());
    EXPECT_DOUBLE_EQ(effects[7].get<FilterEffect::Saturate>().amount, 3.0);
    EXPECT_TRUE(effects[8].is<FilterEffect::Sepia>());
    EXPECT_DOUBLE_EQ(effects[8].get<FilterEffect::Sepia>().amount, 1.0);
    EXPECT_TRUE(effects[9].is<FilterEffect::ElementReference>());
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: drop-shadow(red 1px 2px 3px)");
    ASSERT_TRUE(registry.filter.hasValue());
    const auto& effects = registry.filter.getRequiredRef();
    ASSERT_EQ(effects.size(), 1u);
    const auto& shadow = effects.front().get<FilterEffect::DropShadow>();
    EXPECT_EQ(shadow.offsetX, Lengthd(1, Lengthd::Unit::Px));
    EXPECT_EQ(shadow.offsetY, Lengthd(2, Lengthd::Unit::Px));
    EXPECT_EQ(shadow.stdDeviation, Lengthd(3, Lengthd::Unit::Px));
    EXPECT_EQ(shadow.color, Color(RGBA(0xFF, 0, 0, 0xFF)));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration = css::CSS::ParseStyleAttribute("filter: blur(1%)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()), ParseErrorIs("Invalid blur value"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("filter: brightness(-1)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Negative value not allowed for brightness()"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("filter: drop-shadow(1px)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Expected offset-y for drop-shadow"));
  }
}

TEST(PropertyRegistry, TransformPresentationAttribute) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  PropertyRegistry registry;

  EXPECT_THAT(registry.parsePresentationAttribute("transform", "translate(10 20)",
                                                  rect.entityHandle()),
              ParseResultIs(true));
  ASSERT_TRUE(rect.entityHandle().all_of<components::TransformComponent>());
  EXPECT_TRUE(rect.entityHandle().get<components::TransformComponent>().transform.hasValue());
}

TEST(PropertyRegistry, TransformOriginStrokeMiterlimitAndFilterEdgeCases) {
  {
    PropertyRegistry registry;
    registry.parseStyle("transform-origin: left top");
    const TransformOrigin origin = registry.transformOrigin.getRequired();
    EXPECT_EQ(origin.x, Lengthd(0, Lengthd::Unit::Percent));
    EXPECT_EQ(origin.y, Lengthd(0, Lengthd::Unit::Percent));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("transform-origin: right bottom");
    const TransformOrigin origin = registry.transformOrigin.getRequired();
    EXPECT_EQ(origin.x, Lengthd(100, Lengthd::Unit::Percent));
    EXPECT_EQ(origin.y, Lengthd(100, Lengthd::Unit::Percent));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("transform-origin: center center");
    const TransformOrigin origin = registry.transformOrigin.getRequired();
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
    registry.parseStyle("filter: hue-rotate(200grad) hue-rotate(3.141592653589793rad)");
    const auto& effects = registry.filter.getRequiredRef();
    ASSERT_EQ(effects.size(), 2u);
    EXPECT_DOUBLE_EQ(effects[0].get<FilterEffect::HueRotate>().angleDegrees, 180.0);
    EXPECT_NEAR(effects[1].get<FilterEffect::HueRotate>().angleDegrees, 180.0, 1e-9);
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: hue-rotate(0)");
    const auto& effects = registry.filter.getRequiredRef();
    ASSERT_EQ(effects.size(), 1u);
    EXPECT_DOUBLE_EQ(effects[0].get<FilterEffect::HueRotate>().angleDegrees, 0.0);
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: url(\"#quoted\")");
    const auto& effects = registry.filter.getRequiredRef();
    ASSERT_EQ(effects.size(), 1u);
    EXPECT_TRUE(effects[0].is<FilterEffect::ElementReference>());
    EXPECT_EQ(effects[0].get<FilterEffect::ElementReference>().reference, Reference("#quoted"));
  }

  {
    PropertyRegistry registry;
    registry.parseStyle("filter: drop-shadow(1px 2px red)");
    const auto& shadow = registry.filter.getRequiredRef().front().get<FilterEffect::DropShadow>();
    EXPECT_EQ(shadow.color, Color(RGBA(0xFF, 0, 0, 0xFF)));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("filter: contrast(-1)").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Negative value not allowed for contrast()"));
  }

  {
    PropertyRegistry registry;
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("filter: saturate(-1)").at(0);
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
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("text-decoration: red").at(0);
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
    css::Declaration declaration =
        css::CSS::ParseStyleAttribute("stroke-dasharray: 1px/2px").at(0);
    EXPECT_THAT(registry.parseProperty(declaration, Specificity()),
                ParseErrorIs("Unexpected tokens after dasharray value"));
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
