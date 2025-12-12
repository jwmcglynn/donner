#include "donner/css/parser/ColorProfileParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/parser/ColorParser.h"

using testing::Optional;

namespace donner::css::parser {

TEST(ColorProfileParser, ParsesNamedProfileSrc) {
  ColorProfileRegistry registry =
      ColorProfileParser::ParseStylesheet("@color-profile --brand { src: display-p3; }");

  EXPECT_EQ(registry.size(), 1u);
  EXPECT_THAT(registry.resolve("--brand"), Optional(ColorSpaceId::kDisplayP3));
}

TEST(ColorProfileParser, ParsesColorFunctionSrc) {
  ColorProfileRegistry registry =
      ColorProfileParser::ParseStylesheet("@color-profile --hdr { src: color(rec2020 0 0 0); }");

  EXPECT_THAT(registry.resolve("--hdr"), Optional(ColorSpaceId::kRec2020));
}

TEST(ColorProfileParser, ColorParserUsesCustomProfiles) {
  ColorProfileRegistry registry;
  registry.registerProfile("--accent", ColorSpaceId::kDisplayP3);

  ColorParser::Options options{.profileRegistry = &registry};

  ColorSpaceValue expected;
  expected.id = ColorSpaceId::kDisplayP3;
  expected.c1 = 1.0;
  expected.c2 = 0.5;
  expected.c3 = 0.0;

  EXPECT_THAT(ColorParser::ParseString("color(--accent 1 0.5 0)", options),
              ParseResultIs(Color(expected)));
}

TEST(ColorProfileParser, UnknownProfileStillErrors) {
  ColorProfileRegistry registry;
  ColorParser::Options options{.profileRegistry = &registry};

  EXPECT_THAT(ColorParser::ParseString("color(--missing 1 0 0)", options),
              ParseErrorIs("Unsupported color space '--missing'"));
}

}  // namespace donner::css::parser
