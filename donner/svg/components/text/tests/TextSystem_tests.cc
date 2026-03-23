/**
 * @file Tests for TextSystem.
 */

#include "donner/svg/components/text/TextSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/parser/SVGParser.h"

using testing::Eq;
using testing::IsEmpty;
using testing::SizeIs;

namespace donner::svg::components {

class TextSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseAndCompute(std::string_view input) {
    parser::SVGParser::Options options;
    options.enableExperimental = true;
    auto maybeResult = parser::SVGParser::ParseSVG(input, nullptr, options);
    EXPECT_THAT(maybeResult, NoParseError());
    auto document = std::move(maybeResult).result();

    auto& registry = document.registry();
    std::vector<ParseError> warnings;
    StyleSystem().computeAllStyles(registry, &warnings);
    TextSystem().instantiateAllComputedComponents(registry, &warnings);

    return document;
  }
};

// --- Basic text element ---

TEST_F(TextSystemTest, BasicTextElement) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <text id="t" x="10" y="20">Hello</text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  EXPECT_THAT(computed->spans, SizeIs(1));
  EXPECT_EQ(computed->spans[0].text, "Hello");
}

// --- Text with tspan children ---

TEST_F(TextSystemTest, TextWithTspan) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10" y="30">
        <tspan>First</tspan>
        <tspan dx="5">Second</tspan>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // The root <text> element produces a span (with its own text content, possibly empty),
  // followed by one span per <tspan> child.
  ASSERT_THAT(computed->spans, SizeIs(3));
  EXPECT_EQ(computed->spans[1].text, "First");
  EXPECT_EQ(computed->spans[2].text, "Second");
}

// --- Text positioning: x, y inherited from root ---

TEST_F(TextSystemTest, PositioningInheritedFromRoot) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <text id="t" x="15" y="25">
        <tspan>ABC</tspan>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + tspan child span.
  ASSERT_THAT(computed->spans, SizeIs(2));

  // The tspan should inherit x/y from the root <text> element via xList/yList.
  ASSERT_FALSE(computed->spans[1].xList.empty());
  ASSERT_TRUE(computed->spans[1].xList[0].has_value());
  EXPECT_DOUBLE_EQ(computed->spans[1].xList[0]->value, 15.0);
  ASSERT_FALSE(computed->spans[1].yList.empty());
  ASSERT_TRUE(computed->spans[1].yList[0].has_value());
  EXPECT_DOUBLE_EQ(computed->spans[1].yList[0]->value, 25.0);
}

// --- Per-character positioning ---

TEST_F(TextSystemTest, PerCharacterPositioning) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10 20 30" y="40">ABC</text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  ASSERT_THAT(computed->spans, SizeIs(1));

  auto& span = computed->spans[0];
  ASSERT_EQ(span.xList.size(), 3u);
  // xList[0] now holds the span-start value (was previously cleared).
  EXPECT_TRUE(span.xList[0].has_value());
  EXPECT_DOUBLE_EQ(span.xList[0]->value, 10.0);
  EXPECT_TRUE(span.xList[1].has_value());
  EXPECT_DOUBLE_EQ(span.xList[1]->value, 20.0);
  EXPECT_TRUE(span.xList[2].has_value());
  EXPECT_DOUBLE_EQ(span.xList[2]->value, 30.0);
}

// --- dx/dy positioning ---

TEST_F(TextSystemTest, DxDyPositioning) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t">
        <tspan dx="5 10" dy="2 4">AB</tspan>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + tspan child span.
  ASSERT_THAT(computed->spans, SizeIs(2));

  auto& span = computed->spans[1];
  ASSERT_EQ(span.dxList.size(), 2u);
  ASSERT_EQ(span.dyList.size(), 2u);
  // dxList[0] now holds the span-start value (was previously cleared).
  EXPECT_TRUE(span.dxList[0].has_value());
  EXPECT_DOUBLE_EQ(span.dxList[0]->value, 5.0);
  EXPECT_TRUE(span.dxList[1].has_value());
  EXPECT_DOUBLE_EQ(span.dxList[1]->value, 10.0);
  EXPECT_TRUE(span.dyList[0].has_value());
  EXPECT_DOUBLE_EQ(span.dyList[0]->value, 2.0);
  EXPECT_DOUBLE_EQ(span.dyList[1]->value, 4.0);
}

TEST_F(TextSystemTest, RootDxDyBecomesScalarStartPosition) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" dx="33" dy="100">Text</text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  ASSERT_THAT(computed->spans, SizeIs(1));

  const auto& span = computed->spans[0];
  ASSERT_EQ(span.dxList.size(), 4u);
  ASSERT_EQ(span.dyList.size(), 4u);
  // dxList[0]/dyList[0] now hold the span-start values (no longer cleared).
  EXPECT_TRUE(span.dxList[0].has_value());
  EXPECT_DOUBLE_EQ(span.dxList[0]->value, 33.0);
  EXPECT_TRUE(span.dyList[0].has_value());
  EXPECT_DOUBLE_EQ(span.dyList[0]->value, 100.0);
}

// --- Rotation ---

TEST_F(TextSystemTest, RotateAttribute) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t">
        <tspan rotate="45 90">AB</tspan>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + tspan child span.
  ASSERT_THAT(computed->spans, SizeIs(2));

  auto& span = computed->spans[1];
  ASSERT_THAT(span.rotateList, SizeIs(2));
  EXPECT_DOUBLE_EQ(span.rotateList[0], 45.0);
  EXPECT_DOUBLE_EQ(span.rotateList[1], 90.0);
}

// --- Empty text element ---

TEST_F(TextSystemTest, EmptyTextElement) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <text id="t" x="10" y="20"></text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // The root <text> element itself produces a single span even when empty.
  ASSERT_THAT(computed->spans, SizeIs(1));
  EXPECT_EQ(computed->spans[0].text, "");
}

// --- Multiple tspan with individual positioning ---

TEST_F(TextSystemTest, MultipleTspanWithPositioning) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t">
        <tspan x="10" y="20">A</tspan>
        <tspan x="30" y="40">B</tspan>
        <tspan x="50" y="60">C</tspan>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + 3 tspan children.
  ASSERT_THAT(computed->spans, SizeIs(4));

  ASSERT_TRUE(computed->spans[1].hasExplicitX());
  EXPECT_DOUBLE_EQ(computed->spans[1].xList[0]->value, 10.0);
  ASSERT_TRUE(computed->spans[1].hasExplicitY());
  EXPECT_DOUBLE_EQ(computed->spans[1].yList[0]->value, 20.0);
  ASSERT_TRUE(computed->spans[2].hasExplicitX());
  EXPECT_DOUBLE_EQ(computed->spans[2].xList[0]->value, 30.0);
  ASSERT_TRUE(computed->spans[2].hasExplicitY());
  EXPECT_DOUBLE_EQ(computed->spans[2].yList[0]->value, 40.0);
  ASSERT_TRUE(computed->spans[3].hasExplicitX());
  EXPECT_DOUBLE_EQ(computed->spans[3].xList[0]->value, 50.0);
  ASSERT_TRUE(computed->spans[3].hasExplicitY());
  EXPECT_DOUBLE_EQ(computed->spans[3].yList[0]->value, 60.0);
}

// --- textPath reference ---

TEST_F(TextSystemTest, TextPathReference) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <path id="p" d="M 10 80 C 40 10, 65 10, 95 80"/>
      </defs>
      <text id="t">
        <textPath href="#p">Along the path</textPath>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + textPath child span.
  ASSERT_THAT(computed->spans, SizeIs(2));

  // The textPath span should have a path spline attached.
  EXPECT_TRUE(computed->spans[1].pathSpline.has_value());
}

// --- textPath with startOffset ---

TEST_F(TextSystemTest, TextPathWithStartOffset) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <path id="p" d="M 0 0 L 100 0"/>
      </defs>
      <text id="t">
        <textPath href="#p" startOffset="50%">Offset text</textPath>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + textPath child span.
  ASSERT_THAT(computed->spans, SizeIs(2));

  // 50% of a 100-unit path should be ~50.
  EXPECT_TRUE(computed->spans[1].pathSpline.has_value());
  EXPECT_NEAR(computed->spans[1].pathStartOffset, 50.0, 1.0);
}

TEST_F(TextSystemTest, MixedTextPathChildrenProduceSeparateSpans) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
         viewBox="0 0 200 200">
      <defs>
        <path id="path1" d="M 20 100 C 35 135 85 135 80 100"/>
        <path id="path2" d="M 120 100 C 115 65 165 65 180 100"/>
      </defs>
      <text id="t" x="20" y="60">
        Some
        <textPath xlink:href="#path1">very</textPath>
        <tspan fill="green">long</tspan>
        <textPath xlink:href="#path2">text</textPath>
        .
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);

  std::vector<std::string> nonEmptyTexts;
  std::vector<bool> nonEmptyOnPath;
  std::vector<bool> nonEmptyHasSource;
  for (const auto& span : computed->spans) {
    if (span.text.empty()) {
      continue;
    }

    nonEmptyTexts.push_back(span.text.str());
    nonEmptyOnPath.push_back(span.pathSpline.has_value());
    nonEmptyHasSource.push_back(span.sourceEntity != entt::null);
  }

  EXPECT_THAT(nonEmptyTexts, testing::ElementsAre("Some ", "very", "long", "text", "."));
  EXPECT_THAT(nonEmptyOnPath, testing::ElementsAre(false, true, false, true, false));
  // All spans should have a source entity for style resolution.
  EXPECT_THAT(nonEmptyHasSource, testing::ElementsAre(true, true, true, true, true));
}

// --- UTF-8 multibyte characters ---

TEST_F(TextSystemTest, Utf8MultibyteCounting) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10 20 30">&#x4F60;&#x597D;!</text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  ASSERT_THAT(computed->spans, SizeIs(1));

  // Should have 3 characters (2 CJK + 1 ASCII), so 3 xList entries.
  EXPECT_EQ(computed->spans[0].xList.size(), 3u);
}

// --- No warnings for valid text ---

TEST_F(TextSystemTest, NoWarningsForValidText) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  auto maybeResult = parser::SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <text x="10" y="20">Hello World</text>
    </svg>
  )",
                                                 nullptr, options);
  ASSERT_TRUE(maybeResult.hasResult());
  auto document = std::move(maybeResult).result();

  auto& registry = document.registry();
  std::vector<ParseError> warnings;
  StyleSystem().computeAllStyles(registry, &warnings);
  TextSystem().instantiateAllComputedComponents(registry, &warnings);

  EXPECT_THAT(warnings, IsEmpty());
}

// --- List-only positioning (no scalar fields) ---

TEST_F(TextSystemTest, ListOnlyPositioning) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <text id="t" x="10" y="20">Hello</text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  ASSERT_THAT(computed->spans, SizeIs(1));

  const auto& span = computed->spans[0];
  // xList[0] and yList[0] hold the span-start position.
  ASSERT_TRUE(span.hasExplicitX());
  EXPECT_DOUBLE_EQ(span.xList[0]->value, 10.0);
  ASSERT_TRUE(span.hasExplicitY());
  EXPECT_DOUBLE_EQ(span.yList[0]->value, 20.0);
  EXPECT_TRUE(span.startsNewChunk);
}

// --- No double-application of dx ---

TEST_F(TextSystemTest, NoDoubleApplication) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" dx="5">A</text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  ASSERT_THAT(computed->spans, SizeIs(1));

  const auto& span = computed->spans[0];
  // dx should appear exactly once in dxList[0], not in a separate scalar field.
  ASSERT_GE(span.dxList.size(), 1u);
  ASSERT_TRUE(span.dxList[0].has_value());
  EXPECT_DOUBLE_EQ(span.dxList[0]->value, 5.0);
}

// --- Child tspan preserves dyList[0] ---

TEST_F(TextSystemTest, ChildTspanPreservesListZero) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10" y="20">
        <tspan dy="7">A</tspan>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + tspan child span.
  ASSERT_THAT(computed->spans, SizeIs(2));

  // The tspan's dyList[0] should retain dy=7 (not be cleared).
  const auto& span = computed->spans[1];
  ASSERT_GE(span.dyList.size(), 1u);
  ASSERT_TRUE(span.dyList[0].has_value());
  EXPECT_DOUBLE_EQ(span.dyList[0]->value, 7.0);
}

// --- startsNewChunk flag ---

TEST_F(TextSystemTest, StartsNewChunk) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10" y="20">
        <tspan>Continuation</tspan>
        <tspan x="50">NewChunk</tspan>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + 2 tspan children.
  ASSERT_THAT(computed->spans, SizeIs(3));

  // Root span starts a new chunk (has x and y).
  EXPECT_TRUE(computed->spans[0].startsNewChunk);
  // First tspan has no explicit position — continuation.
  EXPECT_FALSE(computed->spans[1].startsNewChunk);
  // Second tspan has explicit x — new chunk.
  EXPECT_TRUE(computed->spans[2].startsNewChunk);
}

}  // namespace donner::svg::components
