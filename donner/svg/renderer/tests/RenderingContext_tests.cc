/**
 * @file Tests for RenderingContext: render tree instantiation, hit testing, and bounds queries.
 */

#include "donner/svg/renderer/RenderingContext.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/parser/SVGParser.h"

using testing::ElementsAre;
using testing::Gt;
using testing::NotNull;

namespace donner::svg::components {

class RenderingContextTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    ParseWarningSink parseSink;
    auto maybeResult = parser::SVGParser::ParseSVG(input, parseSink);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  ParseWarningSink warningSink_;
};

// --- instantiateRenderTree ---

TEST_F(RenderingContextTest, InstantiateRenderTreeBasic) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="10" y="10" width="80" height="80" fill="red"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);
}

TEST_F(RenderingContextTest, InstantiateRenderTreeVerbose) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect width="50" height="50" fill="blue"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(true, warningSink_);
}

TEST_F(RenderingContextTest, InstantiateRenderTreeWarnings) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect width="50" height="50" fill="red"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);
}

TEST_F(RenderingContextTest, InstantiateRenderTreeMultipleShapes) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <rect x="10" y="10" width="40" height="40" fill="red"/>
      <circle cx="100" cy="100" r="30" fill="blue"/>
      <ellipse cx="160" cy="50" rx="20" ry="30" fill="green"/>
      <line x1="0" y1="0" x2="200" y2="200" stroke="black"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);
}

// --- Hit testing ---

TEST_F(RenderingContextTest, FindIntersectingHitsRect) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="10" width="80" height="80" fill="red"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  Entity hit = ctx.findIntersecting(Vector2d(50, 50));
  EXPECT_TRUE(hit != entt::null);
}

TEST_F(RenderingContextTest, FindIntersectingMissesEmptyArea) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="50" y="50" width="40" height="40" fill="red"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  Entity hit = ctx.findIntersecting(Vector2d(5, 5));
  EXPECT_TRUE(hit == entt::null);
}

TEST_F(RenderingContextTest, FindAllIntersecting) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="0" y="0" width="100" height="100" fill="red"/>
      <rect x="25" y="25" width="50" height="50" fill="blue"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  std::vector<Entity> hits = ctx.findAllIntersecting(Vector2d(50, 50));
  EXPECT_THAT(hits.size(), Gt(0u));
}

// --- World bounds ---

TEST_F(RenderingContextTest, GetWorldBoundsRect) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="20" width="30" height="40" fill="red"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto bounds = ctx.getWorldBounds(element->entityHandle().entity());
  ASSERT_TRUE(bounds.has_value());
  EXPECT_NEAR(bounds->topLeft.x, 10.0, 1.0);
  EXPECT_NEAR(bounds->topLeft.y, 20.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.x, 40.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.y, 60.0, 1.0);
}

TEST_F(RenderingContextTest, GetWorldBoundsInvalidEntity) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"/>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  auto bounds = ctx.getWorldBounds(entt::null);
  EXPECT_FALSE(bounds.has_value());
}

// --- Invalidation ---

TEST_F(RenderingContextTest, InvalidateAndReinstantiate) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="10" y="10" width="80" height="80" fill="red"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);
  ctx.invalidateRenderTree();
  ctx.instantiateRenderTree(false, warningSink_);
}

// --- Complex documents ---

TEST_F(RenderingContextTest, GroupedElements) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <g transform="translate(50, 50)">
        <rect id="r" x="0" y="0" width="50" height="50" fill="red"/>
      </g>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  auto bounds = ctx.getWorldBounds(document.querySelector("#r")->entityHandle().entity());
  ASSERT_TRUE(bounds.has_value());
  EXPECT_NEAR(bounds->topLeft.x, 50.0, 1.0);
  EXPECT_NEAR(bounds->topLeft.y, 50.0, 1.0);
}

TEST_F(RenderingContextTest, GradientAndClipPath) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="g">
          <stop offset="0" stop-color="red"/>
          <stop offset="1" stop-color="blue"/>
        </linearGradient>
        <clipPath id="cp">
          <rect width="50" height="50"/>
        </clipPath>
      </defs>
      <rect width="100" height="100" fill="url(#g)" clip-path="url(#cp)"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);
}

TEST_F(RenderingContextTest, UseElement) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <rect id="template" width="20" height="20" fill="green"/>
      </defs>
      <use href="#template" x="50" y="50"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);
}

TEST_F(RenderingContextTest, FilterElement) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <filter id="blur">
          <feGaussianBlur stdDeviation="5"/>
        </filter>
      </defs>
      <rect width="80" height="80" fill="red" filter="url(#blur)"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);
}

TEST_F(RenderingContextTest, DisplayNoneNotIntersectable) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="0" y="0" width="100" height="100" fill="red" style="display: none"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  Entity hit = ctx.findIntersecting(Vector2d(50, 50));
  EXPECT_TRUE(hit == entt::null);
}

TEST_F(RenderingContextTest, RectIntersection) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <rect x="50" y="50" width="100" height="100" fill="blue"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  std::vector<Entity> hits = ctx.findIntersectingRect(Box2d(Vector2d(60, 60), Vector2d(80, 80)));
  EXPECT_THAT(hits.size(), Gt(0u));
}

TEST_F(RenderingContextTest, MaskElement) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <mask id="m">
          <rect width="50" height="50" fill="white"/>
        </mask>
      </defs>
      <rect width="100" height="100" fill="red" mask="url(#m)"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);
}

TEST_F(RenderingContextTest, PointerEventsBoundingBoxHitsWithoutPaint) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="10" width="80" height="80"
            fill="none" stroke="none" pointer-events="bounding-box"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  const Entity rectEntity = document.querySelector("#r")->entityHandle().entity();
  EXPECT_EQ(ctx.findIntersecting(Vector2d(50, 50)), rectEntity);
}

TEST_F(RenderingContextTest, PointerEventsVisibleFillHitsTransparentFill) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="10" width="80" height="80"
            fill="red" fill-opacity="0" stroke="none" pointer-events="visibleFill"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  const Entity rectEntity = document.querySelector("#r")->entityHandle().entity();
  EXPECT_EQ(ctx.findIntersecting(Vector2d(50, 50)), rectEntity);
}

TEST_F(RenderingContextTest, PointerEventsPaintedRequiresVisiblePaint) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="10" y="10" width="80" height="80"
            fill="none" stroke="none" pointer-events="painted"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  EXPECT_TRUE(ctx.findIntersecting(Vector2d(50, 50)) == entt::null);
}

TEST_F(RenderingContextTest, PointerEventsVisibleStrokeRequiresVisibility) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="10" y="10" width="80" height="80"
            fill="none" stroke="black" stroke-width="20"
            visibility="hidden" pointer-events="visibleStroke"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  EXPECT_TRUE(ctx.findIntersecting(Vector2d(15, 50)) == entt::null);
}

TEST_F(RenderingContextTest, PointerEventsStrokeHitsHiddenStrokeGeometry) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="10" width="80" height="80"
            fill="none" stroke="black" stroke-width="20"
            visibility="hidden" pointer-events="stroke"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  const Entity rectEntity = document.querySelector("#r")->entityHandle().entity();
  EXPECT_EQ(ctx.findIntersecting(Vector2d(15, 50)), rectEntity);
  EXPECT_TRUE(ctx.findIntersecting(Vector2d(50, 50)) == entt::null);
}

TEST_F(RenderingContextTest, FindIntersectingRectReturnsFrontToBackOrder) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="back" x="0" y="0" width="100" height="100" fill="red"/>
      <rect id="front" x="25" y="25" width="50" height="50" fill="blue"/>
      <rect id="outside" x="80" y="80" width="10" height="10" fill="green"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  const Entity backEntity = document.querySelector("#back")->entityHandle().entity();
  const Entity frontEntity = document.querySelector("#front")->entityHandle().entity();

  const std::vector<Entity> hits = ctx.findIntersectingRect(Box2d::FromXYWH(30, 30, 10, 10));
  ASSERT_THAT(hits.size(), Gt(1u));
  EXPECT_EQ(hits.front(), frontEntity);
  EXPECT_THAT(hits, testing::Contains(backEntity));
}

}  // namespace donner::svg::components
