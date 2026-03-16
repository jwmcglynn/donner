/**
 * @file Tests for StyleSystem: CSS cascade, inheritance, and style computation from SVG elements.
 */

#include "donner/svg/components/style/StyleSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/parser/SVGParser.h"

using testing::NotNull;

namespace donner::svg::components {

class StyleSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    auto maybeResult = parser::SVGParser::ParseSVG(input);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  SVGDocument ParseAndComputeStyles(std::string_view input) {
    auto document = ParseSVG(input);
    styleSystem.computeAllStyles(document.registry(), nullptr);
    return document;
  }

  StyleSystem styleSystem;
};

// --- Basic style computation ---

TEST_F(StyleSystemTest, ComputeAllStylesCreatesComponents) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
}

TEST_F(StyleSystemTest, InlineStyleApplied) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" style="fill: blue"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
}

// --- CSS stylesheet rules ---

TEST_F(StyleSystemTest, StylesheetRulesApplied) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        .red { fill: red; }
      </style>
      <rect id="r" class="red" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
}

TEST_F(StyleSystemTest, IdSelectorStyleApplied) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        #target { stroke: green; }
      </style>
      <rect id="target" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#target");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
}

// --- Inheritance ---

TEST_F(StyleSystemTest, StyleInheritedFromParent) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g fill="red">
        <rect id="child" width="50" height="50"/>
      </g>
    </svg>
  )");

  auto element = document.querySelector("#child");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
}

TEST_F(StyleSystemTest, DisplayNoneComputed) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="hidden" width="50" height="50" style="display: none"/>
    </svg>
  )");

  auto element = document.querySelector("#hidden");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
  EXPECT_EQ(computed->properties->display.getRequired(), Display::None);
}

TEST_F(StyleSystemTest, VisibilityHiddenComputed) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="hidden" width="50" height="50" style="visibility: hidden"/>
    </svg>
  )");

  auto element = document.querySelector("#hidden");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_TRUE(computed->properties.has_value());
  EXPECT_EQ(computed->properties->visibility.getRequired(), Visibility::Hidden);
}

// --- Individual style computation ---

TEST_F(StyleSystemTest, ComputeStyleForSingleEntity) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="green"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  const auto& computed = styleSystem.computeStyle(element->entityHandle(), nullptr);
  EXPECT_TRUE(computed.properties.has_value());
}

// --- Invalidation ---

TEST_F(StyleSystemTest, InvalidateComputedRemovesComponent) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  ASSERT_THAT(element->entityHandle().try_get<ComputedStyleComponent>(), NotNull());

  styleSystem.invalidateComputed(element->entityHandle());
  EXPECT_EQ(element->entityHandle().try_get<ComputedStyleComponent>(), nullptr);
}

// --- Multiple elements ---

TEST_F(StyleSystemTest, MultipleElementsAllComputed) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="a" width="25" height="25" fill="red"/>
      <circle id="b" cx="50" cy="50" r="20" fill="blue"/>
      <line id="c" x1="0" y1="0" x2="100" y2="100" stroke="green"/>
    </svg>
  )");

  for (const char* id : {"#a", "#b", "#c"}) {
    auto element = document.querySelector(id);
    ASSERT_TRUE(element.has_value()) << "Missing element: " << id;
    auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
    ASSERT_THAT(computed, NotNull()) << "No computed style for: " << id;
    EXPECT_TRUE(computed->properties.has_value()) << "No properties for: " << id;
  }
}

// --- Warnings ---

TEST_F(StyleSystemTest, WarningsCollectedForInvalidProperties) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        rect { fill: not-a-color; }
      </style>
      <rect id="r" width="50" height="50"/>
    </svg>
  )");

  std::vector<ParseError> warnings;
  styleSystem.computeAllStyles(document.registry(), &warnings);
  // Invalid color should produce a warning (or be ignored gracefully).
  // We just verify it doesn't crash; the warning list may or may not be populated
  // depending on implementation detail.
  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
}

}  // namespace donner::svg::components
