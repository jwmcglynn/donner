/**
 * @file Tests for PaintSystem: computed gradient and pattern resolution from SVG paint server
 * elements.
 */

#include "donner/svg/components/paint/PaintSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"

using testing::Gt;
using testing::IsEmpty;
using testing::Not;
using testing::NotNull;
using testing::SizeIs;

namespace donner::svg::components {

class PaintSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    auto maybeResult = parser::SVGParser::ParseSVG(input);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  SVGDocument ParseAndCompute(std::string_view input) {
    auto document = ParseSVG(input);
    StyleSystem().computeAllStyles(document.registry(), nullptr);
    paintSystem.createShadowTrees(document.registry(), nullptr);
    paintSystem.instantiateAllComputedComponents(document.registry(), nullptr);
    return document;
  }

  PaintSystem paintSystem;
};

// --- Linear gradient ---

TEST_F(PaintSystemTest, LinearGradientWithStops) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g">
          <stop offset="0" stop-color="red"/>
          <stop offset="1" stop-color="blue"/>
        </linearGradient>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#g");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->initialized);
  EXPECT_THAT(computed->stops, SizeIs(2));
}

TEST_F(PaintSystemTest, LinearGradientNoStops) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g"/>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#g");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->initialized);
  EXPECT_THAT(computed->stops, IsEmpty());
}

// --- Radial gradient ---

TEST_F(PaintSystemTest, RadialGradientWithStops) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <radialGradient id="g">
          <stop offset="0" stop-color="white"/>
          <stop offset="1" stop-color="black"/>
        </radialGradient>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#g");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->initialized);
  EXPECT_THAT(computed->stops, SizeIs(2));
}

// --- Gradient inheritance via href ---

TEST_F(PaintSystemTest, GradientInheritsStopsViaHref) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="base">
          <stop offset="0" stop-color="red"/>
          <stop offset="0.5" stop-color="green"/>
          <stop offset="1" stop-color="blue"/>
        </linearGradient>
        <linearGradient id="child" href="#base"/>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#child");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->stops, SizeIs(3));
}

TEST_F(PaintSystemTest, GradientHrefToNonGradientWarns) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <rect id="r" width="10" height="10"/>
        <linearGradient id="g" href="#r">
          <stop offset="0" stop-color="red"/>
        </linearGradient>
      </defs>
    </svg>
  )");

  std::vector<ParseError> warnings;
  StyleSystem().computeAllStyles(document.registry(), &warnings);
  paintSystem.createShadowTrees(document.registry(), &warnings);
  paintSystem.instantiateAllComputedComponents(document.registry(), &warnings);

  EXPECT_THAT(warnings, Not(IsEmpty()));
}

// --- Pattern ---

TEST_F(PaintSystemTest, PatternComputed) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <pattern id="p" width="20" height="20" patternUnits="userSpaceOnUse">
          <rect width="10" height="10" fill="red"/>
        </pattern>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedPatternComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->initialized);
}

TEST_F(PaintSystemTest, PatternHrefToNonPatternWarns) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <rect id="r" width="10" height="10"/>
        <pattern id="p" href="#r" width="20" height="20"/>
      </defs>
    </svg>
  )");

  std::vector<ParseError> warnings;
  StyleSystem().computeAllStyles(document.registry(), &warnings);
  paintSystem.createShadowTrees(document.registry(), &warnings);
  paintSystem.instantiateAllComputedComponents(document.registry(), &warnings);

  EXPECT_THAT(warnings, Not(IsEmpty()));
}

// --- Gradient spread method ---

TEST_F(PaintSystemTest, GradientSpreadMethod) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g" spreadMethod="reflect">
          <stop offset="0" stop-color="red"/>
          <stop offset="1" stop-color="blue"/>
        </linearGradient>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#g");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_EQ(computed->spreadMethod, GradientSpreadMethod::Reflect);
}

// --- Gradient units ---

TEST_F(PaintSystemTest, GradientUnitsObjectBoundingBox) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g" gradientUnits="objectBoundingBox">
          <stop offset="0" stop-color="red"/>
        </linearGradient>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#g");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_EQ(computed->gradientUnits, GradientUnits::ObjectBoundingBox);
}

TEST_F(PaintSystemTest, GradientUnitsUserSpaceOnUse) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g" gradientUnits="userSpaceOnUse">
          <stop offset="0" stop-color="red"/>
        </linearGradient>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#g");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_EQ(computed->gradientUnits, GradientUnits::UserSpaceOnUse);
}

}  // namespace donner::svg::components
