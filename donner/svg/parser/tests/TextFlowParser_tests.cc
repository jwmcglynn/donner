#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/text/TextFlowComponent.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg::parser {

namespace {

SVGParser::Options ExperimentalTextOptions() {
  SVGParser::Options options;
  options.enableExperimental = true;
  return options;
}

SVGDocument ParseWithExperimentalText(std::string_view input) {
  auto maybeDoc = SVGParser::ParseSVG(input, nullptr, ExperimentalTextOptions());
  EXPECT_THAT(maybeDoc, NoParseError());
  return std::move(maybeDoc).result();
}

}  // namespace

TEST(TextFlowParserTests, ParsesFlowRegionsAndAlignment) {
  auto document = ParseWithExperimentalText(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="flow" flow-align="center" flow-overflow="hidden">
        <flowRegion x="10px" y="5px" width="200px" height="40px" flow-overflow="scroll" />
        <flowRegion width="150" height="25" />
        Wrapped text
      </text>
    </svg>
  )");

  const EntityHandle handle = document.querySelector("#flow")->entityHandle();
  const auto* flow = document.registry().try_get<components::TextFlowComponent>(handle.entity());
  ASSERT_NE(flow, nullptr);

  ASSERT_EQ(flow->regions.size(), 2u);
  EXPECT_THAT(flow->regions[0].x, LengthIs(10.0, Lengthd::Unit::Px));
  EXPECT_THAT(flow->regions[0].y, LengthIs(5.0, Lengthd::Unit::Px));
  EXPECT_THAT(flow->regions[0].width, LengthIs(200.0, Lengthd::Unit::Px));
  EXPECT_THAT(flow->regions[0].height, LengthIs(40.0, Lengthd::Unit::Px));
  EXPECT_EQ(flow->regions[0].overflow, Overflow::Scroll);

  EXPECT_THAT(flow->regions[1].x, LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(flow->regions[1].y, LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(flow->regions[1].width, LengthIs(150.0, Lengthd::Unit::None));
  EXPECT_THAT(flow->regions[1].height, LengthIs(25.0, Lengthd::Unit::None));
  EXPECT_EQ(flow->regions[1].overflow, Overflow::Visible);

  ASSERT_TRUE(flow->alignment.has_value());
  EXPECT_EQ(flow->alignment.value(), components::FlowAlignment::Center);
  ASSERT_TRUE(flow->overflow.has_value());
  EXPECT_EQ(flow->overflow.value(), Overflow::Hidden);
}

TEST(TextFlowParserTests, EmitsWarningForMissingRegionSize) {
  std::vector<ParseError> warnings;
  auto maybeDoc = SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text enable-background="new">
        <flowRegion x="1" y="2" />
      </text>
    </svg>
  )",
                                      &warnings, ExperimentalTextOptions());

  EXPECT_THAT(maybeDoc, NoParseError());
  EXPECT_FALSE(warnings.empty());
}

}  // namespace donner::svg::parser
