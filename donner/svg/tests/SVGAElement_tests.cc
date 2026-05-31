#include "donner/svg/SVGAElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::Eq;
using testing::Ne;
using testing::Optional;

namespace donner::svg {

// Tests for the <a> hyperlink element, which is a transparent grouping/text-content container.
TEST(SVGAElementTests, CreateAndCast) {
  auto a = instantiateSubtreeElementAs<SVGAElement>("<a />");
  EXPECT_THAT(a->tryCast<SVGAElement>(), Ne(std::nullopt));
  // `<a>` is a text-content group (like <tspan>) so it must cast to the text interfaces, which is
  // what lets the text layout descend into its children.
  EXPECT_THAT(a->tryCast<SVGTextPositioningElement>(), Ne(std::nullopt));
  EXPECT_THAT(a->tryCast<SVGTextContentElement>(), Ne(std::nullopt));
  // It is also a general grouping element (like <g>).
  EXPECT_THAT(a->tryCast<SVGGraphicsElement>(), Ne(std::nullopt));
}

TEST(SVGAElementTests, EnabledWithoutExperimental) {
  auto a = instantiateSubtreeElement("<a />");
  EXPECT_THAT(a->tryCast<SVGAElement>(), Ne(std::nullopt));
}

TEST(SVGAElementTests, HrefDefaultsToNullopt) {
  auto a = instantiateSubtreeElementAs<SVGAElement>("<a />");
  EXPECT_THAT(a->href(), Eq(std::nullopt));
}

TEST(SVGAElementTests, ParsesHref) {
  auto a = instantiateSubtreeElementAs<SVGAElement>(R"(<a href="https://example.com/" />)");
  EXPECT_THAT(a->href(), Optional(RcString("https://example.com/")));
}

TEST(SVGAElementTests, ParsesXlinkHref) {
  auto a = instantiateSubtreeElementAs<SVGAElement>(
      R"(<a xmlns:xlink="http://www.w3.org/1999/xlink" xlink:href="#target" />)");
  EXPECT_THAT(a->href(), Optional(RcString("#target")));
}

TEST(SVGAElementTests, SetHref) {
  auto a = instantiateSubtreeElementAs<SVGAElement>("<a />");
  a->setHref(RcString("#anchor"));
  EXPECT_THAT(a->href(), Optional(RcString("#anchor")));

  a->setHref(std::nullopt);
  EXPECT_THAT(a->href(), Eq(std::nullopt));
}

/// @test `<a>` captures direct text content like <tspan>, so text children participate in layout.
TEST(SVGAElementTests, TextContentNodes) {
  auto a = instantiateSubtreeElementAs<SVGAElement>("<a>linked text</a>");
  EXPECT_THAT(a->textContent(), ToStringIs("linked text"));
}

/// @test `<a>` supports the per-glyph positioning attributes inherited from <tspan>'s base.
TEST(SVGAElementTests, PositionAttributes) {
  auto a =
      instantiateSubtreeElementAs<SVGAElement>(R"(<a x="1" y="2" dx="3" dy="4" rotate="30" />)");
  EXPECT_THAT(a->x(), Optional(LengthIs(1.0, Lengthd::Unit::None)));
  EXPECT_THAT(a->y(), Optional(LengthIs(2.0, Lengthd::Unit::None)));
  EXPECT_THAT(a->dx(), Optional(LengthIs(3.0, Lengthd::Unit::None)));
  EXPECT_THAT(a->dy(), Optional(LengthIs(4.0, Lengthd::Unit::None)));
  EXPECT_THAT(a->rotate(), Optional(testing::DoubleNear(30.0, 1e-6)));
}

}  // namespace donner::svg
