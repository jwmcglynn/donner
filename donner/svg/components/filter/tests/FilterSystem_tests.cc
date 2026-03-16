/**
 * @file Tests for FilterSystem: computed filter graph generation from SVG filter elements.
 */

#include "donner/svg/components/filter/FilterSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"

using testing::Gt;
using testing::IsEmpty;
using testing::Not;
using testing::NotNull;
using testing::SizeIs;

namespace donner::svg::components {

class FilterSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    auto maybeResult = parser::SVGParser::ParseSVG(input);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  SVGDocument ParseAndComputeFilters(std::string_view input) {
    auto document = ParseSVG(input);
    StyleSystem().computeAllStyles(document.registry(), nullptr);
    filterSystem.instantiateAllComputedComponents(document.registry(), nullptr);
    return document;
  }

  FilterSystem filterSystem;
};

// --- Gaussian blur ---

TEST_F(FilterSystemTest, GaussianBlur) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feGaussianBlur stdDeviation="5"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_FALSE(computed->filterGraph.empty());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- feFlood ---

TEST_F(FilterSystemTest, FeFlood) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feFlood flood-color="red" flood-opacity="0.5"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- feOffset ---

TEST_F(FilterSystemTest, FeOffset) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feOffset dx="10" dy="10"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- feComposite ---

TEST_F(FilterSystemTest, FeComposite) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feFlood flood-color="red" result="flood"/>
          <feComposite in="SourceGraphic" in2="flood" operator="over"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(2));
}

// --- feColorMatrix ---

TEST_F(FilterSystemTest, FeColorMatrix) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feColorMatrix type="saturate" values="0.5"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- feBlend ---

TEST_F(FilterSystemTest, FeBlend) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feFlood flood-color="red" result="flood"/>
          <feBlend in="SourceGraphic" in2="flood" mode="multiply"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(2));
}

// --- feComponentTransfer ---

TEST_F(FilterSystemTest, FeComponentTransfer) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feComponentTransfer>
            <feFuncR type="linear" slope="0.5" intercept="0.25"/>
            <feFuncG type="identity"/>
          </feComponentTransfer>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- feMerge ---

TEST_F(FilterSystemTest, FeMerge) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feFlood flood-color="red" result="flood"/>
          <feMerge>
            <feMergeNode in="flood"/>
            <feMergeNode in="SourceGraphic"/>
          </feMerge>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(2));
}

// --- feDropShadow ---

TEST_F(FilterSystemTest, FeDropShadow) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feDropShadow dx="4" dy="4" stdDeviation="2"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- feMorphology ---

TEST_F(FilterSystemTest, FeMorphology) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feMorphology operator="dilate" radius="2"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- feTurbulence ---

TEST_F(FilterSystemTest, FeTurbulence) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feTurbulence type="fractalNoise" baseFrequency="0.05" numOctaves="2"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- feTile ---

TEST_F(FilterSystemTest, FeTile) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feTile/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- feConvolveMatrix ---

TEST_F(FilterSystemTest, FeConvolveMatrix) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feConvolveMatrix order="3" kernelMatrix="0 -1 0 -1 5 -1 0 -1 0"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- feDisplacementMap ---

TEST_F(FilterSystemTest, FeDisplacementMap) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feTurbulence type="turbulence" baseFrequency="0.05" result="noise"/>
          <feDisplacementMap in="SourceGraphic" in2="noise" scale="20"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(2));
}

// --- feDiffuseLighting ---

TEST_F(FilterSystemTest, FeDiffuseLighting) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feDiffuseLighting surfaceScale="5" diffuseConstant="1">
            <fePointLight x="50" y="50" z="100"/>
          </feDiffuseLighting>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- feSpecularLighting ---

TEST_F(FilterSystemTest, FeSpecularLighting) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feSpecularLighting surfaceScale="5" specularConstant="1" specularExponent="20">
            <feDistantLight azimuth="45" elevation="55"/>
          </feSpecularLighting>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(1));
}

// --- Empty filter ---

TEST_F(FilterSystemTest, EmptyFilterNoComputed) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f"/>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  EXPECT_EQ(computed, nullptr);
}

// --- Filter chain (multiple primitives) ---

TEST_F(FilterSystemTest, FilterChainMultiplePrimitives) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feGaussianBlur stdDeviation="3" result="blur"/>
          <feOffset in="blur" dx="5" dy="5" result="offset"/>
          <feMerge>
            <feMergeNode in="offset"/>
            <feMergeNode in="SourceGraphic"/>
          </feMerge>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->filterGraph.nodes, SizeIs(3));
}

// --- Filter attributes ---

TEST_F(FilterSystemTest, FilterCustomAttributes) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f" x="-20%" y="-20%" width="140%" height="140%"
                filterUnits="objectBoundingBox" primitiveUnits="userSpaceOnUse">
          <feGaussianBlur stdDeviation="2"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_EQ(computed->filterUnits, FilterUnits::ObjectBoundingBox);
  EXPECT_EQ(computed->primitiveUnits, PrimitiveUnits::UserSpaceOnUse);
}

}  // namespace donner::svg::components
