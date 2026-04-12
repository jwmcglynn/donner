/**
 * @file Tests for FilterSystem: computed filter graph generation from SVG filter elements.
 */

#include "donner/svg/components/filter/FilterSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
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
    parser::SVGParser::Options options;
    options.enableExperimental = true;
    ParseWarningSink parseSink;
    auto maybeResult = parser::SVGParser::ParseSVG(input, parseSink, options);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  SVGDocument ParseAndComputeFilters(std::string_view input) {
    auto document = ParseSVG(input);
    ParseWarningSink warningSink;
    StyleSystem().computeAllStyles(document.registry(), warningSink);
    filterSystem.instantiateAllComputedComponents(document.registry(), warningSink);
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

TEST_F(FilterSystemTest, FeFloodResolvesCssCurrentColorAndOpacity) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f" color="lime">
          <feFlood style="flood-color: currentColor; flood-opacity: 0.25"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  ASSERT_THAT(computed->filterGraph.nodes, SizeIs(1));

  const auto* flood =
      std::get_if<filter_primitive::Flood>(&computed->filterGraph.nodes[0].primitive);
  ASSERT_NE(flood, nullptr);
  EXPECT_EQ(flood->floodColor, css::Color(css::RGBA(0, 0xFF, 0, 0xFF)));
  EXPECT_DOUBLE_EQ(flood->floodOpacity, 0.25);
}

TEST_F(FilterSystemTest, FeFloodInvalidCssOverlayProducesWarning) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feFlood style="flood-color: bogus; flood-opacity: bogus"/>
        </filter>
      </defs>
    </svg>
  )");

  ParseWarningSink warningSink;
  StyleSystem().computeAllStyles(document.registry(), warningSink);
  filterSystem.instantiateAllComputedComponents(document.registry(), warningSink);

  EXPECT_THAT(testing::PrintToString(warningSink.warnings()),
              testing::AllOf(testing::HasSubstr("Invalid color 'bogus'"),
                             testing::HasSubstr("Invalid alpha value")));
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

TEST_F(FilterSystemTest, FeComponentTransferSkipsNonFuncChildrenAndCapturesAllChannels) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feComponentTransfer>
            <desc>ignored</desc>
            <feFuncB type="table" tableValues="0 1"/>
            <feFuncA type="linear" slope="0.5"/>
          </feComponentTransfer>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  ASSERT_THAT(computed->filterGraph.nodes, SizeIs(1));

  const auto* transfer = std::get_if<filter_primitive::ComponentTransfer>(
      &computed->filterGraph.nodes[0].primitive);
  ASSERT_NE(transfer, nullptr);
  EXPECT_THAT(transfer->funcB.tableValues, testing::ElementsAre(0.0, 1.0));
  EXPECT_DOUBLE_EQ(transfer->funcA.slope, 0.5);
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

TEST_F(FilterSystemTest, FeMergeNodeWithoutInUsesPrevious) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feMerge>
            <feMergeNode />
          </feMerge>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  ASSERT_THAT(computed->filterGraph.nodes, SizeIs(1));
  ASSERT_THAT(computed->filterGraph.nodes[0].inputs, SizeIs(1));
  EXPECT_TRUE(std::holds_alternative<FilterInput::Previous>(
      computed->filterGraph.nodes[0].inputs[0].value));
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

TEST_F(FilterSystemTest, FeDropShadowResolvesCssCurrentColorAndOpacity) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f" color="red">
          <feDropShadow dx="4" dy="5" stdDeviation="6"
                        style="flood-color: currentColor; flood-opacity: 0.4"/>
        </filter>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  ASSERT_THAT(computed->filterGraph.nodes, SizeIs(1));

  const auto* shadow =
      std::get_if<filter_primitive::DropShadow>(&computed->filterGraph.nodes[0].primitive);
  ASSERT_NE(shadow, nullptr);
  EXPECT_EQ(shadow->floodColor, css::Color(css::RGBA(0xFF, 0, 0, 0xFF)));
  EXPECT_DOUBLE_EQ(shadow->floodOpacity, 0.4);
}

TEST_F(FilterSystemTest, FeDropShadowInvalidCssOverlayProducesWarning) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feDropShadow style="flood-color: bogus; flood-opacity: bogus"/>
        </filter>
      </defs>
    </svg>
  )");

  ParseWarningSink warningSink;
  StyleSystem().computeAllStyles(document.registry(), warningSink);
  filterSystem.instantiateAllComputedComponents(document.registry(), warningSink);

  EXPECT_THAT(testing::PrintToString(warningSink.warnings()),
              testing::AllOf(testing::HasSubstr("Invalid color 'bogus'"),
                             testing::HasSubstr("Invalid alpha value")));
}

TEST_F(FilterSystemTest, LightingColorResolvesCssCurrentColor) {
  auto document = ParseAndComputeFilters(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="diffuseFilter" color="blue">
          <feDiffuseLighting style="lighting-color: currentColor">
            <feDistantLight azimuth="10" elevation="20"/>
          </feDiffuseLighting>
        </filter>
        <filter id="specularFilter" color="lime">
          <feSpecularLighting style="lighting-color: currentColor">
            <fePointLight x="1" y="2" z="3"/>
          </feSpecularLighting>
        </filter>
      </defs>
    </svg>
  )");

  {
    auto element = document.querySelector("#diffuseFilter");
    ASSERT_TRUE(element.has_value());
    auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
    ASSERT_THAT(computed, NotNull());
    const auto* diffuse =
        std::get_if<filter_primitive::DiffuseLighting>(&computed->filterGraph.nodes[0].primitive);
    ASSERT_NE(diffuse, nullptr);
    EXPECT_EQ(diffuse->lightingColor, css::Color(css::RGBA(0, 0, 0xFF, 0xFF)));
  }

  {
    auto element = document.querySelector("#specularFilter");
    ASSERT_TRUE(element.has_value());
    auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
    ASSERT_THAT(computed, NotNull());
    const auto* specular =
        std::get_if<filter_primitive::SpecularLighting>(&computed->filterGraph.nodes[0].primitive);
    ASSERT_NE(specular, nullptr);
    EXPECT_EQ(specular->lightingColor, css::Color(css::RGBA(0, 0xFF, 0, 0xFF)));
  }
}

TEST_F(FilterSystemTest, LightingDefaultsAndInvalidCssOverlay) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="diffuseFilter" color-interpolation-filters="sRGB">
          <feDiffuseLighting style="lighting-color: bogus">
            <feDistantLight azimuth="10" elevation="20"/>
          </feDiffuseLighting>
        </filter>
        <filter id="specularFilter">
          <feSpecularLighting>
            <fePointLight x="1" y="2" z="3"/>
          </feSpecularLighting>
        </filter>
      </defs>
    </svg>
  )");

  ParseWarningSink warningSink;
  StyleSystem().computeAllStyles(document.registry(), warningSink);
  filterSystem.instantiateAllComputedComponents(document.registry(), warningSink);

  {
    auto element = document.querySelector("#diffuseFilter");
    ASSERT_TRUE(element.has_value());
    auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
    ASSERT_THAT(computed, NotNull());
    EXPECT_EQ(computed->colorInterpolationFilters, ColorInterpolationFilters::SRGB);
    EXPECT_EQ(computed->filterGraph.colorInterpolationFilters, ColorInterpolationFilters::SRGB);
    const auto* diffuse =
        std::get_if<filter_primitive::DiffuseLighting>(&computed->filterGraph.nodes[0].primitive);
    ASSERT_NE(diffuse, nullptr);
    EXPECT_EQ(diffuse->lightingColor, css::Color(css::RGBA(0xFF, 0xFF, 0xFF, 0xFF)));
  }

  {
    auto element = document.querySelector("#specularFilter");
    ASSERT_TRUE(element.has_value());
    auto* computed = element->entityHandle().try_get<ComputedFilterComponent>();
    ASSERT_THAT(computed, NotNull());
    const auto* specular =
        std::get_if<filter_primitive::SpecularLighting>(&computed->filterGraph.nodes[0].primitive);
    ASSERT_NE(specular, nullptr);
    EXPECT_EQ(specular->lightingColor, css::Color(css::RGBA(0xFF, 0xFF, 0xFF, 0xFF)));
  }

  EXPECT_THAT(testing::PrintToString(warningSink.warnings()),
              testing::HasSubstr("Invalid color 'bogus'"));
}

TEST_F(FilterSystemTest, HrefWarningsForInvalidAndCircularReferences) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <rect id="notFilter" width="10" height="10"/>
        <filter id="missingRef" href="#doesNotExist">
          <feGaussianBlur stdDeviation="1"/>
        </filter>
        <filter id="wrongType" href="#notFilter">
          <feGaussianBlur stdDeviation="1"/>
        </filter>
        <filter id="cycleA" href="#cycleB">
          <feGaussianBlur stdDeviation="1"/>
        </filter>
        <filter id="cycleB" href="#cycleA"/>
      </defs>
    </svg>
  )");

  ParseWarningSink warningSink;
  StyleSystem().computeAllStyles(document.registry(), warningSink);
  filterSystem.instantiateAllComputedComponents(document.registry(), warningSink);

  EXPECT_THAT(warningSink.warnings(), Not(IsEmpty()));
  EXPECT_THAT(warningSink.warnings()[0].reason,
              testing::AnyOf(testing::HasSubstr("failed to resolve"),
                             testing::HasSubstr("does not reference a <filter>"),
                             testing::HasSubstr("Circular filter inheritance detected")));
  EXPECT_THAT(testing::PrintToString(warningSink.warnings()),
              testing::AllOf(testing::HasSubstr("failed to resolve"),
                             testing::HasSubstr("does not reference a <filter>"),
                             testing::HasSubstr("Circular filter inheritance detected")));
}

TEST_F(FilterSystemTest, FeImageUsesLoadedSvgSubDocument) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f">
          <feImage id="img" href="external.svg"/>
        </filter>
      </defs>
    </svg>
  )");

  auto imageElement = document.querySelector("#img");
  ASSERT_TRUE(imageElement.has_value());
  imageElement->entityHandle().emplace<LoadedSVGImageComponent>(std::make_shared<Registry>());

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  StyleSystem().computeAllStyles(document.registry(), disabledSink);
  filterSystem.instantiateAllComputedComponents(document.registry(), disabledSink);

  auto filterElement = document.querySelector("#f");
  ASSERT_TRUE(filterElement.has_value());
  auto* computed = filterElement->entityHandle().try_get<ComputedFilterComponent>();
  ASSERT_THAT(computed, NotNull());
  ASSERT_THAT(computed->filterGraph.nodes, SizeIs(1));

  auto* imageNode = std::get_if<filter_primitive::Image>(&computed->filterGraph.nodes[0].primitive);
  ASSERT_THAT(imageNode, NotNull());
  EXPECT_TRUE(imageNode->svgSubDocument);
  EXPECT_THAT(imageNode->imageData, IsEmpty());
}

TEST_F(FilterSystemTest, FeImageUsesLoadedRasterAndFragmentReference) {
  {
    auto document = ParseSVG(R"(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
        <defs>
          <filter id="f">
            <feImage id="img" href="external.png"/>
          </filter>
        </defs>
      </svg>
    )");

    auto imageElement = document.querySelector("#img");
    ASSERT_TRUE(imageElement.has_value());
    LoadedImageComponent loaded;
    loaded.image = ImageResource{{0xFF, 0x00, 0x00, 0xFF}, 1, 1};
    imageElement->entityHandle().emplace<LoadedImageComponent>(std::move(loaded));

    ParseWarningSink disabledSink = ParseWarningSink::Disabled();
    StyleSystem().computeAllStyles(document.registry(), disabledSink);
    filterSystem.instantiateAllComputedComponents(document.registry(), disabledSink);

    auto filterElement = document.querySelector("#f");
    ASSERT_TRUE(filterElement.has_value());
    auto* computed = filterElement->entityHandle().try_get<ComputedFilterComponent>();
    ASSERT_THAT(computed, NotNull());
    auto* imageNode =
        std::get_if<filter_primitive::Image>(&computed->filterGraph.nodes[0].primitive);
    ASSERT_THAT(imageNode, NotNull());
    EXPECT_THAT(imageNode->imageData, testing::ElementsAre(0xFF, 0x00, 0x00, 0xFF));
    EXPECT_EQ(imageNode->imageWidth, 1);
    EXPECT_EQ(imageNode->imageHeight, 1);
  }

  {
    auto document = ParseAndComputeFilters(R"(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
        <defs>
          <filter id="f">
            <feImage href="#rectRef"/>
          </filter>
          <rect id="rectRef" width="10" height="10"/>
        </defs>
      </svg>
    )");

    auto filterElement = document.querySelector("#f");
    ASSERT_TRUE(filterElement.has_value());
    auto* computed = filterElement->entityHandle().try_get<ComputedFilterComponent>();
    ASSERT_THAT(computed, NotNull());
    auto* imageNode =
        std::get_if<filter_primitive::Image>(&computed->filterGraph.nodes[0].primitive);
    ASSERT_THAT(imageNode, NotNull());
    EXPECT_TRUE(imageNode->isFragmentReference);
    EXPECT_EQ(imageNode->fragmentId, "rectRef");
  }
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

TEST_F(FilterSystemTest, EmptyFilterRemovesComputedComponent) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="f"/>
      </defs>
    </svg>
  )");

  auto element = document.querySelector("#f");
  ASSERT_TRUE(element.has_value());
  element->entityHandle().emplace<ComputedFilterComponent>();

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  StyleSystem().computeAllStyles(document.registry(), disabledSink);
  filterSystem.instantiateAllComputedComponents(document.registry(), disabledSink);

  EXPECT_FALSE(element->entityHandle().all_of<ComputedFilterComponent>());
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
