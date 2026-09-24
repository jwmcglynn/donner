#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::Optional;

namespace donner::svg {

using css::Color;
using css::RGBA;

TEST(ElementStyleTests, Attributes) {
  EXPECT_THAT(instantiateSubtreeElement(R"(
      <rect fill="red" />
    )")
                  ->getComputedStyle(),
              ToStringIs(R"(PropertyRegistry {
  transform-origin: 0 0 (set) @ Specificity(0, 0, 1)
  fill: PaintServer(solid rgba(255, 0, 0, 255)) (set) @ Specificity(0, 0, 0)
}
)"));

  EXPECT_THAT(instantiateSubtreeElement(R"(
      <rect fill="red" />
    )")
                  ->getComputedStyle()
                  .fill.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0, 0, 0xFF))))));
}

TEST(ElementStyleTests, Style) {
  EXPECT_THAT(instantiateSubtreeElement(R"(
      <rect style="stroke: blue" />
    )")
                  ->getComputedStyle()
                  .stroke.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0, 0xFF, 0xFF))))));

  EXPECT_THAT(instantiateSubtreeElement(R"(
      <rect />
      <style>
        rect { stroke: lime }
      </style>
    )")
                  ->getComputedStyle()
                  .stroke.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0xFF, 0, 0xFF))))));
}

TEST(ElementStyleTests, StyleInheritance) {
  EXPECT_THAT(instantiateSubtreeElement(R"(
      <rect style="color: red" />
      <style>
        rect { stroke: lime }
      </style>
    )")
                  ->getComputedStyle()
                  .stroke.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0xFF, 0, 0xFF))))));
}

TEST(ElementStyleTests, AttributeMatchers) {
  // Using presentation attributes always works.
  EXPECT_THAT(instantiateSubtreeElement(R"(
      <rect fill="red" />
      <style>
        rect[fill=red] { stroke: lime }
      </style>
    )")
                  ->getComputedStyle()
                  .stroke.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0xFF, 0, 0xFF))))));

  // User attributes are retained by default, so the `rect[test="value"]` matcher
  // applies.
  EXPECT_THAT(instantiateSubtreeElement(R"(
      <rect fill="red" test="value" />
      <style>
        rect[test="value"] { stroke: lime }
      </style>
    )")
                  ->getComputedStyle()
                  .stroke.get(),
              Optional(PaintServer(PaintServer::Solid(Color(RGBA(0, 0xFF, 0, 0xFF))))));

  // It will not match if user attributes are explicitly disabled.
  parser::SVGParser::Options disableUserAttributesOptions;
  disableUserAttributesOptions.disableUserAttributes = true;

  EXPECT_THAT(instantiateSubtreeElement(R"(
      <rect fill="red" test="value" />
      <style>
        rect[test="value"] { stroke: lime }
      </style>
    )",
                                        disableUserAttributesOptions)
                  ->getComputedStyle()
                  .stroke.get(),
              Optional(PaintServer(PaintServer::None())));
}
}  // namespace donner::svg
