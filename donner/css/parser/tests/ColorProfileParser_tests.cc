#include "donner/css/parser/ColorProfileParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <vector>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/css/parser/RuleParser.h"

using testing::Optional;

namespace donner::css::parser {

TEST(ColorProfileParser, ParsesNamedProfileSrc) {
  std::vector<Rule> rules =
      RuleParser::ParseStylesheet("@color-profile --brand { src: display-p3; }");
  ColorProfileRegistry registry;
  for (const auto& rule : rules) {
    const auto* atRule = std::get_if<AtRule>(&rule.value);
    if (atRule) {
      ColorProfileParser::ParseIntoRegistry(*atRule, registry);
    }
  }

  EXPECT_EQ(registry.size(), 1u);
  EXPECT_THAT(registry.resolve("--brand"), Optional(ColorSpaceId::DisplayP3));
}

TEST(ColorProfileParser, ParsesColorFunctionSrc) {
  std::vector<Rule> rules =
      RuleParser::ParseStylesheet("@color-profile --hdr { src: color(rec2020 0 0 0); }");
  ColorProfileRegistry registry;
  for (const auto& rule : rules) {
    const auto* atRule = std::get_if<AtRule>(&rule.value);
    if (atRule) {
      ColorProfileParser::ParseIntoRegistry(*atRule, registry);
    }
  }

  EXPECT_THAT(registry.resolve("--hdr"), Optional(ColorSpaceId::Rec2020));
}

TEST(ColorProfileParser, ColorParserUsesCustomProfiles) {
  ColorProfileRegistry registry;
  registry.registerProfile("--accent", ColorSpaceId::DisplayP3);

  ColorParser::Options options;
  options.profileRegistry = registry;

  ColorSpaceValue expected;
  expected.id = ColorSpaceId::DisplayP3;
  expected.c1 = 1.0;
  expected.c2 = 0.5;
  expected.c3 = 0.0;

  EXPECT_THAT(ColorParser::ParseString("color(--accent 1 0.5 0)", options),
              ParseResultIs(Color(expected)));
}

TEST(ColorProfileParser, UnknownProfileStillErrors) {
  ColorProfileRegistry registry;
  ColorParser::Options options;
  options.profileRegistry = registry;

  EXPECT_THAT(ColorParser::ParseString("color(--missing 1 0 0)", options),
              ParseErrorIs("Unsupported color space '--missing'"));
}

}  // namespace donner::css::parser
