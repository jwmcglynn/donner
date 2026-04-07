/**
 * @file Tests for PaintSystem: computed gradient and pattern resolution from SVG paint server
 * elements.
 */

#include "donner/svg/components/paint/PaintSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeSystem.h"
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
    ParseWarningSink parseSink;
    auto maybeResult = parser::SVGParser::ParseSVG(input, parseSink);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  SVGDocument ParseAndCompute(std::string_view input) {
    auto document = ParseSVG(input);
    auto& registry = document.registry();
    ParseWarningSink warningSink;
    StyleSystem().computeAllStyles(registry, warningSink);
    paintSystem.createShadowTrees(registry, warningSink);

    // Instantiate shadow trees (needed for gradient/pattern href inheritance).
    for (auto view = registry.view<ShadowTreeComponent>(); auto entity : view) {
      auto [shadowTreeComponent] = view.get(entity);
      if (auto targetEntity = shadowTreeComponent.mainTargetEntity(registry)) {
        auto& shadow = registry.get_or_emplace<ComputedShadowTreeComponent>(entity);
        ShadowTreeSystem().populateInstance(EntityHandle(registry, entity), shadow,
                                           ShadowBranchType::Main, targetEntity.value(),
                                           shadowTreeComponent.mainHref().value(), warningSink);
      }
    }

    // Re-compute styles to include shadow tree entities.
    StyleSystem().computeAllStyles(registry, warningSink);

    LayoutSystem().instantiateAllComputedComponents(registry, warningSink);
    paintSystem.instantiateAllComputedComponents(registry, warningSink);
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

  ParseWarningSink warningSink;
  StyleSystem().computeAllStyles(document.registry(), warningSink);
  paintSystem.createShadowTrees(document.registry(), warningSink);
  paintSystem.instantiateAllComputedComponents(document.registry(), warningSink);

  EXPECT_THAT(warningSink.warnings(), Not(IsEmpty()));
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

  ParseWarningSink warningSink;
  StyleSystem().computeAllStyles(document.registry(), warningSink);
  paintSystem.createShadowTrees(document.registry(), warningSink);
  paintSystem.instantiateAllComputedComponents(document.registry(), warningSink);

  EXPECT_THAT(warningSink.warnings(), Not(IsEmpty()));
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

// --- Multiple stops at same offset ---

TEST_F(PaintSystemTest, MultipleStopsAtSameOffset) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g">
          <stop offset="0" stop-color="red"/>
          <stop offset="0.5" stop-color="red"/>
          <stop offset="0.5" stop-color="blue"/>
          <stop offset="1" stop-color="green"/>
        </linearGradient>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#g");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->initialized);

  // All four stops should be preserved, including the two at offset 0.5.
  ASSERT_THAT(computed->stops, SizeIs(4));
  EXPECT_FLOAT_EQ(computed->stops[0].offset, 0.0f);
  EXPECT_FLOAT_EQ(computed->stops[1].offset, 0.5f);
  EXPECT_FLOAT_EQ(computed->stops[2].offset, 0.5f);
  EXPECT_FLOAT_EQ(computed->stops[3].offset, 1.0f);

  // Verify colors at the duplicate offset: first is red, second is blue.
  EXPECT_EQ(computed->stops[1].color, css::Color(css::RGBA(0xFF, 0, 0, 0xFF)));
  EXPECT_EQ(computed->stops[2].color, css::Color(css::RGBA(0, 0, 0xFF, 0xFF)));
}

// --- Gradient with objectBoundingBox coordinates ---

TEST_F(PaintSystemTest, LinearGradientObjectBoundingBoxCoordinates) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g" gradientUnits="objectBoundingBox"
                        x1="0.2" y1="0.3" x2="0.8" y2="0.9">
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
  EXPECT_EQ(computed->gradientUnits, GradientUnits::ObjectBoundingBox);

  auto* linear = element->entityHandle().try_get<ComputedLinearGradientComponent>();
  ASSERT_THAT(linear, NotNull());
  // Coordinates should be stored as specified (unitless values become px in parser).
  EXPECT_DOUBLE_EQ(linear->x1.value, 0.2);
  EXPECT_DOUBLE_EQ(linear->y1.value, 0.3);
  EXPECT_DOUBLE_EQ(linear->x2.value, 0.8);
  EXPECT_DOUBLE_EQ(linear->y2.value, 0.9);
}

// --- Radial gradient with focal point ---

TEST_F(PaintSystemTest, RadialGradientFocalPoint) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <radialGradient id="g" fx="0.2" fy="0.2">
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

  auto* radial = element->entityHandle().try_get<ComputedRadialGradientComponent>();
  ASSERT_THAT(radial, NotNull());

  // fx and fy should be set to the explicitly specified values.
  ASSERT_TRUE(radial->fx.has_value());
  EXPECT_DOUBLE_EQ(radial->fx->value, 0.2);
  ASSERT_TRUE(radial->fy.has_value());
  EXPECT_DOUBLE_EQ(radial->fy->value, 0.2);

  // cx, cy, r should remain at defaults (50%).
  EXPECT_EQ(radial->cx, Lengthd(50, Lengthd::Unit::Percent));
  EXPECT_EQ(radial->cy, Lengthd(50, Lengthd::Unit::Percent));
  EXPECT_EQ(radial->r, Lengthd(50, Lengthd::Unit::Percent));
}

// --- Gradient with spreadMethod reflect and repeat ---

TEST_F(PaintSystemTest, GradientSpreadMethodRepeat) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g" spreadMethod="repeat">
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
  EXPECT_EQ(computed->spreadMethod, GradientSpreadMethod::Repeat);
}

TEST_F(PaintSystemTest, GradientSpreadMethodDefaultIsPad) {
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
  EXPECT_EQ(computed->spreadMethod, GradientSpreadMethod::Pad);
}

// --- Pattern href inheritance ---

TEST_F(PaintSystemTest, PatternInheritsAttributesViaHref) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <pattern id="p1" width="20" height="20" patternUnits="userSpaceOnUse">
          <rect width="10" height="10" fill="red"/>
        </pattern>
        <pattern id="p2" href="#p1"/>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#p2");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedPatternComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->initialized);

  // p2 should inherit patternUnits from p1.
  EXPECT_EQ(computed->patternUnits, PatternUnits::UserSpaceOnUse);
}

TEST_F(PaintSystemTest, PatternHrefChildOverridesBase) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <pattern id="p1" width="20" height="20" patternUnits="userSpaceOnUse">
          <rect width="10" height="10" fill="red"/>
        </pattern>
        <pattern id="p2" href="#p1" patternUnits="objectBoundingBox"/>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#p2");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedPatternComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->initialized);

  // p2 sets its own patternUnits, so should override base.
  EXPECT_EQ(computed->patternUnits, PatternUnits::ObjectBoundingBox);
}

// --- Gradient with gradientTransform ---

TEST_F(PaintSystemTest, GradientTransformIsApplied) {
  auto document = ParseAndCompute(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g" gradientTransform="translate(10, 20)">
          <stop offset="0" stop-color="red"/>
          <stop offset="1" stop-color="blue"/>
        </linearGradient>
      </defs>
    </svg>
  )svg");

  auto element = document.querySelector("#g");
  ASSERT_TRUE(element.has_value());

  auto* localTransform =
      element->entityHandle().try_get<ComputedLocalTransformComponent>();
  ASSERT_THAT(localTransform, NotNull());
  // The gradientTransform is mapped to the entity's local transform, and should not be identity.
  EXPECT_FALSE(localTransform->entityFromParent.isIdentity());
}

TEST_F(PaintSystemTest, GradientTransformDefaultIsIdentity) {
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

  // When no gradientTransform is specified, there should be no computed local transform, or it
  // should be identity.
  auto* localTransform =
      element->entityHandle().try_get<ComputedLocalTransformComponent>();
  if (localTransform) {
    EXPECT_TRUE(localTransform->entityFromParent.isIdentity());
  }
}

// --- Stop with explicit and currentColor ---

TEST_F(PaintSystemTest, StopWithExplicitColor) {
  auto document = ParseAndCompute(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g">
          <stop offset="0" stop-color="#00ff00"/>
          <stop offset="0.5" stop-color="rgb(128, 0, 255)"/>
          <stop offset="1" stop-color="black"/>
        </linearGradient>
      </defs>
    </svg>
  )svg");

  auto element = document.querySelector("#g");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());
  ASSERT_THAT(computed->stops, SizeIs(3));

  EXPECT_EQ(computed->stops[0].color, css::Color(css::RGBA(0, 0xFF, 0, 0xFF)));
  EXPECT_EQ(computed->stops[1].color, css::Color(css::RGBA(128, 0, 255, 0xFF)));
  EXPECT_EQ(computed->stops[2].color, css::Color(css::RGBA(0, 0, 0, 0xFF)));
}

TEST_F(PaintSystemTest, StopWithCurrentColor) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g" color="green">
          <stop offset="0" stop-color="currentColor"/>
          <stop offset="1" stop-color="blue"/>
        </linearGradient>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#g");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());
  ASSERT_THAT(computed->stops, SizeIs(2));

  // currentColor should resolve to the inherited "color" property, which is green.
  EXPECT_EQ(computed->stops[0].color, css::Color(css::RGBA(0, 128, 0, 0xFF)));
  EXPECT_EQ(computed->stops[1].color, css::Color(css::RGBA(0, 0, 0xFF, 0xFF)));
}

// --- Gradient inherits spreadMethod via href ---

TEST_F(PaintSystemTest, GradientInheritsSpreadMethodViaHref) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="base" spreadMethod="reflect">
          <stop offset="0" stop-color="red"/>
          <stop offset="1" stop-color="blue"/>
        </linearGradient>
        <linearGradient id="child" href="#base"/>
      </defs>
    </svg>
  )");

  auto child = document.querySelector("#child");
  ASSERT_TRUE(child.has_value());
  auto* computed = child->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());

  // spreadMethod should be inherited from the base gradient.
  EXPECT_EQ(computed->spreadMethod, GradientSpreadMethod::Reflect);
}

TEST_F(PaintSystemTest, GradientChildOverridesSpreadMethodFromHref) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="base" spreadMethod="reflect">
          <stop offset="0" stop-color="red"/>
          <stop offset="1" stop-color="blue"/>
        </linearGradient>
        <linearGradient id="child" href="#base" spreadMethod="repeat"/>
      </defs>
    </svg>
  )");

  auto child = document.querySelector("#child");
  ASSERT_TRUE(child.has_value());
  auto* computed = child->entityHandle().try_get<ComputedGradientComponent>();
  ASSERT_THAT(computed, NotNull());

  // child explicitly sets spreadMethod, so it should override base.
  EXPECT_EQ(computed->spreadMethod, GradientSpreadMethod::Repeat);
}

}  // namespace donner::svg::components
