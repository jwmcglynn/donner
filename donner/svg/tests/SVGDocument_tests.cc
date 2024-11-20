#include "donner/svg/SVGDocument.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/parser/SVGParser.h"

using testing::Optional;

namespace donner::svg {

namespace {

SVGDocument ParseSVG(std::string_view input) {
  auto maybeResult = parser::SVGParser::ParseSVG(input);
  EXPECT_THAT(maybeResult, NoParseError());
  return std::move(maybeResult).result();
}

MATCHER_P(ElementIdEq, id, "") {
  return arg.id() == id;
}

}  // namespace

TEST(SVGDocument, Create) {
  SVGDocument document;
  EXPECT_TRUE(document.rootEntityHandle());
  EXPECT_EQ(document.svgElement().ownerDocument(), document);
}

TEST(SVGDocument, CanvasSize) {
  SVGDocument document;
  EXPECT_EQ(document.canvasSize(), Vector2i(512, 512));

  document.setCanvasSize(100, 200);
  EXPECT_EQ(document.canvasSize(), Vector2i(100, 200));

  document.useAutomaticCanvasSize();
  EXPECT_EQ(document.canvasSize(), Vector2i(512, 512));
}

TEST(SVGDocument, CanvasSizeFromFile) {
  {
    auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
    </svg>
  )");
    EXPECT_EQ(document.canvasSize(), Vector2i(200, 200));
  }

  {
    auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="100" height="200">
    </svg>
  )");
    EXPECT_EQ(document.canvasSize(), Vector2i(100, 200));
  }
}

TEST(SVGDocument, QuerySelector) {
  {
    auto document = ParseSVG(R"(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
        <rect id="rect1" x="10" y="10" width="100" height="100" />
        <rect id="rect2" x="10" y="10" width="100" height="100" />
      </svg>
    )");

    EXPECT_THAT(document.querySelector("rect"), Optional(ElementIdEq("rect1")));
    EXPECT_THAT(document.querySelector("#rect2"), Optional(ElementIdEq("rect2")));
    EXPECT_THAT(document.querySelector("svg > :nth-child(2)"), Optional(ElementIdEq("rect2")));
    EXPECT_THAT(document.querySelector("does-not-exist"), testing::Eq(std::nullopt));
  }
}

}  // namespace donner::svg
