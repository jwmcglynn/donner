#include "donner/svg/components/layout/LayoutSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg::components {

class LayoutSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    auto maybeResult = parser::SVGParser::ParseSVG(input);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  LayoutSystem layoutSystem;
};

TEST_F(LayoutSystemTest, ViewportRoot) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
    </svg>
  )");

  EXPECT_THAT(layoutSystem.getViewBox(document.rootEntityHandle()),
              BoxEq(Vector2i(0, 0), Vector2i(200, 200)));
}

TEST_F(LayoutSystemTest, ViewportRootWithComputedComponents) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
    </svg>
  )");

  layoutSystem.instantiateAllComputedComponents(document.registry(), nullptr);
  EXPECT_THAT(layoutSystem.getViewBox(document.rootEntityHandle()),
              BoxEq(Vector2i(0, 0), Vector2i(200, 200)));
}

TEST_F(LayoutSystemTest, ViewportNestedSvg) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <svg id="nested" viewBox="0 0 100 100" />
    </svg>
  )");

  EXPECT_THAT(layoutSystem.getViewBox(document.querySelector("#nested")->entityHandle()),
              BoxEq(Vector2i(0, 0), Vector2i(100, 100)));
}

TEST_F(LayoutSystemTest, ViewportNestedSvgWithComputedComponents) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <svg id="nested" viewBox="0 0 100 100" />
    </svg>
  )");

  layoutSystem.instantiateAllComputedComponents(document.registry(), nullptr);
  EXPECT_THAT(layoutSystem.getViewBox(document.querySelector("#nested")->entityHandle()),
              BoxEq(Vector2i(0, 0), Vector2i(100, 100)));
}

TEST_F(LayoutSystemTest, ViewportPattern) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <pattern id="pattern" viewBox="0 0 100 100" />
    </svg>
  )");

  EXPECT_THAT(layoutSystem.getViewBox(document.querySelector("pattern")->entityHandle()),
              BoxEq(Vector2i(0, 0), Vector2i(100, 100)));
}

TEST_F(LayoutSystemTest, ViewportPatternWithComputedComponents) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <pattern id="pattern" viewBox="0 0 100 100" />
    </svg>
  )");

  layoutSystem.instantiateAllComputedComponents(document.registry(), nullptr);
  EXPECT_THAT(layoutSystem.getViewBox(document.querySelector("pattern")->entityHandle()),
              BoxEq(Vector2i(0, 0), Vector2i(100, 100)));
}

TEST_F(LayoutSystemTest, GetSetEntityFromParentTransform) {
  auto document = ParseSVG(R"-(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <g id="group1" transform="translate(10, 20)">
        <rect id="rect1" x="0" y="0" width="100" height="100"/>
      </g>
    </svg>
  )-");

  auto groupEntityHandle = document.querySelector("#group1")->entityHandle();
  auto rectEntityHandle = document.querySelector("#rect1")->entityHandle();

  // Test getting the transform for the group
  const Transformd groupTransform = layoutSystem.getRawEntityFromParentTransform(groupEntityHandle);
  EXPECT_THAT(groupTransform, TransformEq(Transformd::Translate({10.0, 20.0})));

  // Test setting a new transform for the rectangle
  const Transformd newRectTransform = Transformd::Translate({30.0, 40.0});
  layoutSystem.setRawEntityFromParentTransform(rectEntityHandle, newRectTransform);

  // Verify the new transform
  const Transformd updatedRectTransform =
      layoutSystem.getRawEntityFromParentTransform(rectEntityHandle);
  EXPECT_THAT(updatedRectTransform, TransformEq(Transformd::Translate({30.0, 40.0})));
}

TEST_F(LayoutSystemTest, GetSetEntityFromParentTransformWithScale) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <g id="group1">
        <rect id="rect1" x="0" y="0" width="100" height="100"/>
      </g>
    </svg>
  )");

  auto rectEntityHandle = document.querySelector("#rect1")->entityHandle();

  // Set a transform with scale and translation
  const Transformd scaleTransform =
      Transformd::Scale({2.0, 3.0}) * Transformd::Translate({10.0, 20.0});
  layoutSystem.setRawEntityFromParentTransform(rectEntityHandle, scaleTransform);

  // Verify the new transform
  const Transformd updatedTransform =
      layoutSystem.getRawEntityFromParentTransform(rectEntityHandle);

  EXPECT_THAT(updatedTransform,
              TransformEq(Transformd::Scale({2.0, 3.0}) * Transformd::Translate({10.0, 20.0})));
}

TEST_F(LayoutSystemTest, GetEntityContentTransform) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <svg id="inner" x="50" y="50" width="100" height="100" viewBox="0 0 50 50">
        <rect x="0" y="0" width="50" height="50" fill="red"/>
      </svg>
    </svg>
  )");

  auto innerSvgEntity = document.querySelector("#inner")->entityHandle();

  EXPECT_THAT(layoutSystem.getEntityContentFromEntityTransform(innerSvgEntity),
              TransformEq(Transformd::Scale({2.0, 2.0}) * Transformd::Translate({50.0, 50.0})));
}

TEST_F(LayoutSystemTest, GetEntityFromWorldTransform) {
  auto document = ParseSVG(R"-(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <rect id="rect1" transform="translate(10, 20)" />
      <g transform="scale(5)">
        <rect id="rect2" transform="translate(10, 20)" />
      </g>
      <svg x="50" y="50" width="100" height="100" viewBox="0 0 50 50">
        <rect id="rect3" transform="translate(10, 20)" />
      </svg>
    </svg>
  )-");

  auto rect1 = document.querySelector("#rect1")->entityHandle();
  auto rect2 = document.querySelector("#rect2")->entityHandle();
  auto rect3 = document.querySelector("#rect3")->entityHandle();

  EXPECT_THAT(layoutSystem.getEntityFromWorldTransform(rect1),
              TransformEq(Transformd::Translate({10.0, 20.0})));
  EXPECT_THAT(layoutSystem.getEntityFromWorldTransform(rect2),
              TransformEq(Transformd::Translate({10.0, 20.0}) * Transformd::Scale({5.0, 5.0})));

  EXPECT_THAT(layoutSystem.getEntityFromWorldTransform(rect3),
              TransformEq(Transformd::Translate({10.0, 20.0}) * Transformd::Scale({2.0, 2.0}) *
                          Transformd::Translate({50.0, 50.0})));
}

TEST_F(LayoutSystemTest, TransformOriginSupport) {
  auto document = ParseSVG(R"-(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="a" x="0" y="0" width="100" height="100" style="transform-origin: 50% 50%; transform: rotate(90deg)" />
      <rect id="b" x="0" y="0" width="100" height="100" style="transform-origin: 0 0; transform: rotate(90deg)" />
    </svg>
  )-");

  auto rectA = document.querySelector("#a")->entityHandle();
  auto rectB = document.querySelector("#b")->entityHandle();

  const Transformd expectedOrigin50Percent = Transformd::Translate({50.0, 50.0}) *
                                             Transformd::Rotate(MathConstants<double>::kHalfPi) *
                                             Transformd::Translate({-50.0, -50.0});

  EXPECT_THAT(layoutSystem.getEntityFromParentTransform(rectA),
              TransformEq(expectedOrigin50Percent));
  EXPECT_THAT(layoutSystem.getEntityFromParentTransform(rectB),
              TransformEq(Transformd::Rotate(MathConstants<double>::kHalfPi)));
}

/**
 * Verify transform-origin with 100 % 100 % (bottom‑right corner of the element).
 */
TEST_F(LayoutSystemTest, TransformOriginBottomRight) {
  auto document = ParseSVG(R"-(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="c" x="0" y="0" width="100" height="100"
            style="transform-origin: 100% 100%; transform: rotate(90deg)" />
    </svg>
  )-");

  auto rectC = document.querySelector("#c")->entityHandle();

  const Transformd expected = Transformd::Translate({100.0, 100.0}) *
                              Transformd::Rotate(MathConstants<double>::kHalfPi) *
                              Transformd::Translate({-100.0, -100.0});

  EXPECT_THAT(layoutSystem.getEntityFromParentTransform(rectC), TransformEq(expected));
}

/**
 * Verify transform-origin with 25 % 75 % (mixed percentages).
 */
TEST_F(LayoutSystemTest, TransformOriginQuarterThreeQuarter) {
  auto document = ParseSVG(R"-(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="d" x="0" y="0" width="100" height="100"
            style="transform-origin: 25% 75%; transform: rotate(90deg)" />
    </svg>
  )-");

  auto rectD = document.querySelector("#d")->entityHandle();

  const Transformd expected = Transformd::Translate({25.0, 75.0}) *
                              Transformd::Rotate(MathConstants<double>::kHalfPi) *
                              Transformd::Translate({-25.0, -75.0});

  EXPECT_THAT(layoutSystem.getEntityFromParentTransform(rectD), TransformEq(expected));
}

/**
 * Verify transform-origin with absolute pixel values (10 px 20 px).
 */
TEST_F(LayoutSystemTest, TransformOriginPixels) {
  auto document = ParseSVG(R"-(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="e" x="0" y="0" width="100" height="100"
            style="transform-origin: 10px 20px; transform: rotate(90deg)" />
    </svg>
  )-");

  auto rectE = document.querySelector("#e")->entityHandle();

  const Transformd expected = Transformd::Translate({10.0, 20.0}) *
                              Transformd::Rotate(MathConstants<double>::kHalfPi) *
                              Transformd::Translate({-10.0, -20.0});

  EXPECT_THAT(layoutSystem.getEntityFromParentTransform(rectE), TransformEq(expected));
}
}  // namespace donner::svg::components
