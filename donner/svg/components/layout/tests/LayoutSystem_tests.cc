#include "donner/svg/components/layout/LayoutSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/parser/tests/ParseResultTestUtils.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/xml/XMLParser.h"

namespace donner::svg::components {

class LayoutSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    parser::XMLParser::InputBuffer inputBuffer(input);
    auto maybeResult = parser::XMLParser::ParseSVG(inputBuffer);
    EXPECT_THAT(maybeResult, base::parser::NoParseError());
    return std::move(maybeResult).result();
  }

  LayoutSystem layoutSystem;
};

TEST_F(LayoutSystemTest, ViewportRoot) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
    </svg>
  )");

  EXPECT_THAT(layoutSystem.getViewport(EntityHandle(document.registry(), document.rootEntity())),
              BoxEq(Vector2i(0, 0), Vector2i(200, 200)));
}

TEST_F(LayoutSystemTest, ViewportRootWithComputedComponents) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
    </svg>
  )");

  layoutSystem.instantiateAllComputedComponents(document.registry(), nullptr);
  EXPECT_THAT(layoutSystem.getViewport(EntityHandle(document.registry(), document.rootEntity())),
              BoxEq(Vector2i(0, 0), Vector2i(200, 200)));
}

TEST_F(LayoutSystemTest, ViewportNestedSvg) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <svg id="nested" viewBox="0 0 100 100" />
    </svg>
  )");

  EXPECT_THAT(layoutSystem.getViewport(
                  EntityHandle(document.registry(), document.querySelector("#nested")->entity())),
              BoxEq(Vector2i(0, 0), Vector2i(100, 100)));
}

TEST_F(LayoutSystemTest, ViewportNestedSvgWithComputedComponents) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <svg id="nested" viewBox="0 0 100 100" />
    </svg>
  )");

  layoutSystem.instantiateAllComputedComponents(document.registry(), nullptr);
  EXPECT_THAT(layoutSystem.getViewport(
                  EntityHandle(document.registry(), document.querySelector("#nested")->entity())),
              BoxEq(Vector2i(0, 0), Vector2i(100, 100)));
}

TEST_F(LayoutSystemTest, ViewportPattern) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <pattern id="pattern" viewBox="0 0 100 100" />
    </svg>
  )");

  EXPECT_THAT(layoutSystem.getViewport(
                  EntityHandle(document.registry(), document.querySelector("pattern")->entity())),
              BoxEq(Vector2i(0, 0), Vector2i(100, 100)));
}

TEST_F(LayoutSystemTest, ViewportPatternWithComputedComponents) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <pattern id="pattern" viewBox="0 0 100 100" />
    </svg>
  )");

  layoutSystem.instantiateAllComputedComponents(document.registry(), nullptr);
  EXPECT_THAT(layoutSystem.getViewport(
                  EntityHandle(document.registry(), document.querySelector("pattern")->entity())),
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

  auto groupEntity = document.querySelector("#group1")->entity();
  auto rectEntity = document.querySelector("#rect1")->entity();

  // Test getting the transform for the group
  Transformd groupTransform =
      layoutSystem.getEntityFromParentTranform(EntityHandle(document.registry(), groupEntity));
  EXPECT_THAT(groupTransform, TransformEq(Transformd::Translate({10.0, 20.0})));

  // Test setting a new transform for the rectangle
  Transformd newRectTransform = Transformd::Translate({30.0, 40.0});
  layoutSystem.setEntityFromParentTransform(EntityHandle(document.registry(), rectEntity),
                                            newRectTransform);

  // Verify the new transform
  Transformd updatedRectTransform =
      layoutSystem.getEntityFromParentTranform(EntityHandle(document.registry(), rectEntity));
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

  auto rectEntity = document.querySelector("#rect1")->entity();

  // Set a transform with scale and translation
  Transformd scaleTransform = Transformd::Scale({2.0, 3.0}) * Transformd::Translate({10.0, 20.0});
  layoutSystem.setEntityFromParentTransform(EntityHandle(document.registry(), rectEntity),
                                            scaleTransform);

  // Verify the new transform
  Transformd updatedTransform =
      layoutSystem.getEntityFromParentTranform(EntityHandle(document.registry(), rectEntity));

  EXPECT_THAT(updatedTransform,
              TransformEq(Transformd::Scale({2.0, 3.0}) * Transformd::Translate({10.0, 20.0})));
}

}  // namespace donner::svg::components
