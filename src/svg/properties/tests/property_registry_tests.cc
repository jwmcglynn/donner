#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/svg/properties/property_registry.h"

namespace donner::svg {

using css::Color;
using css::ComponentValue;
using css::Declaration;
using css::RGBA;
using css::Token;

using testing::Eq;
using testing::Ne;
using testing::Optional;

TEST(PropertyRegistry, ParseDeclaration) {
  css::Declaration declaration("color", {ComponentValue(Token(Token::Ident("lime"), 0))});

  PropertyRegistry registry;
  EXPECT_THAT(registry.parseProperty(declaration), Eq(std::nullopt));
  EXPECT_THAT(registry.color, Optional(Color(RGBA(0, 0xFF, 0, 0xFF))));
}

TEST(PropertyRegistry, ParseDeclarationError) {
  std::optional<ParseError> parseProperty(const css::Declaration& declaration);

  css::Declaration declaration("color", {ComponentValue(Token(Token::Ident("invalid-color"), 0))});

  PropertyRegistry registry;
  EXPECT_THAT(registry.parseProperty(declaration), ParseErrorIs("Invalid color 'invalid-color'"));
}

TEST(PropertyRegistry, ParseDeclarationHash) {
  css::Declaration declaration(
      "color", {ComponentValue(Token(Token::Hash(Token::Hash::Type::Id, "FFF"), 0))});

  PropertyRegistry registry;
  EXPECT_THAT(registry.parseProperty(declaration), Eq(std::nullopt));
  EXPECT_THAT(registry.color, Optional(Color(RGBA(0xFF, 0xFF, 0xFF, 0xFF))));
}

TEST(PropertyRegistry, UnsupportedProperty) {
  css::Declaration declaration("not-supported", {ComponentValue(Token(Token::Ident("test"), 0))});

  PropertyRegistry registry;
  EXPECT_THAT(registry.parseProperty(declaration),
              Optional(ParseErrorIs("Unknown property 'not-supported'")));
  EXPECT_THAT(registry.color, Eq(std::nullopt));
}

TEST(PropertyRegistry, ParseErrorsInvalidate) {
  PropertyRegistry registry;
  registry.parseStyle("color: red");
  EXPECT_THAT(registry.color, Optional(Color(RGBA(0xFF, 0, 0, 0xFF))));

  registry.parseStyle("color: invalid");
  EXPECT_THAT(registry.color, Eq(std::nullopt));
}

TEST(PropertyRegistry, ParseColor) {
  PropertyRegistry registry;
  registry.parseStyle("color: red");
  EXPECT_THAT(registry.color, Optional(Color(RGBA(0xFF, 0, 0, 0xFF))));
}

TEST(PropertyRegistry, Fill) {
  PropertyRegistry registry;
  registry.parseStyle("fill: none");
  EXPECT_THAT(registry.fill, Optional(PaintServer(PaintServer::None())));

  registry.parseStyle("fill: red  ");
  EXPECT_THAT(registry.fill,
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0, 0, 0xFF))))));

  registry.parseStyle("fill: red asdf");
  EXPECT_THAT(registry.fill, Eq(std::nullopt));

  registry.parseStyle("fill: asdf");
  EXPECT_THAT(registry.fill, Eq(std::nullopt));

  registry.parseStyle("fill: context-fill");
  EXPECT_THAT(registry.fill, Optional(PaintServer(PaintServer::ContextFill())));
  registry.parseStyle("fill: \t context-stroke");
  EXPECT_THAT(registry.fill, Optional(PaintServer(PaintServer::ContextStroke())));
  registry.parseStyle("fill: context-stroke invalid");
  EXPECT_THAT(registry.fill, Eq(std::nullopt));

  registry.parseStyle("fill: url(#test)");
  EXPECT_THAT(registry.fill, Optional(PaintServer(PaintServer::Reference("#test"))));

  registry.parseStyle("fill: url(#test) none");
  EXPECT_THAT(registry.fill, Optional(PaintServer(PaintServer::Reference("#test"))));

  registry.parseStyle("fill: url(#test) green");
  EXPECT_THAT(registry.fill,
              Optional(PaintServer(PaintServer::Reference("#test", Color(RGBA(0, 128, 0, 0xFF))))));

  registry.parseStyle("fill: url(#test)   lime\t  ");
  EXPECT_THAT(
      registry.fill,
      Optional(PaintServer(PaintServer::Reference("#test", Color(RGBA(0, 0xFF, 0, 0xFF))))));
}

}  // namespace donner::svg