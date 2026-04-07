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
#include "donner/svg/components/text/TextComponent.h"
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
    std::vector<ParseDiagnostic> warnings;
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
      <text id="t" x="10" y="30"><tspan>First</tspan><tspan dx="5">Second</tspan></text>
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
      <text id="t" x="15" y="25"><tspan>ABC</tspan></text>
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
      <text id="t"><tspan dx="5 10" dy="2 4">AB</tspan></text>
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
      <text id="t"><tspan rotate="45 90">AB</tspan></text>
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
      <text id="t"><tspan x="10" y="20">A</tspan><tspan x="30" y="40">B</tspan><tspan x="50" y="60">C</tspan></text>
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
      <text id="t"><textPath href="#p">Along the path</textPath></text>
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
      <text id="t"><textPath href="#p" startOffset="50%">Offset text</textPath></text>
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

  // Whitespace between elements is preserved and collapses to single spaces per SVG §10.15.
  // The leading space of "Some " comes from its trailing whitespace in the first root chunk.
  // Each inter-element boundary produces a " " span from the root element's whitespace chunks.
  EXPECT_THAT(nonEmptyTexts, testing::ElementsAre("Some ", "very", " ", "long", " ", "text", " ."));
  EXPECT_THAT(nonEmptyOnPath, testing::ElementsAre(false, true, false, false, false, true, false));
  // All spans should have a source entity for style resolution.
  EXPECT_THAT(nonEmptyHasSource, testing::ElementsAre(true, true, true, true, true, true, true));
}

TEST_F(TextSystemTest, NestedTextPathContentIsHidden) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
         viewBox="0 0 200 200">
      <defs>
        <path id="path1" d="M 20 73 C 35 108 85 108 100 73 C 115 38 165 38 180 73"/>
        <path id="path2" d="M 20 127 C 35 162 85 162 100 127 C 115 92 165 92 180 127"/>
      </defs>
      <text id="t" font-size="24">
        <textPath xlink:href="#path1">
          Some long text
          <textPath xlink:href="#path2">Ignored nested path</textPath>
        </textPath>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();
  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);

  // Per SVG spec, nested <textPath> is invalid. The outer textPath content should be
  // on the path, but the nested textPath content should be hidden entirely.
  std::vector<const ComputedTextComponent::TextSpan*> nonEmptySpans;
  for (const auto& span : computed->spans) {
    if (!span.text.empty()) {
      nonEmptySpans.push_back(&span);
    }
  }

  ASSERT_THAT(nonEmptySpans, testing::SizeIs(testing::Ge(2)));
  // First span "Some long text" should be on path1.
  ASSERT_TRUE(nonEmptySpans[0]->pathSpline.has_value());
  // Nested textPath content "Ignored nested path" should be hidden.
  bool foundNested = false;
  for (const auto& span : computed->spans) {
    if (span.text.str().find("Ignored") != std::string::npos) {
      EXPECT_TRUE(span.hidden) << "Nested textPath content should be hidden";
      foundNested = true;
    }
  }
  EXPECT_TRUE(foundNested) << "Should have found the nested textPath content span";
}

TEST_F(TextSystemTest, TextPathTspanChildrenProduceSeparatePathSpans) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
         viewBox="0 0 200 200">
      <defs>
        <path id="path1" d="M 20 100 C 35 135 85 135 100 100 C 115 65 165 65 180 100"/>
      </defs>
      <text id="t" font-size="24">
        <textPath xlink:href="#path1">
          Some <tspan fill="green" x="10" y="20">long</tspan> text
        </textPath>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();
  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);

  std::vector<std::string> nonEmptyTexts;
  std::vector<bool> onPath;
  for (const auto& span : computed->spans) {
    if (span.text.empty()) {
      continue;
    }
    nonEmptyTexts.push_back(span.text.str());
    onPath.push_back(span.pathSpline.has_value());
  }

  EXPECT_THAT(nonEmptyTexts, testing::ElementsAre("Some ", "long", " text"));
  EXPECT_THAT(onPath, testing::ElementsAre(true, true, true));
  const auto it = std::find_if(computed->spans.begin(), computed->spans.end(),
                               [](const auto& span) { return span.text.str() == "long"; });
  ASSERT_NE(it, computed->spans.end());
  EXPECT_TRUE(it->hasExplicitX());
  EXPECT_TRUE(it->hasExplicitY());
  EXPECT_DOUBLE_EQ(it->xList[0]->value, 10.0);
  EXPECT_DOUBLE_EQ(it->yList[0]->value, 20.0);
}

TEST_F(TextSystemTest, TextPathWhitespaceAroundTspansIsPreservedInSpans) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
         viewBox="0 0 200 200">
      <defs>
        <path id="path1" d="M 20 100 C 35 135 85 135 100 100 C 115 65 165 65 180 100"/>
      </defs>
      <text id="t" font-size="24">
        <textPath xlink:href="#path1">
          Some
          <tspan fill="green" dx="10" dy="20">long</tspan>
          <tspan fill="blue" dx="-5" dy="-20">text</tspan>
        </textPath>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();
  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);

  std::vector<std::string> nonEmptyTexts;
  std::vector<bool> onPath;
  for (const auto& span : computed->spans) {
    if (span.text.empty()) {
      continue;
    }
    nonEmptyTexts.push_back(span.text.str());
    onPath.push_back(span.pathSpline.has_value());
  }

  EXPECT_THAT(nonEmptyTexts, testing::ElementsAre("Some ", "long", " ", "text"));
  EXPECT_THAT(onPath, testing::ElementsAre(true, true, true, true));
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
  std::vector<ParseDiagnostic> warnings;
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
      <text id="t" x="10" y="20"><tspan dy="7">A</tspan></text>
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
      <text id="t" x="10" y="20"><tspan>Continuation</tspan><tspan x="50">NewChunk</tspan></text>
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

// --- display:none + rotate ---

TEST_F(TextSystemTest, DisplayNoneConsumesRotateIndices) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" font-size="64">
      <text id="t" x="28" y="100" rotate="10 30 50 70">
        T<tspan display="none">ex</tspan>t
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);

  // Collect non-empty spans and their properties.
  std::vector<std::string> texts;
  std::vector<bool> hiddenFlags;
  std::vector<SmallVector<double, 1>> rotateLists;
  for (const auto& span : computed->spans) {
    if (span.text.empty()) {
      continue;
    }
    texts.push_back(span.text.str());
    hiddenFlags.push_back(span.hidden);
    rotateLists.push_back(span.rotateList);
  }

  EXPECT_THAT(texts, testing::ElementsAre("T", "ex", "t"));
  EXPECT_THAT(hiddenFlags, testing::ElementsAre(false, true, false));

  // "T" is the root's first chunk → gets the full rotate list [10,30,50,70].
  ASSERT_GE(rotateLists[0].size(), 1u);
  EXPECT_DOUBLE_EQ(rotateLists[0][0], 10.0);

  // "ex" is hidden (display:none) — does NOT consume attribute indices.
  // The hidden span has no rotate values (skips character counting entirely).
  EXPECT_TRUE(rotateLists[1].empty());

  // "t" is root continuation at globalCharIndex=1 (hidden chars skipped) → rotate[1]=30.
  ASSERT_EQ(rotateLists[2].size(), 1u);
  EXPECT_DOUBLE_EQ(rotateLists[2][0], 30.0);
}

// --- Whitespace normalization tests ---

// Space after </tspan> must be preserved in the raw textChunks and in the composed text.
TEST_F(TextSystemTest, WhitespaceTspanBoundarySpace) {
  auto document = ParseAndCompute(
      "<svg xmlns='http://www.w3.org/2000/svg'>"
      "<text id='t'>Some <tspan>long</tspan> text</text>"
      "</svg>");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();

  auto& textComp = registry.get<TextComponent>(textEntity);
  EXPECT_EQ(textComp.textChunks.size(), 2u);
  if (textComp.textChunks.size() >= 2) {
    EXPECT_EQ(textComp.textChunks[0].str(), "Some ");
    EXPECT_EQ(textComp.textChunks[1].str(), " text");
  }

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  std::string concatenated;
  for (const auto& span : computed->spans) {
    concatenated += span.text.str();
  }
  EXPECT_EQ(concatenated, "Some long text");
}

// Inter-tspan spacing with indented markup (newlines collapse to spaces).
TEST_F(TextSystemTest, WhitespaceInterTspanSpacing) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="t" x="34" y="100">
        Some
        <tspan>long</tspan>
        text
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();
  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  std::string concatenated;
  for (const auto& span : computed->spans) {
    concatenated += span.text.str();
  }
  EXPECT_EQ(concatenated, "Some long text");
}

// xml:space="preserve" tspan inside default text preserves all spaces.
TEST_F(TextSystemTest, WhitespacePreserveLeadingSpaces) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="t" x="20" y="95">
        <tspan xml:space="preserve">         Text</tspan>
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();
  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  std::string concatenated;
  for (const auto& span : computed->spans) {
    concatenated += span.text.str();
  }
  EXPECT_EQ(concatenated, "         Text");
}

// Mixed xml:space: default root with preserve tspan.
// Per SVG spec: default space following any space (including preserve) is removed.
TEST_F(TextSystemTest, WhitespaceDefaultRootPreserveTspan) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="t" x="20" y="100">
        Text  <tspan xml:space="preserve">  Text  </tspan>  Text
      </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();
  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  std::string concatenated;
  for (const auto& span : computed->spans) {
    concatenated += span.text.str();
  }
  EXPECT_EQ(concatenated, "Text   Text  Text");
}

// Mixed xml:space: preserve root with default tspan.
TEST_F(TextSystemTest, WhitespacePreserveRootDefaultTspan) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <text id="t" x="10" y="100" xml:space="preserve">  Text  <tspan xml:space="default">  Text  </tspan>  Text  </text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();
  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  std::string concatenated;
  for (const auto& span : computed->spans) {
    concatenated += span.text.str();
  }
  EXPECT_EQ(concatenated, "  Text  Text   Text  ");
}

// Gradient-filled tspan preserves spaces at boundaries.
TEST_F(TextSystemTest, WhitespaceGradientTspan) {
  auto document = ParseAndCompute(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <linearGradient id="lg1">
        <stop offset="0" stop-color="white"/>
        <stop offset="1" stop-color="green"/>
      </linearGradient>
      <text id="t" x="20" y="100">
        Some <tspan fill="url(#lg1)">long</tspan> text
      </text>
    </svg>
  )svg");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entityHandle().entity();
  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  std::string concatenated;
  for (const auto& span : computed->spans) {
    concatenated += span.text.str();
  }
  EXPECT_EQ(concatenated, "Some long text");
}

}  // namespace donner::svg::components
