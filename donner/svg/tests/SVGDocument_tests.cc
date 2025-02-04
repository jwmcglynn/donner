#include "donner/svg/SVGDocument.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/Transform.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/parser/SVGParser.h"

using testing::Eq;
using testing::Optional;

namespace donner::svg {

namespace {

/// Helper to parse an SVG string and return the resulting document.
SVGDocument ParseSVG(std::string_view input) {
  parser::SVGParser::Options options;
  options.disableUserAttributes = false;

  auto maybeResult = parser::SVGParser::ParseSVG(input, /*outWarnings=*/nullptr, options);
  EXPECT_THAT(maybeResult, NoParseError());
  return std::move(maybeResult).result();
}

/// Matcher to check for an element with the given id.
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
    EXPECT_THAT(document.querySelector("does-not-exist"), Eq(std::nullopt));
  }
}

/**
 * Verify that the document's root element is an `<svg>` element.
 */
TEST(SVGDocument, RootElementTag) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 300 300">
      <circle id="c1" cx="150" cy="150" r="50"/>
    </svg>
  )");

  EXPECT_EQ(document.svgElement().type(), ElementType::SVG);
}

/**
 * Verify that the width() and height() accessors reflect the canvas size.
 */
TEST(SVGDocument, WidthHeightAccessors) {
  SVGDocument doc;
  doc.setCanvasSize(123, 456);
  EXPECT_EQ(doc.width(), 123);
  EXPECT_EQ(doc.height(), 456);
}

/**
 * Verify that when the viewBox and canvas size are identical,
 * the `documentFromCanvasTransform()` is the identity transform.
 */
TEST(SVGDocument, DocumentFromCanvasTransformIdentity) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
    </svg>
  )");

  const Transformd transform = document.documentFromCanvasTransform();
  EXPECT_TRUE(transform.isIdentity()) << "transform=" << transform;
}

/**
 * Verify that when the canvas size differs from the viewBox, \c documentFromCanvasTransform()
 * returns a transformation in `destinationFromSource` notation that maps coordinates from the
 * viewBox (source) to the canvas-scaled document space (destination).
 *
 * For a viewBox of 200×200 and a canvas size of 100×200, the transformation scales the x‑coordinate
 * by 0.5 (i.e. a point (50, 100) in the viewBox is mapped to (25, 100) in the document space),
 * while the y‑coordinate remains unchanged.
 */
TEST(SVGDocument, DocumentFromCanvasTransformScaling) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="100" height="200">
    </svg>
  )");

  const Transformd transform = document.documentFromCanvasTransform();
  EXPECT_EQ(transform.transformPosition(Vector2d(50, 100)), Vector2d(25, 100));
}

/**
 * Verify that the equality operator distinguishes between different documents.
 *
 * Documents referencing the same underlying registry (via copy construction) compare equal,
 * while independently created documents are not equal.
 */
TEST(SVGDocument, EqualityOperator) {
  SVGDocument doc1;
  SVGDocument doc2 =
      doc1.svgElement().ownerDocument();  // Should refer to the same underlying registry.
  EXPECT_TRUE(doc1 == doc2);

  SVGDocument doc3;
  EXPECT_FALSE(doc1 == doc3);
}

/**
 * Verify that more advanced query selectors work correctly.
 *
 * This includes using attribute selectors and descendant combinators.
 */
TEST(SVGDocument, QuerySelectorAdvanced) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 400">
       <g id="group1">
           <rect id="r1" x="10" y="10" width="50" height="50" data-type="foo"/>
       </g>
       <g id="group2">
           <rect id="r2" x="70" y="10" width="50" height="50" data-type="bar"/>
       </g>
    </svg>
  )");
  // Query by attribute.
  EXPECT_THAT(document.querySelector("[data-type='bar']"), Optional(ElementIdEq("r2")));
  // Query using descendant combinator and id selectors.
  EXPECT_THAT(document.querySelector("svg > g#group1 > rect"), Optional(ElementIdEq("r1")));
}

}  // namespace donner::svg
