#include "donner/svg/components/layout/LayoutSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/parser/tests/ParseResultTestUtils.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/xml/XMLParser.h"

namespace donner::svg::components {

class LayoutSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(const std::string& input) {
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

  EXPECT_THAT(layoutSystem.getViewport(EntityHandle(
                  document.registry(), document.svgElement().querySelector("#nested")->entity())),
              BoxEq(Vector2i(0, 0), Vector2i(100, 100)));
}

TEST_F(LayoutSystemTest, ViewportNestedSvgWithComputedComponents) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <svg id="nested" viewBox="0 0 100 100" />
    </svg>
  )");

  layoutSystem.instantiateAllComputedComponents(document.registry(), nullptr);
  EXPECT_THAT(layoutSystem.getViewport(EntityHandle(
                  document.registry(), document.svgElement().querySelector("#nested")->entity())),
              BoxEq(Vector2i(0, 0), Vector2i(100, 100)));
}

TEST_F(LayoutSystemTest, ViewportPattern) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <pattern id="pattern" viewBox="0 0 100 100" />
    </svg>
  )");

  EXPECT_THAT(layoutSystem.getViewport(EntityHandle(
                  document.registry(), document.svgElement().querySelector("pattern")->entity())),
              BoxEq(Vector2i(0, 0), Vector2i(100, 100)));
}

TEST_F(LayoutSystemTest, ViewportPatternWithComputedComponents) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <pattern id="pattern" viewBox="0 0 100 100" />
    </svg>
  )");

  layoutSystem.instantiateAllComputedComponents(document.registry(), nullptr);
  EXPECT_THAT(layoutSystem.getViewport(EntityHandle(
                  document.registry(), document.svgElement().querySelector("pattern")->entity())),
              BoxEq(Vector2i(0, 0), Vector2i(100, 100)));
}

}  // namespace donner::svg::components
