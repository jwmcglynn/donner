#include "harness/SyntheticScenes.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::bakeoff {
namespace {

using testing::Eq;
using testing::Optional;

TEST(SyntheticScenesTest, EveryNamedSceneParsesAndIsDeterministic) {
  for (const std::string_view name : SyntheticSceneNames()) {
    SCOPED_TRACE(std::string(name));
    const std::optional<std::string> first = GenerateSyntheticScene(name);
    ASSERT_THAT(first, testing::Optional(testing::_));
    EXPECT_THAT(GenerateSyntheticScene(name), Optional(Eq(*first)));

    ParseWarningSink warnings;
    const auto parsed = svg::parser::SVGParser::ParseSVG(*first, warnings);
    EXPECT_FALSE(parsed.hasError()) << parsed.error();
  }
}

TEST(SyntheticScenesTest, UnknownNameYieldsNothing) {
  EXPECT_EQ(GenerateSyntheticScene("no_such_scene"), std::nullopt);
}

}  // namespace
}  // namespace donner::bakeoff
