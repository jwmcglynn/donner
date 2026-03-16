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
  auto textEntity = document.querySelector("#t")->entity();

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
  auto textEntity = document.querySelector("#t")->entity();

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
  auto textEntity = document.querySelector("#t")->entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + tspan child span.
  ASSERT_THAT(computed->spans, SizeIs(2));

  // The tspan should inherit x/y from the root <text> element.
  EXPECT_DOUBLE_EQ(computed->spans[1].x.value, 15.0);
  EXPECT_DOUBLE_EQ(computed->spans[1].y.value, 25.0);
}

// --- Per-character positioning ---

TEST_F(TextSystemTest, PerCharacterPositioning) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10 20 30" y="40">ABC</text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  ASSERT_THAT(computed->spans, SizeIs(1));

  auto& span = computed->spans[0];
  ASSERT_EQ(span.xList.size(), 3u);
  EXPECT_TRUE(span.xList[0].has_value());
  EXPECT_TRUE(span.xList[1].has_value());
  EXPECT_TRUE(span.xList[2].has_value());
  EXPECT_DOUBLE_EQ(span.xList[0]->value, 10.0);
  EXPECT_DOUBLE_EQ(span.xList[1]->value, 20.0);
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
  auto textEntity = document.querySelector("#t")->entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + tspan child span.
  ASSERT_THAT(computed->spans, SizeIs(2));

  auto& span = computed->spans[1];
  ASSERT_EQ(span.dxList.size(), 2u);
  ASSERT_EQ(span.dyList.size(), 2u);
  EXPECT_TRUE(span.dxList[0].has_value());
  EXPECT_TRUE(span.dxList[1].has_value());
  EXPECT_DOUBLE_EQ(span.dxList[0]->value, 5.0);
  EXPECT_DOUBLE_EQ(span.dxList[1]->value, 10.0);
  EXPECT_DOUBLE_EQ(span.dyList[0]->value, 2.0);
  EXPECT_DOUBLE_EQ(span.dyList[1]->value, 4.0);
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
  auto textEntity = document.querySelector("#t")->entity();

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
  auto textEntity = document.querySelector("#t")->entity();

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
  auto textEntity = document.querySelector("#t")->entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + 3 tspan children.
  ASSERT_THAT(computed->spans, SizeIs(4));

  EXPECT_DOUBLE_EQ(computed->spans[1].x.value, 10.0);
  EXPECT_DOUBLE_EQ(computed->spans[1].y.value, 20.0);
  EXPECT_DOUBLE_EQ(computed->spans[2].x.value, 30.0);
  EXPECT_DOUBLE_EQ(computed->spans[2].y.value, 40.0);
  EXPECT_DOUBLE_EQ(computed->spans[3].x.value, 50.0);
  EXPECT_DOUBLE_EQ(computed->spans[3].y.value, 60.0);
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
  auto textEntity = document.querySelector("#t")->entity();

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
  auto textEntity = document.querySelector("#t")->entity();

  auto* computed = registry.try_get<ComputedTextComponent>(textEntity);
  ASSERT_NE(computed, nullptr);
  // Root span + textPath child span.
  ASSERT_THAT(computed->spans, SizeIs(2));

  // 50% of a 100-unit path should be ~50.
  EXPECT_TRUE(computed->spans[1].pathSpline.has_value());
  EXPECT_NEAR(computed->spans[1].pathStartOffset, 50.0, 1.0);
}

// --- UTF-8 multibyte characters ---

TEST_F(TextSystemTest, Utf8MultibyteCounting) {
  auto document = ParseAndCompute(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10 20 30">&#x4F60;&#x597D;!</text>
    </svg>
  )");

  auto& registry = document.registry();
  auto textEntity = document.querySelector("#t")->entity();

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
  )", nullptr, options);
  ASSERT_TRUE(maybeResult.hasResult());
  auto document = std::move(maybeResult).result();

  auto& registry = document.registry();
  std::vector<ParseError> warnings;
  StyleSystem().computeAllStyles(registry, &warnings);
  TextSystem().instantiateAllComputedComponents(registry, &warnings);

  EXPECT_THAT(warnings, IsEmpty());
}

}  // namespace donner::svg::components
