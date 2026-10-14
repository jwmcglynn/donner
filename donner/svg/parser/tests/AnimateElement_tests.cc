#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <sstream>
#include <string>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationPathValue.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/parser/ListParser.h"
#include "donner/svg/parser/PathParser.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/renderer/RendererUtils.h"

namespace donner::svg {

namespace {

SVGDocument parseSVGWithExperimental(std::string_view svg) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  ParseWarningSink warnings;
  auto result = parser::SVGParser::ParseSVG(svg, warnings, options);
  EXPECT_THAT(result, NoParseError()) << "Failed to parse SVG";
  return std::move(result.result());
}

std::string repeatedAnimationValues(std::size_t count) {
  std::string values;
  values.reserve(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    values += "1;";
  }
  return values;
}

/// Helper to run the animation system and return the override value for a given attribute on an
/// element.
std::optional<std::string> getAnimatedValue(SVGDocument& document, const char* selector,
                                            const std::string& attributeName, double time) {
  auto& registry = document.registry();
  components::AnimationSystem().advance(registry, time, nullptr);

  auto element = document.querySelector(selector);
  if (!element.has_value()) {
    return std::nullopt;
  }

  auto* animValues =
      registry.try_get<components::AnimatedValuesComponent>(element->entityHandle().entity());
  if (!animValues) {
    return std::nullopt;
  }

  if (attributeName == "fill" && animValues->contextualLengths.contains("fill")) {
    // Paint stacks are resolved only after the final inheritance and color context is known.
    document.setTime(time);
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    RendererUtils::prepareDocumentForRendering(document, false, warnings);
    const auto paint = element->getComputedStyle().fill.get();
    if (!paint || !paint->is<PaintServer::Solid>()) {
      return std::nullopt;
    }
    return paint->get<PaintServer::Solid>().color.resolve(css::RGBA::RGB(0, 0, 0), 1).toHexString();
  }
  auto it = animValues->overrides.find(attributeName);
  if (it == animValues->overrides.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace

// --- Parsing tests ---

TEST(SVGAnimateElement, ParseFromTo) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="100" to="200" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateValueComponent>();
  ASSERT_FALSE(view.begin() == view.end());

  auto entity = *view.begin();
  auto& valueComp = registry.get<components::AnimateValueComponent>(entity);
  EXPECT_EQ(valueComp.attributeName, "width");
  ASSERT_TRUE(valueComp.from.has_value());
  EXPECT_EQ(valueComp.from.value(), "100");
  ASSERT_TRUE(valueComp.to.has_value());
  EXPECT_EQ(valueComp.to.value(), "200");
  EXPECT_EQ(valueComp.calcMode, components::CalcMode::Linear);
}

TEST(SVGAnimateElement, ParseValues) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" values="10;50;100" begin="0s" dur="3s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateValueComponent>();
  auto entity = *view.begin();
  auto& valueComp = registry.get<components::AnimateValueComponent>(entity);

  ASSERT_EQ(valueComp.values.size(), 3u);
  EXPECT_EQ(valueComp.values[0], "10");
  EXPECT_EQ(valueComp.values[1], "50");
  EXPECT_EQ(valueComp.values[2], "100");
}

TEST(SVGAnimateElement, ValuesListCapRejectsOverflowTransactionally) {
  const auto parsedValueCount = [](std::size_t count) {
    const std::string svg =
        "<svg xmlns='http://www.w3.org/2000/svg'><rect><animate attributeName='width' values='" +
        repeatedAnimationValues(count) + "'/></rect></svg>";
    auto document = parseSVGWithExperimental(svg);
    auto view = document.registry().view<components::AnimateValueComponent>();
    EXPECT_FALSE(view.begin() == view.end());
    return document.registry().get<components::AnimateValueComponent>(*view.begin()).values.size();
  };

  EXPECT_EQ(parsedValueCount(parser::ListParser::kMaximumItems), parser::ListParser::kMaximumItems);
  EXPECT_EQ(parsedValueCount(parser::ListParser::kMaximumItems + 1), 0u);
}

TEST(SVGAnimateElement, TimingListOverflowReportsTheRejectedItem) {
  std::string timings;
  timings.reserve((parser::ListParser::kMaximumItems + 1) * 3);
  for (std::size_t i = 0; i <= parser::ListParser::kMaximumItems; ++i) {
    timings += "1s;";
  }
  const std::string svg =
      "<svg xmlns='http://www.w3.org/2000/svg'><rect><animate attributeName='width' begin='" +
      timings + "'/></rect></svg>";

  parser::SVGParser::Options options;
  options.enableExperimental = true;
  ParseWarningSink warnings;
  auto result = parser::SVGParser::ParseSVG(svg, warnings, options);
  ASSERT_THAT(result, NoParseError());
  EXPECT_THAT(warnings.warnings(), testing::Contains(testing::Field(
                                       &ParseDiagnostic::reason,
                                       testing::HasSubstr("Animation list item limit exceeded"))));
}

TEST(SVGAnimateElement, NumericKeyTimesOverflowReportsTheRejectedNumber) {
  std::string keyTimes;
  keyTimes.reserve((parser::ListParser::kMaximumItems + 1) * 2);
  for (std::size_t i = 0; i <= parser::ListParser::kMaximumItems; ++i) {
    keyTimes += "0 ";
  }
  const std::string prefix =
      "<svg xmlns='http://www.w3.org/2000/svg'><rect><animate attributeName='width' keyTimes='";
  const std::string svg = prefix + keyTimes + "'/></rect></svg>";

  parser::SVGParser::Options options;
  options.enableExperimental = true;
  ParseWarningSink warnings;
  auto result = parser::SVGParser::ParseSVG(svg, warnings, options);
  ASSERT_THAT(result, NoParseError());
  const auto warning = std::ranges::find_if(warnings.warnings(), [](const ParseDiagnostic& item) {
    return std::string_view(item.reason).find("Animation numeric list item limit exceeded") !=
           std::string_view::npos;
  });
  ASSERT_NE(warning, warnings.warnings().end());
  ASSERT_TRUE(warning->range.start.offset.has_value());
  EXPECT_GT(*warning->range.start.offset, prefix.size() + parser::ListParser::kMaximumItems);
}

TEST(SVGAnimateElement, ParseCalcModeDiscrete) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="fill" values="red;blue;green" calcMode="discrete"
                 begin="0s" dur="3s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateValueComponent>();
  auto entity = *view.begin();
  auto& valueComp = registry.get<components::AnimateValueComponent>(entity);
  EXPECT_EQ(valueComp.calcMode, components::CalcMode::Discrete);
}

TEST(SVGAnimateElement, ParseKeyTimes) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" values="10;50;100" keyTimes="0;0.3;1"
                 begin="0s" dur="3s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateValueComponent>();
  auto entity = *view.begin();
  auto& valueComp = registry.get<components::AnimateValueComponent>(entity);

  ASSERT_EQ(valueComp.keyTimes.size(), 3u);
  EXPECT_DOUBLE_EQ(valueComp.keyTimes[0], 0.0);
  EXPECT_DOUBLE_EQ(valueComp.keyTimes[1], 0.3);
  EXPECT_DOUBLE_EQ(valueComp.keyTimes[2], 1.0);
}

TEST(SVGAnimateElement, ParseBy) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="50" by="100" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateValueComponent>();
  auto entity = *view.begin();
  auto& valueComp = registry.get<components::AnimateValueComponent>(entity);

  ASSERT_TRUE(valueComp.from.has_value());
  EXPECT_EQ(valueComp.from.value(), "50");
  ASSERT_TRUE(valueComp.by.has_value());
  EXPECT_EQ(valueComp.by.value(), "100");
}

// --- Linear interpolation tests ---

TEST(SVGAnimateElement, LinearInterpolationFromTo) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="100" to="200" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=0 (start): value should be 100
  auto val0 = getAnimatedValue(document, "#r", "width", 0.0);
  ASSERT_TRUE(val0.has_value());
  EXPECT_NEAR(std::stod(val0.value()), 100.0, 0.01);

  // At t=1s (midpoint): value should be 150
  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_NEAR(std::stod(val1.value()), 150.0, 0.01);

  // At t=1.5s (75%): value should be 175
  auto val15 = getAnimatedValue(document, "#r", "width", 1.5);
  ASSERT_TRUE(val15.has_value());
  EXPECT_NEAR(std::stod(val15.value()), 175.0, 0.01);
}

TEST(SVGAnimateElement, LinearInterpolationValues) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" values="0;100;50" begin="0s" dur="4s" />
      </rect>
    </svg>
  )");

  // 3 values = 2 intervals, each 2s
  // At t=0: 0
  auto val0 = getAnimatedValue(document, "#r", "width", 0.0);
  ASSERT_TRUE(val0.has_value());
  EXPECT_NEAR(std::stod(val0.value()), 0.0, 0.01);

  // At t=1s (midpoint of first interval): 50
  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_NEAR(std::stod(val1.value()), 50.0, 0.01);

  // At t=2s (start of second interval): 100
  auto val2 = getAnimatedValue(document, "#r", "width", 2.0);
  ASSERT_TRUE(val2.has_value());
  EXPECT_NEAR(std::stod(val2.value()), 100.0, 0.01);

  // At t=3s (midpoint of second interval): 75
  auto val3 = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(val3.has_value());
  EXPECT_NEAR(std::stod(val3.value()), 75.0, 0.01);
}

TEST(SVGAnimateElement, DiscreteCalcMode) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="red" width="100" height="100">
        <animate attributeName="fill" values="red;blue;green" calcMode="discrete"
                 begin="0s" dur="3s" />
      </rect>
    </svg>
  )");

  // 3 values, discrete: each occupies 1/3 of the duration
  // At t=0: "red"
  auto val0 = getAnimatedValue(document, "#r", "fill", 0.0);
  ASSERT_TRUE(val0.has_value());
  EXPECT_EQ(val0.value(), "#ff0000");

  // At t=1.5s: "blue" (1/3 to 2/3)
  auto val1 = getAnimatedValue(document, "#r", "fill", 1.5);
  ASSERT_TRUE(val1.has_value());
  EXPECT_EQ(val1.value(), "#0000ff");

  // At t=2.5s: "green" (2/3 to 1)
  auto val2 = getAnimatedValue(document, "#r", "fill", 2.5);
  ASSERT_TRUE(val2.has_value());
  EXPECT_EQ(val2.value(), "#008000");
}

TEST(SVGAnimateElement, FromByInterpolation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="50" by="100" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // from/by: result = from + by * progress
  // At t=0: 50
  auto val0 = getAnimatedValue(document, "#r", "width", 0.0);
  ASSERT_TRUE(val0.has_value());
  EXPECT_NEAR(std::stod(val0.value()), 50.0, 0.01);

  // At t=1s (50%): 50 + 100*0.5 = 100
  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_NEAR(std::stod(val1.value()), 100.0, 0.01);

  // At t=1.9s (95%): 50 + 100*0.95 = 145
  auto val2 = getAnimatedValue(document, "#r", "width", 1.9);
  ASSERT_TRUE(val2.has_value());
  EXPECT_NEAR(std::stod(val2.value()), 145.0, 0.01);
}

TEST(SVGAnimateElement, BeforeBeginNoValue) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="100" to="200" begin="2s" dur="2s" />
      </rect>
    </svg>
  )");

  // Before begin time: no override
  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  EXPECT_FALSE(val.has_value());
}

TEST(SVGAnimateElement, AfterEndRemove) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="100" to="200" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // After end with fill="remove" (default): no override
  auto val = getAnimatedValue(document, "#r", "width", 3.0);
  EXPECT_FALSE(val.has_value());
}

TEST(SVGAnimateElement, AfterEndFreeze) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="100" to="200" begin="0s" dur="2s" fill="freeze" />
      </rect>
    </svg>
  )");

  // After end with fill="freeze": frozen at final value (200)
  auto val = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_NEAR(std::stod(val.value()), 200.0, 0.01);
}

TEST(SVGAnimateElement, RepeatCount) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="2s"
                 repeatCount="3" fill="freeze" />
      </rect>
    </svg>
  )");

  // repeatCount=3, dur=2s → active duration = 6s
  // At t=3s: second iteration, simpleTime = 3-2=1, progress = 1/2 = 0.5, value = 50
  auto val3 = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(val3.has_value());
  EXPECT_NEAR(std::stod(val3.value()), 50.0, 0.01);

  // At t=5s: third iteration, simpleTime = 5-4=1, progress = 0.5, value = 50
  auto val5 = getAnimatedValue(document, "#r", "width", 5.0);
  ASSERT_TRUE(val5.has_value());
  EXPECT_NEAR(std::stod(val5.value()), 50.0, 0.01);

  // At t=7s: frozen at final value (100)
  auto val7 = getAnimatedValue(document, "#r", "width", 7.0);
  ASSERT_TRUE(val7.has_value());
  EXPECT_NEAR(std::stod(val7.value()), 100.0, 0.01);
}

TEST(SVGAnimateElement, OmittedDurationHoldsInitialValue) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="100" to="200" begin="0s" />
      </rect>
    </svg>
  )");

  // An indefinite simple duration holds the first value.
  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  EXPECT_THAT(val, testing::Optional(testing::StrEq("100")));
}

TEST(SVGAnimateElement, KeyTimesInterpolation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" values="0;100;50" keyTimes="0;0.25;1"
                 begin="0s" dur="4s" />
      </rect>
    </svg>
  )");

  // keyTimes=[0, 0.25, 1] with values=[0, 100, 50]
  // First interval: progress 0→0.25 maps to values 0→100
  // At t=0.5s: progress=0.125, in first interval (0→0.25), localT=0.125/0.25=0.5 → value=50
  auto val05 = getAnimatedValue(document, "#r", "width", 0.5);
  ASSERT_TRUE(val05.has_value());
  EXPECT_NEAR(std::stod(val05.value()), 50.0, 0.01);

  // At t=1s: progress=0.25, exact boundary → value=100
  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_NEAR(std::stod(val1.value()), 100.0, 0.5);

  // Second interval: progress 0.25→1 maps to values 100→50
  // At t=2.5s: progress=0.625, in second interval, localT=(0.625-0.25)/(1-0.25)=0.5 → value=75
  auto val25 = getAnimatedValue(document, "#r", "width", 2.5);
  ASSERT_TRUE(val25.has_value());
  EXPECT_NEAR(std::stod(val25.value()), 75.0, 0.01);
}

TEST(SVGAnimateElement, HrefTarget) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">
      <rect id="r" width="100" height="100" />
      <animate xlink:href="#r" attributeName="width" from="100" to="200" begin="0s" dur="2s" />
    </svg>
  )");

  // animate targets #r via href
  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_NEAR(std::stod(val.value()), 150.0, 0.01);
}

// --- Animation sandwich model tests (replace mode; additive/accumulate deferred) ---

TEST(SVGAnimateElement, MultipleAnimationsLastWins) {
  // Two replace-mode animations on the same attribute: last one wins.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="50" begin="0s" dur="2s" />
        <animate attributeName="width" from="0" to="200" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=1s (50%): second animation replaces first → 100
  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_NEAR(std::stod(val.value()), 100.0, 0.01);
}

TEST(SVGAnimateElement, FrozenAnimationInSandwich) {
  // First animation freezes, second replaces during its active interval.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="50" begin="0s" dur="1s" fill="freeze" />
        <animate attributeName="width" from="0" to="200" begin="2s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=1.5s: first is frozen at 50, second hasn't started → 50
  auto val15 = getAnimatedValue(document, "#r", "width", 1.5);
  ASSERT_TRUE(val15.has_value());
  EXPECT_NEAR(std::stod(val15.value()), 50.0, 0.01);

  // At t=3s: first is frozen at 50, second is active (50% → 100) and replaces → 100
  auto val3 = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(val3.has_value());
  EXPECT_NEAR(std::stod(val3.value()), 100.0, 0.01);
}

// --- Edge case tests ---

TEST(SVGAnimateElement, NegativeBeginOffset) {
  // Animation already started before document time 0.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="-1s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=0: animation started at -1s, 1s into 2s duration = 50%
  auto val0 = getAnimatedValue(document, "#r", "width", 0.0);
  ASSERT_TRUE(val0.has_value());
  EXPECT_NEAR(std::stod(val0.value()), 50.0, 0.5);

  // At t=0.5: 1.5s into animation = 75%
  auto val05 = getAnimatedValue(document, "#r", "width", 0.5);
  ASSERT_TRUE(val05.has_value());
  EXPECT_NEAR(std::stod(val05.value()), 75.0, 0.5);
}

TEST(SVGAnimateElement, NegativeBeginAlreadyFinished) {
  // Animation finished before document time 0.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="-5s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=0: animation ended at -3s, fill=remove → no override
  auto val0 = getAnimatedValue(document, "#r", "width", 0.0);
  EXPECT_FALSE(val0.has_value());
}

TEST(SVGAnimateElement, NegativeBeginFrozen) {
  // Animation finished before t=0 but fill=freeze → frozen at final value.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="-5s" dur="2s" fill="freeze" />
      </rect>
    </svg>
  )");

  auto val0 = getAnimatedValue(document, "#r", "width", 0.0);
  ASSERT_TRUE(val0.has_value());
  EXPECT_NEAR(std::stod(val0.value()), 100.0, 0.5);
}

TEST(SVGAnimateElement, MultipleBeginValues) {
  // begin="1s;5s" should start at the earliest qualifying time (1s).
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="3s;1s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=0.5: before 1s → no value
  auto val05 = getAnimatedValue(document, "#r", "width", 0.5);
  EXPECT_FALSE(val05.has_value());

  // At t=1.5: 0.5s into animation (started at 1s) = 25% → 25
  auto val15 = getAnimatedValue(document, "#r", "width", 1.5);
  ASSERT_TRUE(val15.has_value());
  EXPECT_NEAR(std::stod(val15.value()), 25.0, 0.5);
}

TEST(SVGAnimateElement, FreezeSamplesTruncatedActiveEnd) {
  // end="2s" truncates the 4s simple duration to a 2s active duration. fill="freeze" must hold
  // the value sampled at the truncated active end (t=2s, 50% progress -> 50), not the
  // simple-duration end value (100).
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="4s" end="2s"
                 fill="freeze" />
      </rect>
    </svg>
  )");

  auto val3 = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(val3.has_value());
  EXPECT_NEAR(std::stod(val3.value()), 50.0, 0.5);

  auto val10 = getAnimatedValue(document, "#r", "width", 10.0);
  ASSERT_TRUE(val10.has_value());
  EXPECT_NEAR(std::stod(val10.value()), 50.0, 0.5);
}

TEST(SVGAnimateElement, FreezeSamplesRepeatDurTruncatedMidIteration) {
  // repeatDur="6s" ends the animation 2s into the second 4s iteration, so the frozen value is
  // the value at 50% of the simple duration (50), not the iteration end value (100).
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="4s" repeatCount="2"
                 repeatDur="6s" fill="freeze" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedValue(document, "#r", "width", 7.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_NEAR(std::stod(val.value()), 50.0, 0.5);
}

TEST(SVGAnimateElement, FreezeSamplesMaxTruncatedActiveEnd) {
  // max="2s" caps the 4s simple duration to a 2s active duration. fill="freeze" must hold the
  // value sampled at the truncated active end (t=2s, 50% progress -> 50), not the
  // simple-duration end value (100).
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="4s" max="2s"
                 fill="freeze" />
      </rect>
    </svg>
  )");

  auto val3 = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(val3.has_value());
  EXPECT_NEAR(std::stod(val3.value()), 50.0, 0.5);

  auto val10 = getAnimatedValue(document, "#r", "width", 10.0);
  ASSERT_TRUE(val10.has_value());
  EXPECT_NEAR(std::stod(val10.value()), 50.0, 0.5);
}

TEST(SVGAnimateElement, SandwichOrderSetAfterAnimate) {
  // Replace-mode sandwich priority is document order across all animation element types: a later
  // <set> must win over an earlier <animate> targeting the same attribute.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="10" to="10" begin="0s" dur="4s" />
        <set attributeName="width" to="99" begin="0s" dur="4s" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "99");
}

TEST(SVGAnimateElement, SandwichOrderAnimateAfterSet) {
  // The reverse document order: a later <animate> must win over an earlier <set>.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <set attributeName="width" to="99" begin="0s" dur="4s" />
        <animate attributeName="width" from="10" to="10" begin="0s" dur="4s" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "10");
}

TEST(SVGAnimateElement, BeginIndefiniteNeverStarts) {
  // begin="indefinite" means the begin time is present but unresolved: the animation never
  // starts. This is distinct from an absent begin attribute, which defaults to an offset of 0.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="indefinite" dur="2s" />
      </rect>
    </svg>
  )");

  EXPECT_FALSE(getAnimatedValue(document, "#r", "width", 0.0).has_value());
  EXPECT_FALSE(getAnimatedValue(document, "#r", "width", 1.0).has_value());
  EXPECT_FALSE(getAnimatedValue(document, "#r", "width", 100.0).has_value());
}

TEST(SVGAnimateElement, BeginListWithNoResolvableOffsetNeverStarts) {
  // A begin list containing only unsupported (syncbase/event) values has no resolvable offset,
  // so the animation never starts rather than starting at t=0.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="other.end; r.click" dur="2s" />
      </rect>
    </svg>
  )");

  EXPECT_FALSE(getAnimatedValue(document, "#r", "width", 0.0).has_value());
  EXPECT_FALSE(getAnimatedValue(document, "#r", "width", 1.0).has_value());
  EXPECT_FALSE(getAnimatedValue(document, "#r", "width", 100.0).has_value());
}

TEST(SVGAnimateElement, ZeroDurationNoEffect) {
  // Explicit dur="0s" produces no animation effect.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="0s" />
      </rect>
    </svg>
  )");

  auto val0 = getAnimatedValue(document, "#r", "width", 0.0);
  EXPECT_FALSE(val0.has_value());
}

TEST(SVGAnimateElement, MinConstraint) {
  // min="3s" forces active duration to be at least 3s even though dur is 1s.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="1s" min="3s" />
      </rect>
    </svg>
  )");

  // The interval remains active, but fill=remove stops contributing after the simple duration.
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 2.0), testing::Eq(std::nullopt));
}

TEST(SVGAnimateElement, MaxConstraint) {
  // max="0.5s" limits active duration even though dur is 2s.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="2s" max="0.5s" />
      </rect>
    </svg>
  )");

  // At t=1s: past max=0.5s → After phase, fill=remove → no value
  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  EXPECT_FALSE(val1.has_value());

  // At t=0.25: within active duration (25% of 2s dur → 12.5)
  auto val025 = getAnimatedValue(document, "#r", "width", 0.25);
  ASSERT_TRUE(val025.has_value());
  EXPECT_NEAR(std::stod(val025.value()), 12.5, 0.5);
}

// --- Path d animation tests ---

TEST(SVGAnimateElement, PathDAnimation) {
  // Animate the 'd' attribute of a path between two compatible shapes.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <path id="p" d="M0,0 L100,0 L100,100 Z">
        <animate attributeName="d"
                 from="M0,0 L100,0 L100,100 Z"
                 to="M0,0 L200,0 L200,200 Z"
                 begin="0s" dur="2s" />
      </path>
    </svg>
  )");

  // At t=0: from value → M0,0 L100,0 L100,100 Z
  auto val0 = getAnimatedValue(document, "#p", "d", 0.0);
  ASSERT_TRUE(val0.has_value());
  // Should contain "M0" and "L100" approximately
  EXPECT_NE(val0.value().find('M'), std::string::npos);

  // At t=1s (50%): midpoint → L150,0 L150,150
  auto val1 = getAnimatedValue(document, "#p", "d", 1.0);
  ASSERT_TRUE(val1.has_value());
  // The interpolated L should be at (150, 0) and (150, 150).
  // Parse the path to verify.
  EXPECT_NE(val1.value().find('L'), std::string::npos);
  EXPECT_NE(val1.value().find("150"), std::string::npos);

  // At t=1.9s (95%): close to end → L195,0 L195,195 approx
  auto val19 = getAnimatedValue(document, "#p", "d", 1.9);
  ASSERT_TRUE(val19.has_value());
  EXPECT_NE(val19.value().find("19"), std::string::npos);  // 195 or 190
}

TEST(SVGAnimateElement, PathDIncompatibleFallsBackToDiscrete) {
  // Incompatible paths (different command counts) should use discrete interpolation.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <path id="p" d="M0,0 L100,0">
        <animate attributeName="d"
                 from="M0,0 L100,0"
                 to="M0,0 L100,0 L100,100 Z"
                 begin="0s" dur="2s" />
      </path>
    </svg>
  )");

  // At t=0.5 (25%): discrete should pick the first value (< 50%)
  auto val05 = getAnimatedValue(document, "#p", "d", 0.5);
  ASSERT_TRUE(val05.has_value());
  // Discrete falls back: localT < 0.5 returns 'from'
  // The from value doesn't have "Z"
  EXPECT_EQ(val05.value().find('Z'), std::string::npos);

  // At t=1.5 (75%): discrete should pick the second value (> 50%)
  auto val15 = getAnimatedValue(document, "#p", "d", 1.5);
  ASSERT_TRUE(val15.has_value());
  // The to value has "Z"
  EXPECT_NE(val15.value().find('Z'), std::string::npos);
}

TEST(SVGAnimateElement, PathDValuesAnimation) {
  // Path animation via values list with 3 keyframes.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <path id="p" d="M0,0 L100,0 L100,100 Z">
        <animate attributeName="d"
                 values="M0,0 L100,0 L100,100 Z;
                         M0,0 L200,0 L200,200 Z;
                         M0,0 L300,0 L300,300 Z"
                 begin="0s" dur="4s" />
      </path>
    </svg>
  )");

  // At t=1s (25%): halfway through first interval → L150,0 L150,150
  auto val1 = getAnimatedValue(document, "#p", "d", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_NE(val1.value().find("150"), std::string::npos);

  // At t=3s (75%): halfway through second interval → L250,0 L250,250
  auto val3 = getAnimatedValue(document, "#p", "d", 3.0);
  ASSERT_TRUE(val3.has_value());
  EXPECT_NE(val3.value().find("250"), std::string::npos);
}

// --- restart attribute enforcement ---

TEST(SVGAnimateElement, RestartNever) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="100" to="200" begin="0s" dur="1s"
                 restart="never" fill="freeze" />
      </rect>
    </svg>
  )");

  // During active: at t=0.5, should animate.
  auto val05 = getAnimatedValue(document, "#r", "width", 0.5);
  ASSERT_TRUE(val05.has_value());
  EXPECT_NEAR(std::stod(val05.value()), 150.0, 1.0);

  // After completion: frozen at final value.
  auto val2 = getAnimatedValue(document, "#r", "width", 2.0);
  ASSERT_TRUE(val2.has_value());
  EXPECT_NEAR(std::stod(val2.value()), 200.0, 1.0);
}

TEST(SVGAnimateElement, RestartWhenNotActive) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="100" to="200" begin="0s" dur="2s"
                 restart="whenNotActive" />
      </rect>
    </svg>
  )");

  // During active: should animate.
  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_NEAR(std::stod(val.value()), 150.0, 1.0);
}

// --- Number-list interpolation ---

TEST(SVGAnimateElement, NumberListInterpolation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="stroke-dasharray" from="10 20 30" to="40 50 60"
                 begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=1s (50%): should be "25 35 45".
  auto val = getAnimatedValue(document, "#r", "stroke-dasharray", 1.0);
  ASSERT_TRUE(val.has_value());
  // Parse the space-separated result.
  double a = 0, b = 0, c = 0;
  std::istringstream iss(val.value());
  iss >> a >> b >> c;
  EXPECT_NEAR(a, 25.0, 1.0);
  EXPECT_NEAR(b, 35.0, 1.0);
  EXPECT_NEAR(c, 45.0, 1.0);
}

TEST(SVGAnimateElement, WhitespaceAroundNumbersIsTrimmed) {
  // Numeric interpolation must tolerate surrounding whitespace in from/to values.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from=" 100 " to=" 200 " begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_NEAR(std::stod(val.value()), 150.0, 0.01);
}

TEST(SVGAnimateElement, RestartAlwaysAllowsRerunWhenTimeMovesBackward) {
  // Baseline for the restart="never" test below: with the default restart behavior, sampling an
  // earlier time after the animation has completed reactivates it.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="1s" />
      </rect>
    </svg>
  )");

  auto valActive = getAnimatedValue(document, "#r", "width", 0.5);
  ASSERT_TRUE(valActive.has_value());
  EXPECT_NEAR(std::stod(valActive.value()), 50.0, 0.01);

  // Past the active duration with fill="remove": no override.
  EXPECT_FALSE(getAnimatedValue(document, "#r", "width", 2.0).has_value());

  // Sampling backwards restarts the animation.
  auto valAgain = getAnimatedValue(document, "#r", "width", 0.5);
  ASSERT_TRUE(valAgain.has_value());
  EXPECT_NEAR(std::stod(valAgain.value()), 50.0, 0.01);
}

TEST(SVGAnimateElement, RestartNeverAllowsSeekingWithinTheOriginalInterval) {
  // Restart policy filters begin instances; seeking reconstructs the original interval.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="1s" restart="never" />
      </rect>
    </svg>
  )");

  auto valActive = getAnimatedValue(document, "#r", "width", 0.5);
  ASSERT_TRUE(valActive.has_value());
  EXPECT_NEAR(std::stod(valActive.value()), 50.0, 0.01);

  // Completes; fill="remove" clears the override.
  EXPECT_FALSE(getAnimatedValue(document, "#r", "width", 2.0).has_value());

  // Seeking samples the existing interval; it does not create a new begin instance.
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 0.5),
              testing::Optional(testing::StrEq("50")));
}

TEST(SVGAnimateElement, PathQuadCurveCloseInterpolation) {
  // Structurally compatible paths with Q, C, and Z verbs interpolate pointwise.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <path id="p" d="M0,0 L10,0 Q20,0 30,0 C40,0 50,0 60,0 Z">
        <animate attributeName="d"
                 from="M0,0 L10,0 Q20,0 30,0 C40,0 50,0 60,0 Z"
                 to="M10,10 L20,10 Q30,10 40,10 C50,10 60,10 70,10 Z"
                 begin="0s" dur="2s" />
      </path>
    </svg>
  )");

  auto val = getAnimatedValue(document, "#p", "d", 1.0);
  ASSERT_TRUE(val.has_value());
  const auto valPath = parser::PathParser::Parse(val.value());
  const auto valExpected = parser::PathParser::Parse("M5,5 L15,5 Q25,5 35,5 C45,5 55,5 65,5 Z");
  ASSERT_THAT(valPath, NoParseError());
  ASSERT_THAT(valExpected, NoParseError());
  EXPECT_EQ(valPath.result(), valExpected.result());
}

TEST(SVGAnimateElement, PathVerbMismatchFallsBackToDiscrete) {
  // Same command count but different verbs (L vs Q): not interpolable, so discrete fallback.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <path id="p" d="M0,0 L10,10">
        <animate attributeName="d" from="M0,0 L10,10" to="M0,0 Q5,5 10,10"
                 begin="0s" dur="2s" />
      </path>
    </svg>
  )");

  auto val05 = getAnimatedValue(document, "#p", "d", 0.5);
  ASSERT_TRUE(val05.has_value());
  const auto val05Path = parser::PathParser::Parse(val05.value());
  const auto val05Expected = parser::PathParser::Parse("M0,0 L10,10");
  ASSERT_THAT(val05Path, NoParseError());
  ASSERT_THAT(val05Expected, NoParseError());
  EXPECT_EQ(val05Path.result(), val05Expected.result());

  auto val15 = getAnimatedValue(document, "#p", "d", 1.5);
  ASSERT_TRUE(val15.has_value());
  const auto val15Path = parser::PathParser::Parse(val15.value());
  const auto val15Expected = parser::PathParser::Parse("M0,0 Q5,5 10,10");
  ASSERT_THAT(val15Path, NoParseError());
  ASSERT_THAT(val15Expected, NoParseError());
  EXPECT_EQ(val15Path.result(), val15Expected.result());
}

TEST(SVGAnimateElement, NamedColorsInterpolateInSrgb) {
  // Named colors interpolate in the default sRGB color space.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="red" width="100" height="100">
        <animate attributeName="fill" from="red" to="blue" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto val05 = getAnimatedValue(document, "#r", "fill", 0.5);
  ASSERT_TRUE(val05.has_value());
  EXPECT_EQ(val05.value(), "#bf0040");

  auto val15 = getAnimatedValue(document, "#r", "fill", 1.5);
  ASSERT_TRUE(val15.has_value());
  EXPECT_EQ(val15.value(), "#4000bf");
}

TEST(SVGAnimateElement, HexColorsInterpolateInSrgb) {
  // Hex colors use the same typed interpolator as named colors.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="#000000" width="100" height="100">
        <animate attributeName="fill" from="#ff0000" to="#0000ff" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto val05 = getAnimatedValue(document, "#r", "fill", 0.5);
  ASSERT_TRUE(val05.has_value());
  EXPECT_EQ(val05.value(), "#bf0040");

  auto val15 = getAnimatedValue(document, "#r", "fill", 1.5);
  ASSERT_TRUE(val15.has_value());
  EXPECT_EQ(val15.value(), "#4000bf");
}

TEST(SVGAnimateElement, ToOnlyAnimationInterpolatesUnderlyingValue) {
  // A to-only animation interpolates from the underlying value.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" to="200" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "150");

  // After the active interval with fill="remove": no override.
  EXPECT_FALSE(getAnimatedValue(document, "#r", "width", 3.0).has_value());
}

TEST(SVGAnimateElement, FromByColorAddsTheColorChannels) {
  // Color offsets add channel values before sampling.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" fill="green" width="100" height="100">
        <animate attributeName="fill" from="red" by="blue" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedValue(document, "#r", "fill", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "#ff0080");
}

TEST(SVGAnimateElement, SingleValuesEntryAppliesConstantValue) {
  // A one-entry values list applies that value for the whole active interval.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" values="42" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "42");
}

TEST(SVGAnimateElement, NoValueSpecificationProducesNoOverride) {
  // <animate> without values/from/to/by has nothing to apply.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  EXPECT_FALSE(getAnimatedValue(document, "#r", "width", 1.0).has_value());
}

TEST(SVGAnimateElement, DiscreteAtActiveEndClampsToLastValue) {
  // Frozen at 100% progress, the discrete index (numValues) clamps to the last value.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" values="10;20" calcMode="discrete" begin="0s" dur="2s"
                 fill="freeze" />
      </rect>
    </svg>
  )");

  auto val = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), "20");
}

TEST(SVGAnimateElement, MissingAttributeNameProducesNoOverride) {
  // <animate> without attributeName cannot target an attribute.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate from="0" to="100" begin="0s" dur="2s" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  components::AnimationSystem().advance(registry, 1.0, nullptr);

  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  EXPECT_EQ(registry.try_get<components::AnimatedValuesComponent>(rect->entityHandle().entity()),
            nullptr);
}

TEST(SVGAnimateElement, HrefToMissingIdProducesNoOverride) {
  // An href that resolves to no element leaves the sibling untouched and does not crash.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">
      <rect id="r" width="100" height="100" />
      <animate xlink:href="#missing" attributeName="width" from="0" to="100"
               begin="0s" dur="2s" />
    </svg>
  )");

  EXPECT_FALSE(getAnimatedValue(document, "#r", "width", 1.0).has_value());
}

TEST(SVGAnimateElement, NumberListValuesInterpolation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="stroke-dasharray" values="0 0;100 200;50 100"
                 begin="0s" dur="4s" />
      </rect>
    </svg>
  )");

  // At t=1s (25%): halfway through first interval → "50 100".
  auto val = getAnimatedValue(document, "#r", "stroke-dasharray", 1.0);
  ASSERT_TRUE(val.has_value());
  double a = 0, b = 0;
  std::istringstream iss(val.value());
  iss >> a >> b;
  EXPECT_NEAR(a, 50.0, 1.0);
  EXPECT_NEAR(b, 100.0, 1.0);
}

TEST(SVGAnimateElement, TimingListsRestartAndSeek) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns='http://www.w3.org/2000/svg'><rect id="r"><animate attributeName="width" from="0" to="100"
      begin="1s;5s" dur="2s" fill="freeze"/></rect></svg>)");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 5.5),
              testing::Optional(testing::StrEq("25")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1.5),
              testing::Optional(testing::StrEq("25")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 4.0),
              testing::Optional(testing::StrEq("100")));
}

TEST(SVGAnimateElement, EndListsChooseTheEndForEachInterval) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns='http://www.w3.org/2000/svg'><rect id="r"><animate attributeName="width" from="0" to="100"
      begin="0s;5s" end="1s;6s" dur="4s" fill="freeze"/></rect></svg>)");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 2.0),
              testing::Optional(testing::StrEq("25")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 5.5),
              testing::Optional(testing::StrEq("12.5")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 7.0),
              testing::Optional(testing::StrEq("25")));
}

TEST(SVGAnimateElement, RestartPolicyAppliesToEveryInstanceTime) {
  for (const auto& [policy, atOne, atThree] :
       {std::tuple{"always", "25", "25"}, std::tuple{"whenNotActive", "75", "25"},
        std::tuple{"never", "75", "100"}}) {
    auto document = parseSVGWithExperimental(
        std::string("<svg xmlns='http://www.w3.org/2000/svg'><rect id='r'><animate "
                    "attributeName='width' from='0' to='100' ") +
        "begin='0s;1s;3s' dur='2s' fill='freeze' restart='" + policy + "'/></rect></svg>");
    SCOPED_TRACE(policy);
    EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1.5), testing::Optional(atOne));
    EXPECT_THAT(getAnimatedValue(document, "#r", "width", 3.5), testing::Optional(atThree));
  }
}

TEST(SVGAnimateElement, ActiveRepeatBoundaryStartsNextIteration) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns='http://www.w3.org/2000/svg'><rect id="r"><animate attributeName="width" from="0" to="100"
      dur="2s" repeatCount="2" fill="freeze"/></rect></svg>)");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 2.0),
              testing::Optional(testing::StrEq("0")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 4.0),
              testing::Optional(testing::StrEq("100")));
}

TEST(SVGAnimateElement, SyncbaseResolvesForwardReferencesAndRepeatedIntervals) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns='http://www.w3.org/2000/svg'><rect id="r"><animate attributeName="width" from="0" to="100"
      begin="clock.end+0.5s" dur="2s"/></rect>
      <rect><set id="clock" attributeName="fill" to="red" begin="0s;4s" dur="1s"/></rect>
    </svg>)");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 2.0),
              testing::Optional(testing::StrEq("25")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 6.0),
              testing::Optional(testing::StrEq("25")));
}

TEST(SVGAnimateElement, RepeatEventAndOffsetStartDependentAnimation) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns='http://www.w3.org/2000/svg'><rect><set id="clock" attributeName="fill" to="red" dur="1s" repeatCount="4"/></rect>
      <rect id="r"><animate attributeName="width" from="0" to="100"
        begin="clock.repeat(2)+0.5s" dur="2s"/></rect></svg>)svg");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 2.0), testing::Eq(std::nullopt));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 3.0),
              testing::Optional(testing::StrEq("25")));
}

TEST(SVGAnimateElement, MouseEventOffsetsRetainIntervalsForSeeking) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns='http://www.w3.org/2000/svg'><rect id="r"><animate attributeName="width" from="0" to="100"
      begin="click+0.5s" dur="2s" fill="freeze"/></rect></svg>)");
  const auto target = document.querySelector("#r");
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  components::AnimationSystem system;
  EXPECT_EQ(system.dispatchMouseEvent(document.registry(), target->entityHandle().entity(),
                                      EventType::Click, 1.0),
            true);
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1.25), testing::Eq(std::nullopt));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 2.0),
              testing::Optional(testing::StrEq("25")));
  EXPECT_EQ(system.dispatchMouseEvent(document.registry(), target->entityHandle().entity(),
                                      EventType::Click, 5.0),
            true);
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 6.0),
              testing::Optional(testing::StrEq("25")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 2.0),
              testing::Optional(testing::StrEq("25")));
}

TEST(SVGAnimateElement, UnseededTimingCycleRemainsUnresolved) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate id="a" attributeName="width" from="0" to="100" begin="b.end" dur="1s"/>
      <set id="b" attributeName="fill" to="red" begin="a.end" dur="1s"/>
    </rect></svg>)");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 10.0), testing::Eq(std::nullopt));
}

TEST(SVGAnimateElement, ExpandingTimingCycleFailsClosedAtTheInstanceBudget) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate id="a" attributeName="width" from="0" to="100" begin="0s;a.end" dur="1s"/>
    </rect></svg>)");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 0.5), testing::Eq(std::nullopt));
}

TEST(SVGAnimateElement, PacedValuesUseDistanceAndIgnoreKeyTimes) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate attributeName="width" values="0;10;100" keyTimes="0;0.9;1"
        calcMode="paced" dur="2s"/>
    </rect></svg>)");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1.0),
              testing::Optional(testing::StrEq("50")));
}

TEST(SVGAnimateElement, SplineUsesBezierTimeInversion) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate attributeName="width" values="0;100" keySplines="0.42 0 1 1"
        calcMode="spline" dur="2s"/>
    </rect></svg>)");
  const auto value = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_THAT(value, testing::Ne(std::nullopt));
  EXPECT_NEAR(std::stod(*value), 31.5357, 0.001);
}

TEST(SVGAnimateElement, InvalidKeyframeTimingDisablesAnimation) {
  for (const auto* attributes :
       {"keyTimes='0;0.8;0.5'", "keyTimes='0;1'", "calcMode='spline' keySplines='2 0 1 1;0 0 1 1'",
        "calcMode='spline' keySplines='0 0 1 1'"}) {
    auto document = parseSVGWithExperimental(
        std::string("<svg xmlns='http://www.w3.org/2000/svg'><rect id='r'>") +
        "<animate attributeName='width' values='0;10;100' dur='2s' " + attributes +
        "/></rect></svg>");
    SCOPED_TRACE(attributes);
    EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1.0), testing::Eq(std::nullopt));
  }
}

TEST(SVGAnimateElement, TransformDiscreteUsesNonuniformKeyTimes) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animateTransform attributeName="transform" type="translate" values="0 0;10 0;20 0" keyTimes="0;0.25;1"
        calcMode="discrete" dur="2s"/>
    </rect></svg>)");
  EXPECT_THAT(getAnimatedValue(document, "#r", "transform", 0.25),
              testing::Optional(testing::StrEq("translate(0)")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "transform", 0.5),
              testing::Optional(testing::StrEq("translate(10)")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "transform", 1.5),
              testing::Optional(testing::StrEq("translate(10)")));
}

TEST(SVGAnimateElement, ToAndByUseUnderlyingValue) {
  for (const auto* attributes : {"to='200'", "by='100'"}) {
    auto document = parseSVGWithExperimental(
        std::string("<svg xmlns='http://www.w3.org/2000/svg'><rect id='r' width='100'>") +
        "<animate attributeName='width' dur='2s' " + attributes + "/></rect></svg>");
    EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1.0),
                testing::Optional(testing::StrEq("150")));
  }
}

TEST(SVGAnimateElement, AdditiveSandwichAndRepeatAccumulation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r" width="100">
      <animate attributeName="width" from="10" to="20" dur="2s"
        additive="sum" accumulate="sum" repeatCount="3" fill="freeze"/>
      <animate attributeName="width" from="0" to="10" dur="2s" additive="sum"/>
    </rect></svg>)");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1.0),
              testing::Optional(testing::StrEq("120")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 3.0),
              testing::Optional(testing::StrEq("135")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 6.0),
              testing::Optional(testing::StrEq("160")));
}

TEST(SVGAnimateElement, LaterActivationOverridesLaterDocumentOrder) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate attributeName="width" from="100" to="200" begin="1s" dur="2s"/>
      <animate attributeName="width" from="0" to="10" begin="0s" dur="4s"/>
    </rect></svg>)");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 2.0),
              testing::Optional(testing::StrEq("150")));
}

TEST(SVGAnimateElement, TypedColorAndUnitInterpolation) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate attributeName="fill" from="red" to="blue" dur="2s"/>
      <animate attributeName="width" from="1in" to="192px" dur="2s"/>
      <animate attributeName="height" from="10%" to="50%" dur="2s"/>
    </rect></svg>)");
  EXPECT_THAT(getAnimatedValue(document, "#r", "fill", 1.0),
              testing::Optional(testing::StrEq("#800080")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1.0),
              testing::Optional(testing::StrEq("144")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "height", 1.0),
              testing::Optional(testing::StrEq("30%")));
}

TEST(SVGAnimateElement, AdditiveTransformPreservesUnderlyingTransform) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r" transform="translate(100)">
      <animateTransform attributeName="transform" type="translate" from="0" to="20" dur="2s" additive="sum"/>
    </rect></svg>)svg");
  EXPECT_THAT(getAnimatedValue(document, "#r", "transform", 1.0),
              testing::Optional(testing::StrEq("translate(100) translate(10)")));
}

TEST(SVGAnimateElement, OneEventSchedulesDelayedBeginAndEnd) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <set attributeName="fill" to="red" begin="click+1s" end="click+2s"/>
    </rect></svg>)svg");
  const auto target = document.querySelector("#r");
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  EXPECT_EQ(document.dispatchMouseEvent(EventType::Click, *target), true);
  EXPECT_THAT(getAnimatedValue(document, "#r", "fill", 0.5), testing::Eq(std::nullopt));
  EXPECT_THAT(getAnimatedValue(document, "#r", "fill", 1.5),
              testing::Optional(testing::StrEq("#ff0000")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "fill", 2.5), testing::Eq(std::nullopt));
}

TEST(SVGAnimateElement, HostileTimingNameRemainsUnresolved) {
  const std::string source =
      "<svg xmlns='http://www.w3.org/2000/svg'><rect id='r'>"
      "<animate attributeName='width' from='0' to='100' dur='2s' begin='" +
      std::string(1024 * 1024, '-') + "'/></rect></svg>";
  auto document = parseSVGWithExperimental(source);
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1), testing::Eq(std::nullopt));
}

TEST(SVGAnimateElement, MinimumDurationExtendsAnExplicitEnd) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate attributeName="width" from="0" to="100" dur="10s" end="1s" min="3s"/>
    </rect></svg>)svg");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 2),
              testing::Optional(testing::StrEq("20")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 3), testing::Eq(std::nullopt));
}

TEST(SVGAnimateElement, MinimumDurationFreezeHoldsFinalValueAfterRepeats) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate attributeName="width" from="0" to="100" dur="1s" min="3s" fill="freeze"/>
    </rect></svg>)svg");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 2),
              testing::Optional(testing::StrEq("100")));
}

TEST(SVGAnimateElement, AuthoredAnimationEditsRefreshValuesTimingAndValidation) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate id="a" attributeName="width" values="0;100" dur="2s"/>
    </rect></svg>)svg");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1),
              testing::Optional(testing::StrEq("50")));
  auto animation = document.querySelector("#a");
  ASSERT_THAT(animation, testing::Ne(std::nullopt));
  animation->setAttribute("values", "0;200");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1),
              testing::Optional(testing::StrEq("100")));
  animation->setAttribute("keyTimes", "0;0.5;1");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1), testing::Eq(std::nullopt));
  animation->removeAttribute("keyTimes");
  animation->setAttribute("dur", "4s");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1),
              testing::Optional(testing::StrEq("50")));
}

TEST(SVGAnimateElement, SourceEditsBeforeFirstSampleAreNotLost) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate id="a" attributeName="width" from="0" to="10" dur="1s"/>
    </rect></svg>)svg");
  auto animation = document.querySelector("#a");
  ASSERT_THAT(animation, testing::Ne(std::nullopt));
  animation->setAttribute("to", "20");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 0.5),
              testing::Optional(testing::StrEq("10")));
}

TEST(SVGAnimateElement, NamespaceChangesDisableAndRestoreAnimation) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate id="a" attributeName="width" from="0" to="20" dur="2s"/>
    </rect></svg>)svg");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1),
              testing::Optional(testing::StrEq("10")));
  auto animation = document.querySelector("#a").value();
  animation.setAttribute("xmlns", "urn:foreign");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1), testing::Eq(std::nullopt));
  animation.removeAttribute("xmlns");
  EXPECT_THAT(getAnimatedValue(document, "#r", "width", 1),
              testing::Optional(testing::StrEq("10")));
}

TEST(SVGAnimateElement, MixedTextPositionUnitsUseTheirOwnViewportAxis) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100"><text id="r">
      <animate attributeName="dx" from="10px" to="50%" dur="2s"/>
      <animate attributeName="dy" from="10px" to="50%" dur="2s"/>
    </text></svg>)svg");
  EXPECT_THAT(getAnimatedValue(document, "#r", "dx", 1), testing::Optional(testing::StrEq("55")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "dy", 1), testing::Optional(testing::StrEq("30")));
}

TEST(SVGAnimateElement, ReferenceValuesAreDiscreteAndNonAdditive) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><use id="r" href="9">
      <animate attributeName="href" values="1;10" dur="2s" additive="sum" accumulate="sum" repeatCount="2"/>
    </use></svg>)svg");
  EXPECT_THAT(getAnimatedValue(document, "#r", "href", 0.5),
              testing::Optional(testing::StrEq("1")));
  EXPECT_THAT(getAnimatedValue(document, "#r", "href", 3.5),
              testing::Optional(testing::StrEq("10")));
}

TEST(SVGAnimateElement, ToOnlyReferenceUsesTheTypedBaseValue) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><use id="r" href="#a">
      <animate attributeName="href" to="#c" dur="2s"/>
    </use></svg>)svg");
  document.querySelector("#r")->cast<SVGUseElement>().setHref("#b");
  EXPECT_THAT(getAnimatedValue(document, "#r", "href", 0.25),
              testing::Optional(testing::StrEq("#b")));
}

TEST(AnimationPathValue, SharedAdmissionRefusesAndRecoversWithoutGlobalRejection) {
  using namespace components;
  auto shared = std::make_shared<AnimationContextualWorkBudget>();
  AnimationResourceBudget first;
  first.setContextualWorkBudget(shared);
  AnimateValueComponent value;
  value.attributeName = "d";
  value.from = "M1 0L2 0";
  value.to = "M3 0L4 0";
  EXPECT_THAT(SampleAnimationPath(0.5, 0, nullptr, &value, "", first),
              testing::Optional(std::string("M2 0L3 0")));
  ASSERT_THAT(shared->reserve(shared->remaining()), testing::IsTrue());
  AnimationResourceBudget refused;
  refused.setContextualWorkBudget(shared);
  EXPECT_THAT(SampleAnimationPath(0.5, 0, nullptr, &value, "", refused), testing::Eq(std::nullopt));
  EXPECT_THAT(refused.rejected(), testing::IsFalse());
  shared->reset();
  AnimationResourceBudget nextFrame;
  nextFrame.setContextualWorkBudget(shared);
  EXPECT_THAT(SampleAnimationPath(0.5, 0, nullptr, &value, "", nextFrame),
              testing::Optional(std::string("M2 0L3 0")));
}

TEST(AnimationPathValue, PathSourceLimitIsCheckedBeforePreparation) {
  using namespace components;
  SetAnimationComponent value;
  value.attributeName = "d";
  value.to = "M0 0" + std::string(AnimationResourceBudget::kMaximumPathValueBytes - 4, 'Z');
  AnimationResourceBudget accepted;
  const auto result = SampleAnimationPath(0.5, 0, &value, nullptr, "", accepted);
  ASSERT_THAT(result, testing::Ne(std::nullopt));
  EXPECT_EQ(result->size(), AnimationResourceBudget::kMaximumPathValueBytes);
  value.to->push_back('Z');
  AnimationResourceBudget refused;
  EXPECT_THAT(SampleAnimationPath(0.5, 0, &value, nullptr, "", refused), testing::Eq(std::nullopt));
  EXPECT_EQ(refused.remainingContextualWork(), AnimationResourceBudget::kMaximumContextualWork);
  EXPECT_THAT(refused.rejected(), testing::IsFalse());
}

TEST(AnimationPathValue, NonFiniteCompositionProducesNoEffect) {
  using namespace components;
  AnimateValueComponent value;
  value.attributeName = "d";
  value.from = "M0 0L1e308 0";
  value.to = "M0 0L1e308 0";
  value.additive = true;
  AnimationResourceBudget budget;
  EXPECT_THAT(SampleAnimationPath(0.5, 0, nullptr, &value, "M0 0L1e308 0", budget),
              testing::Eq(std::nullopt));
  EXPECT_THAT(budget.rejected(), testing::IsFalse());
}

TEST(AnimationPathValue, TypedGeometryKeepsPrecisionAndEmptyValues) {
  using namespace components;
  PathBuilder builder;
  builder.moveTo({0.12345678901234566, 1e100})
      .lineTo({2, 3})
      .quadTo({4, 5}, {6, 7})
      .curveTo({8, 9}, {10, 11}, {12, 13})
      .closePath();
  AnimationResourceBudget budget;
  EXPECT_THAT(
      SerializeAnimationPathGeometry(builder.build(), budget),
      testing::Optional(std::string("M0.12345678901234566 1e+100L2 3Q4 5 6 7C8 9 10 11 12 13Z")));
  AnimationResourceBudget emptyBudget;
  EXPECT_THAT(SerializeAnimationPathGeometry(Path{}, emptyBudget),
              testing::Optional(std::string{}));
}

TEST(AnimationPathValue, TypedGeometryRefusalDoesNotBecomeAnEmptyPath) {
  using namespace components;
  PathBuilder nonfinite;
  nonfinite.moveTo({std::numeric_limits<double>::infinity(), 0});
  AnimationResourceBudget invalidBudget;
  EXPECT_THAT(SerializeAnimationPathGeometry(nonfinite.build(), invalidBudget),
              testing::Eq(std::nullopt));
  PathBuilder oversized;
  oversized.moveTo({0, 0});
  for (std::size_t i = 0; i < AnimationResourceBudget::kMaximumPathValueBytes / 4; ++i) {
    oversized.lineTo({1, 1});
  }
  AnimationResourceBudget sizeBudget;
  EXPECT_THAT(SerializeAnimationPathGeometry(oversized.build(), sizeBudget),
              testing::Eq(std::nullopt));
  EXPECT_THAT(sizeBudget.rejected(), testing::IsFalse());
  AnimationResourceBudget workBudget;
  ASSERT_TRUE(workBudget.reserveContextualWork(workBudget.remainingContextualWork()));
  PathBuilder small;
  small.moveTo({0, 0});
  EXPECT_THAT(SerializeAnimationPathGeometry(small.build(), workBudget), testing::Eq(std::nullopt));
  EXPECT_THAT(workBudget.rejected(), testing::IsFalse());
}

TEST(SVGAnimateElement, ReplacementPathValuesDoNotReadAnUnusedTypedBase) {
  auto document = parseSVGWithExperimental(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><path id="p" d="M0 0L4 0">
      <animate attributeName="d" values="M0 0L4 0;M0 0L8 0" dur="2s"/>
    </path></svg>)svg");
  PathBuilder oversized;
  oversized.moveTo({0, 0});
  for (std::size_t i = 0; i < components::AnimationResourceBudget::kMaximumPathValueBytes / 4;
       ++i) {
    oversized.lineTo({1, 1});
  }
  document.querySelector("#p")->cast<SVGPathElement>().setSpline(oversized.build());
  EXPECT_THAT(getAnimatedValue(document, "#p", "d", 1), testing::Optional(std::string("M0 0L6 0")));
}

}  // namespace donner::svg
