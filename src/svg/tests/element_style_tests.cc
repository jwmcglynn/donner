#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/tests/base_test_utils.h"
#include "src/svg/tests/xml_test_utils.h"

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
  fill: PaintServer(solid Color(255, 0, 0, 255)) (set) @ Specificity(0, 0, 0)
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

}  // namespace donner::svg
