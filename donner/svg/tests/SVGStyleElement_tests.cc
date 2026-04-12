#include "donner/svg/SVGStyleElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/StylesheetComponent.h"

namespace donner::svg {
namespace {

TEST(SVGStyleElementTests, DefaultsToCssType) {
  SVGDocument document;
  SVGStyleElement style = SVGStyleElement::Create(document);

  EXPECT_TRUE(style.isCssType());
}

TEST(SVGStyleElementTests, SetTypeControlsCssParsing) {
  SVGDocument document;
  SVGStyleElement style = SVGStyleElement::Create(document);

  style.setType("text/plain");
  EXPECT_FALSE(style.isCssType());

  style.setType("text/css");
  EXPECT_TRUE(style.isCssType());
}

TEST(SVGStyleElementTests, SetContentsOnlyParsesCssType) {
  SVGDocument document;
  SVGStyleElement style = SVGStyleElement::Create(document);

  style.setType("text/plain");
  style.setContents("rect { fill: red; }");
  auto& component = style.entityHandle().get<components::StylesheetComponent>();
  EXPECT_TRUE(component.stylesheet.rules().empty());

  style.setType("text/css");
  style.setContents("rect { fill: red; }");
  EXPECT_THAT(component.stylesheet.rules(), testing::SizeIs(1));
}

}  // namespace
}  // namespace donner::svg
