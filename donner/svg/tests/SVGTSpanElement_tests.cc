#include "donner/svg/SVGTSpanElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
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

// Tests for <tspan> element parsing and attributes.
TEST(SVGTSpanElementTests, CreateAndCast) {
  auto tspan = instantiateSubtreeElementAs<SVGTSpanElement>("<tspan />", kExperimentalOptions);
  EXPECT_THAT(tspan->tryCast<SVGTextPositioningElement>(), Ne(std::nullopt));
  EXPECT_THAT(tspan->tryCast<SVGTextContentElement>(), Ne(std::nullopt));
  EXPECT_THAT(tspan->tryCast<SVGGraphicsElement>(), Ne(std::nullopt));
  EXPECT_THAT(tspan->tryCast<SVGTSpanElement>(), Ne(std::nullopt));
}

TEST(SVGTSpanElementTests, DisabledWithoutExperimental) {
  auto tspan = instantiateSubtreeElement("<tspan />");
  EXPECT_THAT(tspan->tryCast<SVGTSpanElement>(), Eq(std::nullopt));
}

TEST(SVGTSpanElementTests, Defaults) {
  auto tspan = instantiateSubtreeElementAs<SVGTSpanElement>("<tspan />", kExperimentalOptions);
  EXPECT_THAT(tspan->x(), testing::Eq(std::nullopt));
  EXPECT_THAT(tspan->y(), testing::Eq(std::nullopt));
  EXPECT_THAT(tspan->dx(), testing::Eq(std::nullopt));
  EXPECT_THAT(tspan->dy(), testing::Eq(std::nullopt));
  EXPECT_THAT(tspan->rotate(), testing::Eq(std::nullopt));
}

TEST(SVGTSpanElementTests, PositionAttributes) {
  auto tspan = instantiateSubtreeElementAs<SVGTSpanElement>(
      R"(<tspan x="1" y="2" dx="3" dy="4" rotate="30" />)", kExperimentalOptions);
  EXPECT_THAT(tspan->x(), Optional(LengthIs(1.0, Lengthd::Unit::None)));
  EXPECT_THAT(tspan->y(), Optional(LengthIs(2.0, Lengthd::Unit::None)));
  EXPECT_THAT(tspan->dx(), Optional(LengthIs(3.0, Lengthd::Unit::None)));
  EXPECT_THAT(tspan->dy(), Optional(LengthIs(4.0, Lengthd::Unit::None)));
  EXPECT_THAT(tspan->rotate(), Optional(testing::DoubleNear(30.0, 1e-6)));
}

/// @test Test that text content is read from child text nodes.
TEST(SVGTSpanElementTests, TextContentNodes) {
  auto tspan =
      instantiateSubtreeElementAs<SVGTSpanElement>("<tspan>contents</tspan>", kExperimentalOptions);
  EXPECT_THAT(tspan->textContent(), ToStringIs("contents"));
}

/// @test Test empty text content.
TEST(SVGTSpanElementTests, TextContentEmpty) {
  auto tspan =
      instantiateSubtreeElementAs<SVGTSpanElement>("<tspan></tspan>", kExperimentalOptions);
  EXPECT_THAT(tspan->textContent(), ToStringIs(""));
}

/// @test Test text content with leading/trailing/internal whitespace.
TEST(SVGTSpanElementTests, TextContentWhitespace) {
  auto tspan = instantiateSubtreeElementAs<SVGTSpanElement>(
      "<tspan>  leading and trailing  </tspan>", kExperimentalOptions);
  EXPECT_THAT(tspan->textContent(), ToStringIs("  leading and trailing  "));

  tspan = instantiateSubtreeElementAs<SVGTSpanElement>("<tspan>internal  whitespace</tspan>",
                                                       kExperimentalOptions);
  EXPECT_THAT(tspan->textContent(), ToStringIs("internal  whitespace"));
}

/// @test Test text content within a CDATA section.
TEST(SVGTSpanElementTests, TextContentCData) {
  auto tspan = instantiateSubtreeElementAs<SVGTSpanElement>(
      "<tspan><![CDATA[CDATA content]]></tspan>", kExperimentalOptions);
  EXPECT_THAT(tspan->textContent(), ToStringIs("CDATA content"));
}

/// @test Test multiple adjacent text nodes.
TEST(SVGTSpanElementTests, TextContentMultipleNodes) {
  // Note: The parser implicitly concatenates adjacent text nodes.
  auto tspan = instantiateSubtreeElementAs<SVGTSpanElement>(
      "<tspan>Part1<!-- comment -->Part2</tspan>", kExperimentalOptions);
  EXPECT_THAT(tspan->textContent(), ToStringIs("Part1Part2"));
}

}  // namespace donner::svg
