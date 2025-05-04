#include "donner/svg/SVGTextElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
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

TEST(SVGTextElementTests, DisabledWithoutExperimental) {
  auto text = instantiateSubtreeElement("<text />");
  EXPECT_THAT(text->tryCast<SVGTextElement>(), Eq(std::nullopt));
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
  // Render a single character 'I' in white on black background
  // Create a full SVG document for rendering
  SVGDocument doc = instantiateSubtree(R"-(
    <svg viewBox="0 0 16 16">
      <text x="5" y="12" font-family="fallback-font" font-size="12px" fill="white">T</text>
    </svg>
  )-",
                                       kExperimentalOptions);

  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(doc).matches(R"(
      ................
      ................
      ................
      .....+******....
      .....-++@*+=....
      ........@-......
      ........@-......
      ........@-......
      ........@-......
      ........@-......
      ........@-......
      ........@-......
      ................
      ................
      ................
      ................
  )"));
}

}  // namespace donner::svg
