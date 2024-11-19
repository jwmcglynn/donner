#include "donner/svg/properties/PropertyRegistry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"

namespace donner::svg {

using namespace base::parser;  // NOLINT: For tests

using css::Color;
using css::ComponentValue;
using css::Declaration;
using css::RGBA;
using css::Specificity;
using css::Token;

using testing::Eq;
using testing::Ne;
using testing::Optional;

TEST(PropertyRegistry, ParseDeclaration) {
  css::Declaration declaration("color", {ComponentValue(Token(Token::Ident("lime"), 0))});

  PropertyRegistry registry;
  EXPECT_THAT(registry.parseProperty(declaration, Specificity()), Eq(std::nullopt));
  EXPECT_THAT(registry.color.get(), Optional(Color(RGBA(0, 0xFF, 0, 0xFF))));

  // Test printing to string.
  EXPECT_THAT(registry, ToStringIs(R"(PropertyRegistry {
  color: Color(rgba(0, 255, 0, 255)) (set) @ Specificity(0, 0, 0)
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

}  // namespace donner::svg
