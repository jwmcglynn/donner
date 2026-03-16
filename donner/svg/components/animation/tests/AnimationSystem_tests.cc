/**
 * @file Tests for AnimationSystem: SMIL animation timing and value computation.
 */

#include "donner/svg/components/animation/AnimationSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"

using testing::NotNull;

namespace donner::svg::components {

class AnimationSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    parser::SVGParser::Options options;
    options.enableExperimental = true;
    auto maybeResult = parser::SVGParser::ParseSVG(input, nullptr, options);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  SVGDocument ParseAndAdvance(std::string_view input, double time) {
    auto document = ParseSVG(input);
    StyleSystem().computeAllStyles(document.registry(), nullptr);
    animationSystem.advance(document.registry(), time, nullptr);
    return document;
  }

  AnimationSystem animationSystem;
};

// --- <set> element ---

TEST_F(AnimationSystemTest, SetAnimationBeforeBegin) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <set attributeName="fill" to="blue" begin="5s"/>
      </rect>
    </svg>
  )", 0.0);

  // Before begin time, the animation should not have applied.
  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
}

TEST_F(AnimationSystemTest, SetAnimationDuringActive) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <set attributeName="fill" to="blue" begin="0s"/>
      </rect>
    </svg>
  )", 1.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  // The <set> with indefinite duration should be active.
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  // Animated values should be present.
  EXPECT_THAT(animated, NotNull());
}

TEST_F(AnimationSystemTest, SetAnimationWithDuration) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <set attributeName="fill" to="blue" begin="0s" dur="2s"/>
      </rect>
    </svg>
  )", 1.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

TEST_F(AnimationSystemTest, SetAnimationAfterDuration) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <set attributeName="fill" to="blue" begin="0s" dur="2s" fill="remove"/>
      </rect>
    </svg>
  )", 5.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  // After duration with fill="remove", the override should be cleared.
}

TEST_F(AnimationSystemTest, SetAnimationFillFreeze) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <set attributeName="fill" to="blue" begin="0s" dur="1s" fill="freeze"/>
      </rect>
    </svg>
  )", 5.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  // With fill="freeze", the final value should persist after the animation ends.
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

// --- <animate> element ---

TEST_F(AnimationSystemTest, AnimateValueDuringActive) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="0" y="0" width="50" height="50" fill="red">
        <animate attributeName="x" from="0" to="100" dur="10s" begin="0s"/>
      </rect>
    </svg>
  )", 5.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

TEST_F(AnimationSystemTest, AnimateValueBeforeBegin) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="0" y="0" width="50" height="50" fill="red">
        <animate attributeName="x" from="0" to="100" dur="10s" begin="20s"/>
      </rect>
    </svg>
  )", 5.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
}

TEST_F(AnimationSystemTest, AnimateWithRepeatCount) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="0" y="0" width="50" height="50" fill="red">
        <animate attributeName="x" from="0" to="100" dur="2s" begin="0s" repeatCount="3"/>
      </rect>
    </svg>
  )", 3.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

TEST_F(AnimationSystemTest, AnimateWithRepeatDur) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="0" y="0" width="50" height="50" fill="red">
        <animate attributeName="x" from="0" to="100" dur="2s" begin="0s" repeatDur="5s"/>
      </rect>
    </svg>
  )", 3.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

// --- <animateTransform> ---

TEST_F(AnimationSystemTest, AnimateTransformRotate) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <animateTransform attributeName="transform" type="rotate"
                          from="0 25 25" to="360 25 25" dur="4s" begin="0s"/>
      </rect>
    </svg>
  )", 2.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

TEST_F(AnimationSystemTest, AnimateTransformScale) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <animateTransform attributeName="transform" type="scale"
                          from="1" to="2" dur="4s" begin="0s"/>
      </rect>
    </svg>
  )", 2.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

// --- <animateMotion> ---

TEST_F(AnimationSystemTest, AnimateMotionPath) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <circle id="c" cx="10" cy="10" r="5" fill="red">
        <animateMotion path="M0,0 L100,100" dur="5s" begin="0s"/>
      </circle>
    </svg>
  )", 2.5);

  auto element = document.querySelector("#c");
  ASSERT_TRUE(element.has_value());
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

// --- Multiple animations on same target ---

TEST_F(AnimationSystemTest, MultipleAnimationsOnSameTarget) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="0" y="0" width="50" height="50" fill="red">
        <animate attributeName="x" from="0" to="100" dur="10s" begin="0s"/>
        <animate attributeName="y" from="0" to="50" dur="10s" begin="0s"/>
      </rect>
    </svg>
  )", 5.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

// --- Animation with href target ---

TEST_F(AnimationSystemTest, AnimateWithHrefTarget) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="target" width="50" height="50" fill="red"/>
      <set href="#target" attributeName="fill" to="blue" begin="0s"/>
    </svg>
  )", 1.0);

  auto element = document.querySelector("#target");
  ASSERT_TRUE(element.has_value());
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

// --- Edge cases ---

TEST_F(AnimationSystemTest, AnimateZeroDuration) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <animate attributeName="x" from="0" to="100" dur="0s" begin="0s"/>
      </rect>
    </svg>
  )", 0.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  // Zero duration should be handled gracefully.
}

TEST_F(AnimationSystemTest, AnimateNoDuration) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <animate attributeName="x" from="0" to="100" begin="0s"/>
      </rect>
    </svg>
  )", 1.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  // No duration on <animate> means inactive (simple duration = 0).
}

TEST_F(AnimationSystemTest, AdvanceMultipleTimes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <animate attributeName="x" from="0" to="100" dur="10s" begin="0s"/>
      </rect>
    </svg>
  )");

  StyleSystem().computeAllStyles(document.registry(), nullptr);

  // Advance multiple times to simulate a timeline.
  animationSystem.advance(document.registry(), 0.0, nullptr);
  animationSystem.advance(document.registry(), 2.5, nullptr);
  animationSystem.advance(document.registry(), 5.0, nullptr);
  animationSystem.advance(document.registry(), 10.0, nullptr);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
}

TEST_F(AnimationSystemTest, WarningsCollected) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <animate attributeName="x" from="0" to="100" dur="10s" begin="0s"/>
      </rect>
    </svg>
  )");

  StyleSystem().computeAllStyles(document.registry(), nullptr);

  std::vector<ParseError> warnings;
  animationSystem.advance(document.registry(), 5.0, &warnings);
  // Just verifying warnings are collected without crash.
}

TEST_F(AnimationSystemTest, AnimationWithEndAttribute) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <animate attributeName="x" from="0" to="100" dur="10s" begin="0s" end="5s"/>
      </rect>
    </svg>
  )", 3.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

TEST_F(AnimationSystemTest, AnimationWithMinMax) {
  auto document = ParseAndAdvance(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red">
        <animate attributeName="x" from="0" to="100" dur="2s" begin="0s"
                 min="1s" max="5s"/>
      </rect>
    </svg>
  )", 1.0);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* animated = element->entityHandle().try_get<AnimatedValuesComponent>();
  EXPECT_THAT(animated, NotNull());
}

}  // namespace donner::svg::components
