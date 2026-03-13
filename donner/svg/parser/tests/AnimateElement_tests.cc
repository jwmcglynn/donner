#include <gtest/gtest.h>

#include <sstream>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

namespace {

SVGDocument parseSVGWithExperimental(std::string_view svg) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  std::vector<ParseError> warnings;
  auto result = parser::SVGParser::ParseSVG(svg, &warnings, options);
  EXPECT_THAT(result, NoParseError()) << "Failed to parse SVG";
  return std::move(result.result());
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
  EXPECT_EQ(val0.value(), "red");

  // At t=1.5s: "blue" (1/3 to 2/3)
  auto val1 = getAnimatedValue(document, "#r", "fill", 1.5);
  ASSERT_TRUE(val1.has_value());
  EXPECT_EQ(val1.value(), "blue");

  // At t=2.5s: "green" (2/3 to 1)
  auto val2 = getAnimatedValue(document, "#r", "fill", 2.5);
  ASSERT_TRUE(val2.has_value());
  EXPECT_EQ(val2.value(), "green");
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

TEST(SVGAnimateElement, NoDurNoEffect) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="100" to="200" begin="0s" />
      </rect>
    </svg>
  )");

  // <animate> without dur has simpleDuration=0 → no effect
  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  EXPECT_FALSE(val.has_value());
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

// --- Spline calcMode tests ---

TEST(SVGAnimateElement, SplineEaseInOut) {
  // keySplines "0.42 0 0.58 1" is a standard ease-in-out curve.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="2s"
                 calcMode="spline" keyTimes="0;1" keySplines="0.42 0 0.58 1" />
      </rect>
    </svg>
  )");

  // At t=0: value = 0
  auto val0 = getAnimatedValue(document, "#r", "width", 0.0);
  ASSERT_TRUE(val0.has_value());
  EXPECT_NEAR(std::stod(val0.value()), 0.0, 0.5);

  // At t=1s (50%): ease-in-out should be near 50 (symmetric curve)
  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val1.has_value());
  double mid = std::stod(val1.value());
  EXPECT_NEAR(mid, 50.0, 2.0);  // Should be close to 50 for a symmetric curve.

  // At t=0.5s (25%): ease-in-out should be less than 25 (slow start)
  auto val05 = getAnimatedValue(document, "#r", "width", 0.5);
  ASSERT_TRUE(val05.has_value());
  double quarter = std::stod(val05.value());
  EXPECT_LT(quarter, 25.0);  // Ease-in: slower at start.
  EXPECT_GT(quarter, 0.0);

  // At t=1.5s (75%): ease-in-out should be more than 75 (fast middle, slow end)
  auto val15 = getAnimatedValue(document, "#r", "width", 1.5);
  ASSERT_TRUE(val15.has_value());
  double threeQuarter = std::stod(val15.value());
  EXPECT_GT(threeQuarter, 75.0);  // Ease-out: slowing down at end.
  EXPECT_LT(threeQuarter, 100.0);
}

TEST(SVGAnimateElement, SplineMultipleIntervals) {
  // Two intervals, each with different easing.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" values="0;100;0" begin="0s" dur="4s"
                 calcMode="spline" keyTimes="0;0.5;1"
                 keySplines="0 0 1 1; 0 0 1 1" />
      </rect>
    </svg>
  )");

  // keySplines "0 0 1 1" is a linear curve (control points at corners).
  // At t=1s (25% overall, 50% of first interval): value = 50
  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_NEAR(std::stod(val1.value()), 50.0, 1.0);

  // At t=3s (75% overall, 50% of second interval): value = 50
  auto val3 = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(val3.has_value());
  EXPECT_NEAR(std::stod(val3.value()), 50.0, 1.0);
}

TEST(SVGAnimateElement, SplineParseKeySplines) {
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" values="0;50;100" begin="0s" dur="2s"
                 calcMode="spline" keyTimes="0;0.5;1"
                 keySplines="0.25 0.1 0.25 1; 0.42 0 1 1" />
      </rect>
    </svg>
  )");

  auto& registry = document.registry();
  auto view = registry.view<components::AnimateValueComponent>();
  auto entity = *view.begin();
  auto& valueComp = registry.get<components::AnimateValueComponent>(entity);

  // Should have 8 doubles (2 intervals × 4 control points)
  ASSERT_EQ(valueComp.keySplines.size(), 8u);
  EXPECT_DOUBLE_EQ(valueComp.keySplines[0], 0.25);
  EXPECT_DOUBLE_EQ(valueComp.keySplines[1], 0.1);
  EXPECT_DOUBLE_EQ(valueComp.keySplines[2], 0.25);
  EXPECT_DOUBLE_EQ(valueComp.keySplines[3], 1.0);
  EXPECT_DOUBLE_EQ(valueComp.keySplines[4], 0.42);
  EXPECT_DOUBLE_EQ(valueComp.keySplines[5], 0.0);
  EXPECT_DOUBLE_EQ(valueComp.keySplines[6], 1.0);
  EXPECT_DOUBLE_EQ(valueComp.keySplines[7], 1.0);
}

// --- Paced calcMode tests ---

TEST(SVGAnimateElement, PacedEvenlySpaced) {
  // values="0;100;200" — equal distances, so paced = linear.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" values="0;100;200" begin="0s" dur="4s"
                 calcMode="paced" />
      </rect>
    </svg>
  )");

  // Distances: [0→100]=100, [100→200]=100. Total=200.
  // At t=1s (25%): targetDist=50, in first interval, localT=0.5 → value=50
  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_NEAR(std::stod(val1.value()), 50.0, 0.01);

  // At t=2s (50%): targetDist=100, at boundary → value=100
  auto val2 = getAnimatedValue(document, "#r", "width", 2.0);
  ASSERT_TRUE(val2.has_value());
  EXPECT_NEAR(std::stod(val2.value()), 100.0, 0.01);

  // At t=3s (75%): targetDist=150, in second interval, localT=0.5 → value=150
  auto val3 = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(val3.has_value());
  EXPECT_NEAR(std::stod(val3.value()), 150.0, 0.01);
}

TEST(SVGAnimateElement, PacedUnevenDistances) {
  // values="0;10;110" — distances: 10, 100. Total=110.
  // Paced distributes time proportional to distance.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" values="0;10;110" begin="0s" dur="11s"
                 calcMode="paced" />
      </rect>
    </svg>
  )");

  // Total distance = 110. First segment = 10 (9.09% of total), second = 100 (90.91%).
  // At t=0.5s: progress=0.5/11≈0.0455, targetDist=0.0455*110=5.0, in first interval (0→10)
  //   localT=5/10=0.5 → value=5
  auto val05 = getAnimatedValue(document, "#r", "width", 0.5);
  ASSERT_TRUE(val05.has_value());
  EXPECT_NEAR(std::stod(val05.value()), 5.0, 0.1);

  // At t=1s: progress=1/11≈0.0909, targetDist=10.0, at boundary → value=10
  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_NEAR(std::stod(val1.value()), 10.0, 0.1);

  // At t=6s: progress=6/11≈0.5455, targetDist=60.0
  //   First segment ends at 10.0, so in second interval, offset=50, localT=50/100=0.5
  //   value=10+(110-10)*0.5 = 60
  auto val6 = getAnimatedValue(document, "#r", "width", 6.0);
  ASSERT_TRUE(val6.has_value());
  EXPECT_NEAR(std::stod(val6.value()), 60.0, 0.1);
}

TEST(SVGAnimateElement, PacedFromTo) {
  // from/to with paced: same as linear (only 2 values).
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="2s"
                 calcMode="paced" />
      </rect>
    </svg>
  )");

  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_NEAR(std::stod(val1.value()), 50.0, 0.01);
}

// --- Animation sandwich model tests ---

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

TEST(SVGAnimateElement, AdditiveSum) {
  // Two animations: first is replace, second is additive="sum".
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="2s" />
        <animate attributeName="width" from="0" to="50" begin="0s" dur="2s" additive="sum" />
      </rect>
    </svg>
  )");

  // At t=1s (50%): first → 50, second additive → 50 + 25 = 75
  auto val = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val.has_value());
  EXPECT_NEAR(std::stod(val.value()), 75.0, 0.01);
}

TEST(SVGAnimateElement, AccumulateSum) {
  // accumulate="sum": each repeat iteration adds the previous iteration's final value.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate attributeName="width" from="0" to="100" begin="0s" dur="2s"
                 repeatCount="3" accumulate="sum" fill="freeze" />
      </rect>
    </svg>
  )");

  // Iteration 0 (t=0-2s): 0→100
  // Iteration 1 (t=2-4s): 0→100, plus accumulated 100 → 100→200
  // Iteration 2 (t=4-6s): 0→100, plus accumulated 200 → 200→300

  // At t=1s: iteration 0, progress=0.5, value=50 + 0*100 = 50
  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_NEAR(std::stod(val1.value()), 50.0, 0.01);

  // At t=3s: iteration 1, progress=0.5, value=50 + 1*100 = 150
  auto val3 = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(val3.has_value());
  EXPECT_NEAR(std::stod(val3.value()), 150.0, 0.01);

  // At t=5s: iteration 2, progress=0.5, value=50 + 2*100 = 250
  auto val5 = getAnimatedValue(document, "#r", "width", 5.0);
  ASSERT_TRUE(val5.has_value());
  EXPECT_NEAR(std::stod(val5.value()), 250.0, 0.01);
}

TEST(SVGAnimateElement, AdditiveSumWithSet) {
  // A <set> provides a base value, then an additive <animate> adds to it.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <set attributeName="width" to="50" begin="0s" dur="4s" />
        <animate attributeName="width" from="0" to="100" begin="0s" dur="4s" additive="sum" />
      </rect>
    </svg>
  )");

  // At t=2s (50%): <set> provides "50", <animate> additive adds 50 → 50+50 = 100
  auto val = getAnimatedValue(document, "#r", "width", 2.0);
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

// --- Syncbase timing tests ---

TEST(SVGAnimateElement, SyncbaseBeginAfterEnd) {
  // Second animation begins when first ends.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate id="a1" attributeName="width" from="100" to="200" begin="0s" dur="2s" />
        <animate id="a2" attributeName="width" from="200" to="300" begin="a1.end" dur="2s" />
      </rect>
    </svg>
  )");

  // At t=0s: a1 is active (from=100), a2 hasn't started → 100
  auto val0 = getAnimatedValue(document, "#r", "width", 0.0);
  ASSERT_TRUE(val0.has_value());
  EXPECT_NEAR(std::stod(val0.value()), 100.0, 0.5);

  // At t=1s: a1 is active (50% → 150), a2 hasn't started → 150
  auto val1 = getAnimatedValue(document, "#r", "width", 1.0);
  ASSERT_TRUE(val1.has_value());
  EXPECT_NEAR(std::stod(val1.value()), 150.0, 0.5);

  // At t=2.5s: a1 has ended (fill=remove), a2 is active (25% → 225)
  auto val25 = getAnimatedValue(document, "#r", "width", 2.5);
  ASSERT_TRUE(val25.has_value());
  EXPECT_NEAR(std::stod(val25.value()), 225.0, 0.5);

  // At t=3s: a2 is at 50% → 250
  auto val3 = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(val3.has_value());
  EXPECT_NEAR(std::stod(val3.value()), 250.0, 0.5);
}

TEST(SVGAnimateElement, SyncbaseBeginAfterEndWithOffset) {
  // Second animation begins 1s after first ends.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate id="a1" attributeName="width" from="100" to="200" begin="0s" dur="2s" />
        <animate id="a2" attributeName="width" from="0" to="100" begin="a1.end+1s" dur="2s" />
      </rect>
    </svg>
  )");

  // a1 ends at 2s, a2 begins at 3s (a1.end + 1s).
  // At t=2.5s: a1 ended, a2 hasn't started → no value
  auto val25 = getAnimatedValue(document, "#r", "width", 2.5);
  EXPECT_FALSE(val25.has_value());

  // At t=3.5s: a2 is active (25% → 25)
  auto val35 = getAnimatedValue(document, "#r", "width", 3.5);
  ASSERT_TRUE(val35.has_value());
  EXPECT_NEAR(std::stod(val35.value()), 25.0, 0.5);

  // At t=4s: a2 is at 50% → 50
  auto val4 = getAnimatedValue(document, "#r", "width", 4.0);
  ASSERT_TRUE(val4.has_value());
  EXPECT_NEAR(std::stod(val4.value()), 50.0, 0.5);
}

TEST(SVGAnimateElement, SyncbaseBeginRefBegin) {
  // Second animation begins when first begins (synchronized start).
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate id="a1" attributeName="width" from="100" to="200" begin="1s" dur="2s" />
        <animate id="a2" attributeName="height" from="100" to="200" begin="a1.begin" dur="2s" />
      </rect>
    </svg>
  )");

  // Both should be inactive before t=1s.
  auto wid0 = getAnimatedValue(document, "#r", "width", 0.5);
  EXPECT_FALSE(wid0.has_value());
  auto ht0 = getAnimatedValue(document, "#r", "height", 0.5);
  EXPECT_FALSE(ht0.has_value());

  // At t=2s: both are active at 50% → 150
  auto wid2 = getAnimatedValue(document, "#r", "width", 2.0);
  ASSERT_TRUE(wid2.has_value());
  EXPECT_NEAR(std::stod(wid2.value()), 150.0, 0.5);

  auto ht2 = getAnimatedValue(document, "#r", "height", 2.0);
  ASSERT_TRUE(ht2.has_value());
  EXPECT_NEAR(std::stod(ht2.value()), 150.0, 0.5);
}

TEST(SVGAnimateElement, SyncbaseChain) {
  // Three-animation chain: a → b → c.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <animate id="a1" attributeName="width" from="0" to="100" begin="0s" dur="1s" />
        <animate id="a2" attributeName="width" from="100" to="200" begin="a1.end" dur="1s" />
        <animate id="a3" attributeName="width" from="200" to="300" begin="a2.end" dur="1s" />
      </rect>
    </svg>
  )");

  // a1: 0-1s, a2: 1-2s, a3: 2-3s.
  // At t=0.5: a1 active at 50% → 50
  auto val05 = getAnimatedValue(document, "#r", "width", 0.5);
  ASSERT_TRUE(val05.has_value());
  EXPECT_NEAR(std::stod(val05.value()), 50.0, 0.5);

  // At t=1.5: a2 active at 50% → 150
  auto val15 = getAnimatedValue(document, "#r", "width", 1.5);
  ASSERT_TRUE(val15.has_value());
  EXPECT_NEAR(std::stod(val15.value()), 150.0, 0.5);

  // At t=2.5: a3 active at 50% → 250
  auto val25 = getAnimatedValue(document, "#r", "width", 2.5);
  ASSERT_TRUE(val25.has_value());
  EXPECT_NEAR(std::stod(val25.value()), 250.0, 0.5);
}

TEST(SVGAnimateElement, SyncbaseSetTrigger) {
  // <set> triggers <animate> via syncbase.
  auto document = parseSVGWithExperimental(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="r" width="100" height="100">
        <set id="s1" attributeName="fill" to="red" begin="0s" dur="2s" />
        <animate id="a1" attributeName="width" from="100" to="200" begin="s1.end" dur="2s" />
      </rect>
    </svg>
  )");

  // <set> ends at 2s, <animate> begins at 2s.
  // At t=1s: set is active (fill=red), animate hasn't started
  auto fill1 = getAnimatedValue(document, "#r", "fill", 1.0);
  ASSERT_TRUE(fill1.has_value());
  EXPECT_EQ(fill1.value(), "red");

  auto width1 = getAnimatedValue(document, "#r", "width", 1.0);
  EXPECT_FALSE(width1.has_value());

  // At t=3s: set has ended, animate is at 50% → 150
  auto width3 = getAnimatedValue(document, "#r", "width", 3.0);
  ASSERT_TRUE(width3.has_value());
  EXPECT_NEAR(std::stod(width3.value()), 150.0, 0.5);
}

// --- Edge case tests (Phase 8) ---

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
  // begin="1s;5s" — should start at the earliest qualifying time (1s).
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

  // At t=2s: past dur=1s but within min=3s, so still active.
  // The simple duration is 1s, so at t=2s we're in repeat iteration 2 (progress within iteration).
  // With no repeatCount, simple duration stays 1s but active duration is extended to 3s.
  // fmod(2.0, 1.0) = 0 → simpleTime = 1.0 → progress = 1.0
  auto val2 = getAnimatedValue(document, "#r", "width", 2.0);
  ASSERT_TRUE(val2.has_value());
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

}  // namespace donner::svg
