#include "donner/svg/SVGTextElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/SVGTSpanElement.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::Eq;
using testing::Ne;
using testing::Optional;

namespace donner::svg {

namespace {

constexpr parser::SVGParser::Options kExperimentalOptions = []() {
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  return options;
}();

}  // namespace

// Tests for <text> element parsing and attributes.
TEST(SVGTextElementTests, CreateAndCast) {
  auto text = instantiateSubtreeElementAs<SVGTextElement>("<text />", kExperimentalOptions);
  // Cast to base classes
  EXPECT_THAT(text->tryCast<SVGTextContentElement>(), Ne(std::nullopt));
  EXPECT_THAT(text->tryCast<SVGTextPositioningElement>(), Ne(std::nullopt));
  EXPECT_THAT(text->tryCast<SVGGraphicsElement>(), Ne(std::nullopt));
  EXPECT_THAT(text->tryCast<SVGTextElement>(), Ne(std::nullopt));
}

TEST(SVGTextElementTests, EnabledWithoutExperimental) {
  auto text = instantiateSubtreeElement("<text />");
  EXPECT_THAT(text->tryCast<SVGTextElement>(), Ne(std::nullopt));
}

TEST(SVGTextElementTests, Defaults) {
  auto text = instantiateSubtreeElementAs<SVGTextElement>("<text />", kExperimentalOptions);
  // Default lengthAdjust is Spacing
  EXPECT_THAT(text->lengthAdjust(), testing::Eq(LengthAdjust::Spacing));
  // Default textLength is unset
  EXPECT_THAT(text->textLength(), testing::Eq(std::nullopt));
  // Default positioning lists empty
  EXPECT_THAT(text->x(), testing::Eq(std::nullopt));
  EXPECT_THAT(text->y(), testing::Eq(std::nullopt));
  EXPECT_THAT(text->dx(), testing::Eq(std::nullopt));
  EXPECT_THAT(text->dy(), testing::Eq(std::nullopt));
  EXPECT_THAT(text->rotate(), testing::Eq(std::nullopt));
}

TEST(SVGTextElementTests, PositionAttributes) {
  auto text = instantiateSubtreeElementAs<SVGTextElement>(
      R"(<text x="10 20" y="5 15" dx="1 2" dy="3 4" rotate="0 45 90" />)", kExperimentalOptions);
  // Check first values
  EXPECT_THAT(text->x(), Optional(LengthIs(10.0, Lengthd::Unit::None)));
  EXPECT_THAT(text->y(), Optional(LengthIs(5.0, Lengthd::Unit::None)));
  EXPECT_THAT(text->dx(), Optional(LengthIs(1.0, Lengthd::Unit::None)));
  EXPECT_THAT(text->dy(), Optional(LengthIs(3.0, Lengthd::Unit::None)));
  EXPECT_THAT(text->rotate(), Optional(testing::DoubleNear(0.0, 1e-6)));

  // Check lists
  EXPECT_THAT(text->xList(), testing::ElementsAre(LengthIs(10.0, Lengthd::Unit::None),
                                                  LengthIs(20.0, Lengthd::Unit::None)));
  EXPECT_THAT(text->rotateList(), testing::ElementsAre(0.0, 45.0, 90.0));
}

TEST(SVGTextElementTests, TextLengthAndAdjust) {
  auto text = instantiateSubtreeElementAs<SVGTextElement>(
      R"(<text textLength="100" lengthAdjust="spacingAndGlyphs" />)", kExperimentalOptions);
  EXPECT_THAT(text->textLength(), Optional(LengthIs(100.0, Lengthd::Unit::None)));
  EXPECT_THAT(text->lengthAdjust(), testing::Eq(LengthAdjust::SpacingAndGlyphs));
}

/// @test Test that text content is read from child text nodes.
TEST(SVGTextElementTests, TextContentNodes) {
  auto text =
      instantiateSubtreeElementAs<SVGTextElement>("<text>contents</text>", kExperimentalOptions);
  EXPECT_THAT(text->textContent(), ToStringIs("contents"));
}

/// @test Test empty text content.
TEST(SVGTextElementTests, TextContentEmpty) {
  auto text = instantiateSubtreeElementAs<SVGTextElement>("<text></text>", kExperimentalOptions);
  EXPECT_THAT(text->textContent(), ToStringIs(""));
}

/// @test Test text content with leading/trailing/internal whitespace.
TEST(SVGTextElementTests, TextContentWhitespace) {
  auto text = instantiateSubtreeElementAs<SVGTextElement>("<text>  leading and trailing  </text>",
                                                          kExperimentalOptions);
  EXPECT_THAT(text->textContent(), ToStringIs("  leading and trailing  "));

  text = instantiateSubtreeElementAs<SVGTextElement>("<text>internal  whitespace</text>",
                                                     kExperimentalOptions);
  EXPECT_THAT(text->textContent(), ToStringIs("internal  whitespace"));
}

/// @test Test text content within a CDATA section.
TEST(SVGTextElementTests, TextContentCData) {
  auto text = instantiateSubtreeElementAs<SVGTextElement>("<text><![CDATA[CDATA content]]></text>",
                                                          kExperimentalOptions);
  EXPECT_THAT(text->textContent(), ToStringIs("CDATA content"));
}

/// @test Test multiple adjacent text nodes.
TEST(SVGTextElementTests, TextContentMultipleNodes) {
  // Note: The parser implicitly concatenates adjacent text nodes.
  auto text = instantiateSubtreeElementAs<SVGTextElement>("<text>Part1<!-- comment -->Part2</text>",
                                                          kExperimentalOptions);
  EXPECT_THAT(text->textContent(), ToStringIs("Part1Part2"));
}

// Simple rendering test for a single-letter <text> element
TEST(SVGTextElementViewportTests, SimpleLetter) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::Text)) {
    GTEST_SKIP() << "Active renderer does not support text rendering";
  }

  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 16 16">
      <text x="5" y="12" font-family="fallback-font" font-size="12px" fill="black">T</text>
    </svg>
  )-",
                                       kExperimentalOptions);

  // Both TinySkia and Skia use TextEngine glyph outlines, producing the same
  // output per platform. macOS produces non-AA output, Linux produces AA edges.
  // HarfBuzz/FreeType (text-full) produces a slightly thicker T crossbar than
  // stb_truetype (simple text).
  if (ActiveRendererSupportsFeature(RendererBackendFeature::TextFull)) {
    EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(doc).matchBackend()
        .defaultPattern(R"(
        ................
        ................
        ................
        .....@@@@@@@....
        .....@@@@@@@....
        ........@.......
        ........@.......
        ........@.......
        ........@.......
        ........@.......
        ........@.......
        ........@.......
        ................
        ................
        ................
        ................
    )"));
  } else {
    EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(doc).matchBackend()
        .defaultPattern(R"(
        ................
        ................
        ................
        .....@@@@@@@....
        ........@.......
        ........@.......
        ........@.......
        ........@.......
        ........@.......
        ........@.......
        ........@.......
        ........@.......
        ................
        ................
        ................
        ................
    )"));
  }
}

// Verify that a <text> element with multiple <tspan> children generates multiple
// computed text spans.
TEST(SVGTextElementViewportTests, MultipleTSpansComputed) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::Text)) {
    GTEST_SKIP() << "Active renderer does not support text rendering";
  }

  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 16 16">
      <text id="root" x="0" y="12" font-family="fallback-font" font-size="12px" fill="black">
        <tspan>A</tspan><tspan dx="2">B</tspan>
      </text>
    </svg>
  )-",
                                       kExperimentalOptions);

  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(doc).matchBackend()
      .defaultPattern(R"(
      ................
      ................
      ................
      ....@.......@@@@
      ...@@.......@...
      ...@.@......@...
      ..@@.@......@...
      ..@..@......@@@@
      ..@...@.....@...
      .@@@@@@.....@...
      .@....@@....@...
      .@.....@....@@@@
      ................
      ................
      ................
      ................
  )"));
}

TEST(SVGTextElementPublicApiTests, TextGeometryApisReturnComputedValues) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 120 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px">ABC</text>
    </svg>
  )-",
                                       kExperimentalOptions);

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();

  EXPECT_EQ(textElement.getNumberOfChars(), 3);
  EXPECT_NEAR(textElement.getComputedTextLength(), 25.0, 0.05);
  EXPECT_NEAR(textElement.getSubStringLength(0, 2), 16.66, 0.05);

  const Vector2d start = textElement.getStartPositionOfChar(0);
  EXPECT_NEAR(start.x, 10.0, 0.5);
  EXPECT_NEAR(start.y, 20.0, 0.5);
  const Vector2d end = textElement.getEndPositionOfChar(0);
  EXPECT_GE(end.x, start.x);
  EXPECT_NEAR(end.y, start.y, 1.0);

  const Box2d extent = textElement.getExtentOfChar(0);
  EXPECT_THAT(extent, BoxEq(Vector2Near(10.4531, 11.3281), Vector2Near(18.125, 20.0)));
  EXPECT_EQ(textElement.getCharNumAtPosition((extent.topLeft + extent.bottomRight) * 0.5), 0);

  const std::vector<Path> paths = textElement.convertToPath();
  EXPECT_FALSE(paths.empty());
  const Box2d inkBounds = textElement.inkBoundingBox();
  if (ActiveRendererSupportsFeature(RendererBackendFeature::TextFull)) {
    // HarfBuzz v14 + FreeType produces slightly wider glyph advances.
    EXPECT_THAT(inkBounds, BoxEq(Vector2Near(10.45, 11.20), Vector2Near(34.375, 20.125)));
  } else {
    EXPECT_THAT(inkBounds, BoxEq(Vector2Near(10.45, 11.204), Vector2Near(34.36, 20.12)));
  }

  const Box2d objectBounds = textElement.objectBoundingBox();
  if (ActiveRendererSupportsFeature(RendererBackendFeature::TextFull)) {
    EXPECT_THAT(objectBounds, BoxEq(Vector2Near(10.0, 8.6), Vector2Near(35.0, 22.7)));
  } else {
    EXPECT_THAT(objectBounds, BoxEq(Vector2Near(10.0, 8.6), Vector2Near(34.984, 22.7)));
  }
}

TEST(SVGTextElementPublicApiTests, AppendTextAndAdvanceTextChunkUpdateTextContent) {
  SVGDocument document;
  SVGTextElement text = SVGTextElement::Create(document);

  EXPECT_EQ(text.textContent(), "");
  text.advanceTextChunk();
  text.appendText("Hello");
  EXPECT_EQ(text.textContent(), "Hello");
  text.advanceTextChunk();
  text.appendText("World");
  EXPECT_EQ(text.textContent(), "HelloWorld");
}

TEST(SVGTextElementPublicApiTests, DetachedElementMutatorsDoNotRequireTree) {
  SVGDocument document;
  SVGTextElement text = SVGTextElement::Create(document);

  text.setTextLength(Lengthd(10.0, Lengthd::Unit::Px));
  text.setLengthAdjust(LengthAdjust::SpacingAndGlyphs);

  EXPECT_THAT(text.textLength(), Optional(LengthIs(10.0, Lengthd::Unit::Px)));
  EXPECT_EQ(text.lengthAdjust(), LengthAdjust::SpacingAndGlyphs);
}

TEST(SVGTextElementPublicApiTests, TspanMutatorsHandleMissingTreeComponent) {
  SVGDocument document;
  SVGTSpanElement text = SVGTSpanElement::Create(document);
  text.entityHandle().remove<::donner::components::TreeComponent>();

  text.setTextLength(Lengthd(12.0, Lengthd::Unit::Px));
  text.setLengthAdjust(LengthAdjust::SpacingAndGlyphs);

  EXPECT_THAT(text.textLength(), Optional(LengthIs(12.0, Lengthd::Unit::Px)));
  EXPECT_EQ(text.lengthAdjust(), LengthAdjust::SpacingAndGlyphs);
}

TEST(SVGTextElementPublicApiTests, EndPositionOfCharReturnsComputedValue) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 120 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px">ABC</text>
    </svg>
  )-",
                                       kExperimentalOptions);

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();
  const Vector2d start = textElement.getStartPositionOfChar(0);
  const Vector2d end = textElement.getEndPositionOfChar(0);
  EXPECT_GT(end.x, start.x);
  EXPECT_NEAR(end.y, start.y, 1.0);
}

TEST(SVGTextElementPublicApiTests, SelectSubstringIsNoOp) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 120 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px">ABC</text>
    </svg>
  )-",
                                       kExperimentalOptions);

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();
  const double before = textElement.getComputedTextLength();
  textElement.selectSubString(0, 2);
  const double after = textElement.getComputedTextLength();
  EXPECT_DOUBLE_EQ(before, after);
}

TEST(SVGTextElementPublicApiTests, TspanApisFilterToOwnSubtree) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 120 40">
      <text id="root" x="10" y="20" font-family="fallback-font" font-size="12px">
        A<tspan id="span" dx="6" rotate="30">BC</tspan>
      </text>
    </svg>
  )-",
                                       kExperimentalOptions);

  auto root = doc.querySelector("#root")->cast<SVGTextElement>();
  auto span = doc.querySelector("#span")->cast<SVGTSpanElement>();

  EXPECT_EQ(root.getNumberOfChars(), 3);
  EXPECT_EQ(span.getNumberOfChars(), 2);
  EXPECT_NEAR(span.getComputedTextLength(), root.getSubStringLength(1, 2), 1e-6);
  EXPECT_EQ(span.getExtentOfChar(0), root.getExtentOfChar(1));
  EXPECT_DOUBLE_EQ(span.getRotationOfChar(0), 30.0);
  EXPECT_EQ(span.getStartPositionOfChar(0), root.getStartPositionOfChar(1));
}

// ── Cache invalidation tests ────────────────────────────────────────────────

TEST(SVGTextElementCacheTests, GeometryIsCachedAcrossQueries) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 120 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px">ABC</text>
    </svg>
  )-",
                                       kExperimentalOptions);

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();

  // First query populates cache.
  const double len1 = textElement.getComputedTextLength();
  // Second query should return same result from cache.
  const double len2 = textElement.getComputedTextLength();
  EXPECT_DOUBLE_EQ(len1, len2);
}

TEST(SVGTextElementCacheTests, SetTextLengthInvalidatesCache) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 200 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px">ABCDEF</text>
    </svg>
  )-",
                                       kExperimentalOptions);

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();

  // textLength affects glyph positions, so use start position of last char.
  const Vector2d posBefore = textElement.getStartPositionOfChar(5);

  // Setting textLength should invalidate the cache and change glyph positions.
  textElement.setTextLength(Lengthd(200.0, Lengthd::Unit::Px));
  const Vector2d posAfter = textElement.getStartPositionOfChar(5);
  // The last character should have moved significantly.
  EXPECT_GT(std::abs(posAfter.x - posBefore.x), 10.0);
}

TEST(SVGTextElementCacheTests, SetPositionInvalidatesCache) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 200 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px">AB</text>
    </svg>
  )-",
                                       kExperimentalOptions);

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();

  const Vector2d startBefore = textElement.getStartPositionOfChar(0);
  EXPECT_NEAR(startBefore.x, 10.0, 0.5);

  // Change X position.
  textElement.setX(Lengthd(50.0, Lengthd::Unit::Px));
  const Vector2d startAfter = textElement.getStartPositionOfChar(0);
  EXPECT_NEAR(startAfter.x, 50.0, 0.5);
}

TEST(SVGTextElementCacheTests, TSpanPositionChangeInvalidatesParent) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 200 40">
      <text id="root" x="10" y="20" font-family="fallback-font" font-size="12px">
        A<tspan id="span">B</tspan>
      </text>
    </svg>
  )-",
                                       kExperimentalOptions);

  auto span = doc.querySelector("#span")->cast<SVGTSpanElement>();

  // Prime the cache.
  const Vector2d spanStartBefore = span.getStartPositionOfChar(0);

  // Change dx on the tspan.
  span.setDx(Lengthd(20.0, Lengthd::Unit::Px));
  const Vector2d spanStartAfter = span.getStartPositionOfChar(0);
  // The span's character should have moved by ~20px.
  EXPECT_NEAR(spanStartAfter.x - spanStartBefore.x, 20.0, 1.0);
}

// ── getRotationOfChar tests ─────────────────────────────────────────────────

TEST(SVGTextElementPublicApiTests, GetRotationOfCharWithRotateAttribute) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 200 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px"
            rotate="0 45 90">ABC</text>
    </svg>
  )-");

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();

  EXPECT_DOUBLE_EQ(textElement.getRotationOfChar(0), 0.0);
  EXPECT_DOUBLE_EQ(textElement.getRotationOfChar(1), 45.0);
  EXPECT_DOUBLE_EQ(textElement.getRotationOfChar(2), 90.0);
}

TEST(SVGTextElementPublicApiTests, GetRotationOfCharLastValueRepeats) {
  // Per SVG spec, the last rotate value is reused for remaining characters.
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 200 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px"
            rotate="15">ABCD</text>
    </svg>
  )-");

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();

  // All characters should get the single rotate value of 15.
  EXPECT_DOUBLE_EQ(textElement.getRotationOfChar(0), 15.0);
  EXPECT_DOUBLE_EQ(textElement.getRotationOfChar(1), 15.0);
  EXPECT_DOUBLE_EQ(textElement.getRotationOfChar(2), 15.0);
  EXPECT_DOUBLE_EQ(textElement.getRotationOfChar(3), 15.0);
}

// ── Word-spacing tests ──────────────────────────────────────────────────────

TEST(SVGTextElementPublicApiTests, WordSpacingAffectsCharacterPositions) {
  SVGDocument docNoSpacing = instantiateSubtree(R"-(
    <svg viewBox="0 0 300 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px">Hi World</text>
    </svg>
  )-");

  SVGDocument docWithSpacing = instantiateSubtree(R"-(
    <svg viewBox="0 0 300 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px"
            style="word-spacing: 20px">Hi World</text>
    </svg>
  )-");

  auto textNoSpacing = docNoSpacing.querySelector("#t")->cast<SVGTextElement>();
  auto textWithSpacing = docWithSpacing.querySelector("#t")->cast<SVGTextElement>();

  // The character after the space ("W" at index 3) should be shifted right by the word-spacing.
  const double posWithout = textNoSpacing.getStartPositionOfChar(3).x;
  const double posWith = textWithSpacing.getStartPositionOfChar(3).x;

  // Word-spacing of 20px should shift characters after the space by ~20px.
  EXPECT_NEAR(posWith - posWithout, 20.0, 1.0);
}

// ── Letter-spacing tests ────────────────────────────────────────────────────

TEST(SVGTextElementPublicApiTests, LetterSpacingAffectsCharacterPositions) {
  SVGDocument docNoSpacing = instantiateSubtree(R"-(
    <svg viewBox="0 0 300 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px">ABC</text>
    </svg>
  )-");

  SVGDocument docWithSpacing = instantiateSubtree(R"-(
    <svg viewBox="0 0 300 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px"
            style="letter-spacing: 10px">ABC</text>
    </svg>
  )-");

  auto textNoSpacing = docNoSpacing.querySelector("#t")->cast<SVGTextElement>();
  auto textWithSpacing = docWithSpacing.querySelector("#t")->cast<SVGTextElement>();

  // The third character 'C' (index 2) should be shifted right by letter-spacing * 2 (two gaps).
  const double posWithout = textNoSpacing.getStartPositionOfChar(2).x;
  const double posWith = textWithSpacing.getStartPositionOfChar(2).x;

  // Letter-spacing of 10px applied twice (A->B and B->C) should shift C by ~20px.
  EXPECT_NEAR(posWith - posWithout, 20.0, 1.0);
}

// ── Text-anchor tests ───────────────────────────────────────────────────────

TEST(SVGTextElementPublicApiTests, TextAnchorMiddleShiftsStartPosition) {
  SVGDocument docStart = instantiateSubtree(R"-(
    <svg viewBox="0 0 300 40">
      <text id="t" x="100" y="20" font-family="fallback-font" font-size="12px"
            text-anchor="start">ABC</text>
    </svg>
  )-");

  SVGDocument docMiddle = instantiateSubtree(R"-(
    <svg viewBox="0 0 300 40">
      <text id="t" x="100" y="20" font-family="fallback-font" font-size="12px"
            text-anchor="middle">ABC</text>
    </svg>
  )-");

  auto textStart = docStart.querySelector("#t")->cast<SVGTextElement>();
  auto textMiddle = docMiddle.querySelector("#t")->cast<SVGTextElement>();

  const double startX = textStart.getStartPositionOfChar(0).x;
  const double middleX = textMiddle.getStartPositionOfChar(0).x;

  // With text-anchor:middle, the text is shifted left by half the text length.
  const double textLen = textStart.getComputedTextLength();
  EXPECT_NEAR(startX - middleX, textLen / 2.0, 1.0);
}

TEST(SVGTextElementPublicApiTests, TextAnchorEndShiftsStartPosition) {
  SVGDocument docStart = instantiateSubtree(R"-(
    <svg viewBox="0 0 300 40">
      <text id="t" x="100" y="20" font-family="fallback-font" font-size="12px"
            text-anchor="start">ABC</text>
    </svg>
  )-");

  SVGDocument docEnd = instantiateSubtree(R"-(
    <svg viewBox="0 0 300 40">
      <text id="t" x="100" y="20" font-family="fallback-font" font-size="12px"
            text-anchor="end">ABC</text>
    </svg>
  )-");

  auto textStart = docStart.querySelector("#t")->cast<SVGTextElement>();
  auto textEnd = docEnd.querySelector("#t")->cast<SVGTextElement>();

  const double startX = textStart.getStartPositionOfChar(0).x;
  const double endX = textEnd.getStartPositionOfChar(0).x;

  // With text-anchor:end, the text is shifted left by the full text length.
  const double textLen = textStart.getComputedTextLength();
  EXPECT_NEAR(startX - endX, textLen, 1.0);
}

// ── Multiple tspan positioning tests ────────────────────────────────────────

TEST(SVGTextElementPublicApiTests, MultipleTSpanPerCharacterPositioning) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 300 40">
      <text id="root" x="10 30" y="20" font-family="fallback-font" font-size="12px">
        A<tspan id="span" x="50 70">BC</tspan>
      </text>
    </svg>
  )-");

  auto root = doc.querySelector("#root")->cast<SVGTextElement>();

  // Character 'A' should be at x=10.
  EXPECT_NEAR(root.getStartPositionOfChar(0).x, 10.0, 0.5);

  // Character 'B' (first in tspan) should be at x=50 (tspan's own x overrides).
  EXPECT_NEAR(root.getStartPositionOfChar(1).x, 50.0, 0.5);

  // Character 'C' (second in tspan) should be at x=70.
  EXPECT_NEAR(root.getStartPositionOfChar(2).x, 70.0, 0.5);
}

// ── convertToPath tests ─────────────────────────────────────────────────────

TEST(SVGTextElementPublicApiTests, ConvertToPathReturnsPathPerGlyph) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 200 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px">ABCD</text>
    </svg>
  )-");

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();

  const std::vector<Path> paths = textElement.convertToPath();
  // Each character should produce one path.
  EXPECT_EQ(static_cast<long>(paths.size()), textElement.getNumberOfChars());

  // Each path should be non-empty (all of A, B, C, D have outlines).
  for (const auto& path : paths) {
    EXPECT_FALSE(path.empty());
  }
}

// ── objectBoundingBox vs inkBoundingBox tests ───────────────────────────────

TEST(SVGTextElementPublicApiTests, ObjectBoundingBoxDiffersFromInkBoundingBox) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 200 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px">Tg</text>
    </svg>
  )-");

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();

  const Box2d inkBounds = textElement.inkBoundingBox();
  const Box2d objectBounds = textElement.objectBoundingBox();

  // Both should be valid (non-empty) boxes.
  EXPECT_GT(inkBounds.width(), 0.0);
  EXPECT_GT(inkBounds.height(), 0.0);
  EXPECT_GT(objectBounds.width(), 0.0);
  EXPECT_GT(objectBounds.height(), 0.0);

  // objectBoundingBox accounts for line-height and baseline, so it should typically
  // be taller than the ink bounds. At minimum they should differ in some dimension.
  EXPECT_NE(inkBounds, objectBounds);
}

// ── getCharNumAtPosition tests ──────────────────────────────────────────────

TEST(SVGTextElementPublicApiTests, GetCharNumAtPositionFindsCorrectChar) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 200 40">
      <text id="t" x="10" y="20" font-family="fallback-font" font-size="12px">ABC</text>
    </svg>
  )-");

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();

  // Use the center of each character's extent to verify lookup.
  for (long i = 0; i < textElement.getNumberOfChars(); ++i) {
    const Box2d extent = textElement.getExtentOfChar(static_cast<std::size_t>(i));
    const Vector2d center = (extent.topLeft + extent.bottomRight) * 0.5;
    EXPECT_EQ(textElement.getCharNumAtPosition(center), i) << "Failed for character index " << i;
  }
}

TEST(SVGTextElementPublicApiTests, GetCharNumAtPositionReturnsNegativeForMiss) {
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 200 40">
      <text id="t" x="100" y="20" font-family="fallback-font" font-size="12px">A</text>
    </svg>
  )-");

  auto textElement = doc.querySelector("#t")->cast<SVGTextElement>();

  // A point far away from the text should return -1.
  EXPECT_EQ(textElement.getCharNumAtPosition(Vector2d(0.0, 0.0)), -1);
}

}  // namespace donner::svg
