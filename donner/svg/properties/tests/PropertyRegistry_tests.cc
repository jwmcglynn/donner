#include "donner/svg/properties/PropertyRegistry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"

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
  std::optional<ParseError> parseProperty(const css::Declaration& declaration);

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
