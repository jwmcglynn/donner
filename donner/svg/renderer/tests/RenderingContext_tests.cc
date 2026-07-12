/**
 * Tests for RenderingContext: render tree instantiation, hit testing, and bounds queries.
 */

#include "donner/svg/renderer/RenderingContext.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/parser/SVGParser.h"

using testing::ElementsAre;
using testing::Gt;
using testing::NotNull;
using testing::Optional;

namespace donner::svg::components {

class RenderingContextTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    ParseWarningSink parseSink;
    auto maybeResult = parser::SVGParser::ParseSVG(input, parseSink);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  /**
   * Find the render-tree instance of the shadow entity instantiated for \p lightHandle (the
   * element in the document tree that a `<use>` shadow instance mirrors). Expects exactly one
   * shadow instance; returns nullptr if none exists.
   */
  static const RenderingInstanceComponent* findShadowInstance(Registry& registry,
                                                              EntityHandle lightHandle) {
    const RenderingInstanceComponent* result = nullptr;
    for (auto view = registry.view<RenderingInstanceComponent>(); auto entity : view) {
      const auto& instance = view.get<RenderingInstanceComponent>(entity);
      if (instance.dataEntity == lightHandle.entity() && entity != lightHandle.entity()) {
        EXPECT_THAT(result, testing::IsNull())
            << "Multiple shadow instances found for the same light entity";
        result = &instance;
      }
    }

    return result;
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
  auto bounds = ctx.getWorldBounds(element->unsafeEntityHandle().entity());
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

TEST_F(RenderingContextTest, InstantiateRenderTreeClearsFullRebuildState) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="10" y="10" width="80" height="80" fill="red"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.invalidateRenderTree();
  ctx.instantiateRenderTree(false, warningSink_);

  const auto* renderState = document.registry().ctx().find<RenderTreeState>();
  ASSERT_THAT(renderState, NotNull());
  EXPECT_FALSE(renderState->needsFullRebuild);
  EXPECT_FALSE(renderState->needsFullStyleRecompute);
  EXPECT_TRUE(renderState->hasBeenBuilt);
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

  auto bounds = ctx.getWorldBounds(document.querySelector("#r")->unsafeEntityHandle().entity());
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

TEST_F(RenderingContextTest, DirtyUseRebuildRecomputesShadowTreeStyles) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <rect id="template" width="20" height="20" fill="green"/>
      </defs>
      <use id="u" href="#template" x="50" y="50"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  auto useElement = document.querySelector("#u");
  ASSERT_TRUE(useElement.has_value());
  useElement->setAttribute("transform", "translate(10 0)");

  ctx.instantiateRenderTree(false, warningSink_);

  for (auto view =
           document.registry().view<donner::components::TreeComponent, ComputedStyleComponent>();
       auto entity : view) {
    const auto& computedStyle = view.get<ComputedStyleComponent>(entity);
    EXPECT_TRUE(computedStyle.properties.has_value())
        << "entity " << int(entt::to_integral(entity))
        << " kept an uncomputed style after dirty shadow-tree rebuild";
  }
}

// --- <use> referencing an inline <svg> (resvg structure/use/xlink-to-svg-element-*) ---

/**
 * `<use>` of an inline `<svg>` with `width`/`height` on the use element: the use's size overrides
 * the referenced svg's, the svg's own `x`/`y` offset the instance viewport, and the viewport
 * clips (UA style `overflow: hidden`). Mirrors resvg `structure/use/xlink-to-svg-element-with-
 * rect.svg`.
 */
TEST_F(RenderingContextTest, UseOfInlineSvgOverridesSizeAndClips) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <svg id="inner" x="40" y="40" width="80" height="80">
          <circle cx="100" cy="100" r="120" fill="green"/>
        </svg>
      </defs>
      <use href="#inner" width="100" height="150"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  const RenderingInstanceComponent* instance =
      findShadowInstance(document.registry(), document.querySelector("#inner")->entityHandle());
  ASSERT_THAT(instance, NotNull());

  // Viewport: x/y from the referenced <svg>, width/height overridden by the <use>.
  EXPECT_THAT(instance->clipRect, Optional(Box2d(Vector2d(40.0, 40.0), Vector2d(140.0, 190.0))));
  EXPECT_THAT(instance->worldFromEntityTransform,
              TransformEq(Transform2d::Translate({40.0, 40.0})));
}

/**
 * `<use>` of an inline `<svg viewBox="...">`: the instance viewport takes the use's size and the
 * viewBox maps into it with preserveAspectRatio (xMidYMid meet). Mirrors resvg
 * `structure/use/xlink-to-svg-element-with-viewBox.svg`.
 */
TEST_F(RenderingContextTest, UseOfInlineSvgWithViewBoxScalesContent) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <svg id="inner" viewBox="0 0 200 200">
          <circle cx="100" cy="100" r="80" fill="green"/>
        </svg>
      </defs>
      <use href="#inner" width="100" height="150"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  const RenderingInstanceComponent* instance =
      findShadowInstance(document.registry(), document.querySelector("#inner")->entityHandle());
  ASSERT_THAT(instance, NotNull());

  EXPECT_THAT(instance->clipRect, Optional(Box2d(Vector2d(0.0, 0.0), Vector2d(100.0, 150.0))));

  // viewBox 200x200 into 100x150 with xMidYMid meet: scale 0.5, centered vertically (+25 on y).
  EXPECT_THAT(instance->worldFromEntityTransform,
              TransformEq(Transform2d::Scale({0.5, 0.5}) * Transform2d::Translate({0.0, 25.0})));
}

/**
 * `<use>` of an inline `<svg>` with no size of its own: the use's width/height define the clip
 * viewport and content renders unscaled. Mirrors resvg
 * `structure/use/xlink-to-svg-element-with-width-height-on-use.svg`.
 */
TEST_F(RenderingContextTest, UseOfInlineSvgSizeFromUseOnly) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <svg id="inner">
          <circle cx="100" cy="100" r="80" fill="green"/>
        </svg>
      </defs>
      <use href="#inner" width="100" height="150"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  const RenderingInstanceComponent* instance =
      findShadowInstance(document.registry(), document.querySelector("#inner")->entityHandle());
  ASSERT_THAT(instance, NotNull());

  EXPECT_THAT(instance->clipRect, Optional(Box2d(Vector2d(0.0, 0.0), Vector2d(100.0, 150.0))));
  EXPECT_THAT(instance->worldFromEntityTransform, TransformEq(Transform2d()));
}

/**
 * Nested `<use>` chain: `use1 -> use2 -> <svg>`. The inner use's `height` overrides the svg's,
 * while the outer use's `width` has no effect (its target is a `<use>`, which does not establish
 * a viewport). Mirrors resvg
 * `structure/use/nested-xlink-to-svg-element-with-rect-and-size.svg`.
 */
TEST_F(RenderingContextTest, NestedUseOfUseOfInlineSvgIgnoresOuterUseSize) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <svg id="inner" x="40" y="40" width="80" height="80">
          <circle cx="100" cy="100" r="120" fill="green"/>
        </svg>
        <use id="use2" href="#inner" height="100"/>
      </defs>
      <use id="use1" href="#use2" width="200"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  const RenderingInstanceComponent* instance =
      findShadowInstance(document.registry(), document.querySelector("#inner")->entityHandle());
  ASSERT_THAT(instance, NotNull());

  // Width stays 80 (from the svg), height 100 (from use2); use1's width="200" is ignored.
  EXPECT_THAT(instance->clipRect, Optional(Box2d(Vector2d(40.0, 40.0), Vector2d(120.0, 140.0))));
  EXPECT_THAT(instance->worldFromEntityTransform,
              TransformEq(Transform2d::Translate({40.0, 40.0})));
}

/**
 * A descendant selector must keep matching through shadow-entity parents. Regression test:
 * ShadowedElementAdapter::parentElement() looked up ElementTypeComponent on the raw tree entity,
 * so a shadow entity whose parent was also a shadow entity appeared parentless (matching `:root`
 * and breaking descendant combinators; the UA rule `svg:not(:root) { overflow: hidden }` also
 * failed to clip nested `<use>` -> `<svg>` viewports).
 */
TEST_F(RenderingContextTest, ShadowTreeDescendantSelectorMatchesThroughShadowParents) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <style>g rect { fill: rgb(1, 2, 3); }</style>
      <defs>
        <g id="template"><rect id="r" width="10" height="10"/></g>
      </defs>
      <use href="#template"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  // The shadow rect's parent is the shadow <g>: the `g rect` selector only matches if ancestor
  // traversal resolves shadow entities to their light data entities.
  const RenderingInstanceComponent* instance =
      findShadowInstance(document.registry(), document.querySelector("#r")->entityHandle());
  ASSERT_THAT(instance, NotNull());

  const auto& style = instance->styleHandle(document.registry()).get<ComputedStyleComponent>();
  ASSERT_TRUE(style.properties.has_value());
  EXPECT_EQ(style.properties->fill.get().value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(1, 2, 3, 0xFF)))));
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
  const Entity rectEntity = document.querySelector("#r")->unsafeEntityHandle().entity();
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
  const Entity rectEntity = document.querySelector("#r")->unsafeEntityHandle().entity();
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
  const Entity rectEntity = document.querySelector("#r")->unsafeEntityHandle().entity();
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
  const Entity backEntity = document.querySelector("#back")->unsafeEntityHandle().entity();
  const Entity frontEntity = document.querySelector("#front")->unsafeEntityHandle().entity();

  const std::vector<Entity> hits = ctx.findIntersectingRect(Box2d::FromXYWH(30, 30, 10, 10));
  ASSERT_THAT(hits.size(), Gt(1u));
  EXPECT_EQ(hits.front(), frontEntity);
  EXPECT_THAT(hits, testing::Contains(backEntity));
}

TEST_F(RenderingContextTest, RecursiveMarkerKeepsNestedSubtreeOutsideParentRenderRange) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <marker id="a" markerWidth="6" markerHeight="6" refX="1" refY="3" orient="auto">
          <use href="#recursive-path" transform="matrix(-1 0 0 1 6 0)"/>
        </marker>
        <marker id="b" markerWidth="6" markerHeight="6" refX="5" refY="3" orient="auto">
          <use href="#recursive-path"/>
        </marker>
        <path id="recursive-path" d="M 1 1 5 3 1 5" fill="none" stroke="blue"
              marker-start="url(#a)" marker-end="url(#b)"/>
      </defs>
      <path id="host" d="M 10 10 L 190 190" fill="none" stroke="green" stroke-width="5"
            marker-start="url(#a)" marker-end="url(#b)"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  Registry& registry = document.registry();
  const Entity hostEntity = document.querySelector("#host")->unsafeEntityHandle().entity();
  const auto& hostInstance = registry.get<RenderingInstanceComponent>(hostEntity);
  ASSERT_TRUE(hostInstance.markerEnd.has_value());
  ASSERT_TRUE(hostInstance.markerEnd->subtreeInfo.has_value());

  const SubtreeInfo& rootInfo = *hostInstance.markerEnd->subtreeInfo;
  const int rootFirstOrder =
      registry.get<RenderingInstanceComponent>(rootInfo.firstRenderedEntity).drawOrder;
  const int rootLastOrder =
      registry.get<RenderingInstanceComponent>(rootInfo.lastRenderedEntity).drawOrder;

  const RenderingInstanceComponent* nestedPath = nullptr;
  for (const Entity entity : registry.view<RenderingInstanceComponent>()) {
    const auto& candidate = registry.get<RenderingInstanceComponent>(entity);
    const auto* id = registry.try_get<IdComponent>(candidate.dataEntity);
    if (id && id->id() == "recursive-path" && candidate.drawOrder >= rootFirstOrder &&
        candidate.drawOrder <= rootLastOrder && candidate.markerStart.has_value()) {
      nestedPath = &candidate;
      break;
    }
  }

  ASSERT_THAT(nestedPath, NotNull());
  ASSERT_TRUE(nestedPath->markerStart->subtreeInfo.has_value());
  const int nestedLastOrder =
      registry
          .get<RenderingInstanceComponent>(nestedPath->markerStart->subtreeInfo->lastRenderedEntity)
          .drawOrder;
  // Nested markers are rendered through their referencing path, not as part of the parent's direct
  // range. Renderer cleanup must still skip their separate inline ranges after drawing the parent.
  EXPECT_GT(nestedLastOrder, rootLastOrder);
}

// --- context-fill / context-stroke resolution ---

/// Matches a ResolvedPaintServer holding a solid color equal to @p expectedColor.
MATCHER_P(IsSolidPaint, expectedColor, "is solid paint " + testing::PrintToString(expectedColor)) {
  const auto* solid = std::get_if<PaintServer::Solid>(&arg);
  if (!solid) {
    *result_listener << "which is not a solid paint (variant index " << arg.index() << ")";
    return false;
  }
  if (solid->color != expectedColor) {
    *result_listener << "whose color is " << solid->color;
    return false;
  }
  return true;
}

/// Matches a ResolvedPaintServer holding PaintServer::None.
MATCHER(IsNoPaint, "is no paint") {
  if (!std::holds_alternative<PaintServer::None>(arg)) {
    *result_listener << "which holds variant index " << arg.index();
    return false;
  }
  return true;
}

/// Matches a ResolvedPaintServer holding a PaintResolvedReference whose contextRemap matches
/// @p remapMatcher.
MATCHER_P(IsPaintReferenceWithRemap, remapMatcher, "is a paint reference with a context remap") {
  const auto* ref = std::get_if<PaintResolvedReference>(&arg);
  if (!ref) {
    *result_listener << "which is not a paint reference (variant index " << arg.index() << ")";
    return false;
  }
  if (!ref->contextRemap.has_value()) {
    *result_listener << "which has no context remap";
    return false;
  }
  return testing::ExplainMatchResult(remapMatcher, *ref->contextRemap, result_listener);
}

/// Matches a PaintContextRemap field-by-field: bounds, entityFromContextTransform,
/// resolveAtDrawTime, seededFromStroke.
MATCHER_P4(RemapIs, boundsMatcher, entityFromContextMatcher, resolveAtDrawTimeValue,
           seededFromStrokeValue, "") {
  bool matched = true;
  if (!testing::ExplainMatchResult(boundsMatcher, arg.contextBounds, result_listener)) {
    *result_listener << " (contextBounds " << arg.contextBounds << ") ";
    matched = false;
  }
  if (!testing::ExplainMatchResult(entityFromContextMatcher, arg.entityFromContextTransform,
                                   result_listener)) {
    *result_listener << " (entityFromContextTransform " << arg.entityFromContextTransform << ") ";
    matched = false;
  }
  if (arg.resolveAtDrawTime != resolveAtDrawTimeValue) {
    *result_listener << " (resolveAtDrawTime is " << arg.resolveAtDrawTime << ") ";
    matched = false;
  }
  if (arg.seededFromStroke != seededFromStrokeValue) {
    *result_listener << " (seededFromStroke is " << arg.seededFromStroke << ") ";
    matched = false;
  }
  return matched;
}

class RenderingContextContextPaintTest : public RenderingContextTest {
protected:
  /// Find the rendering instance whose data entity carries the given id. Prefers shadow
  /// instances (content instantiated through <use> or markers) over the light instance.
  const RenderingInstanceComponent* findInstanceByDataId(Registry& registry, std::string_view id) {
    const RenderingInstanceComponent* result = nullptr;
    for (const Entity entity : registry.view<RenderingInstanceComponent>()) {
      const auto& instance = registry.get<RenderingInstanceComponent>(entity);
      const auto* idComponent = registry.try_get<IdComponent>(instance.dataEntity);
      if (idComponent && idComponent->id() == id) {
        if (result == nullptr || instance.isShadow(registry)) {
          result = &instance;
        }
      }
    }
    return result;
  }
};

TEST_F(RenderingContextContextPaintTest, UseContextSolidPaintsSwapFillAndStroke) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <path id="consumer" fill="context-stroke" stroke="context-fill" d="M 10 10 H 90 V 90 Z"/>
      </defs>
      <use href="#consumer" fill="blue" stroke="green"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  const auto* instance = findInstanceByDataId(document.registry(), "consumer");
  ASSERT_THAT(instance, NotNull());
  EXPECT_THAT(instance->resolvedFill, IsSolidPaint(css::Color(css::RGBA(0, 0x80, 0, 0xFF))));
  EXPECT_THAT(instance->resolvedStroke, IsSolidPaint(css::Color(css::RGBA(0, 0, 0xFF, 0xFF))));
}

TEST_F(RenderingContextContextPaintTest, ContextPaintWithoutContextElementResolvesToNone) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <rect id="consumer" x="10" y="10" width="80" height="80"
            fill="context-fill" stroke="context-stroke"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  const auto* instance = findInstanceByDataId(document.registry(), "consumer");
  ASSERT_THAT(instance, NotNull());
  EXPECT_THAT(instance->resolvedFill, IsNoPaint());
  EXPECT_THAT(instance->resolvedStroke, IsNoPaint());
}

TEST_F(RenderingContextContextPaintTest, UseContextGradientCarriesRemapWithContextBounds) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <linearGradient id="lg">
        <stop offset="0" stop-color="white"/>
        <stop offset="1" stop-color="green"/>
      </linearGradient>
      <defs>
        <g id="g1">
          <rect id="consumer" x="50" y="50" width="100" height="100" fill="context-fill"/>
          <rect x="30" y="40" width="20" height="20" fill="context-fill"/>
        </g>
      </defs>
      <use href="#g1" fill="url(#lg)"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  const auto* instance = findInstanceByDataId(document.registry(), "consumer");
  ASSERT_THAT(instance, NotNull());

  // The context bounding box is the union of the <use> content in the use's space, and the
  // consumer shares the use's coordinate space (no transforms), so the remap transform is
  // identity and is fully determined at instantiation time.
  EXPECT_THAT(instance->resolvedFill,
              IsPaintReferenceWithRemap(RemapIs(
                  BoxEq(Vector2Eq(30.0, 40.0), Vector2Eq(150.0, 150.0)), TransformIsIdentity(),
                  /*resolveAtDrawTimeValue=*/false, /*seededFromStrokeValue=*/false)));
}

TEST_F(RenderingContextContextPaintTest, UseContextGradientRemapAccountsForConsumerTransform) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <linearGradient id="lg">
        <stop offset="0" stop-color="white"/>
        <stop offset="1" stop-color="green"/>
      </linearGradient>
      <defs>
        <g id="g1" transform="translate(10 20)">
          <rect id="consumer" x="50" y="50" width="100" height="100" fill="context-fill"/>
        </g>
      </defs>
      <use href="#g1" fill="url(#lg)"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  const auto* instance = findInstanceByDataId(document.registry(), "consumer");
  ASSERT_THAT(instance, NotNull());

  // The consumer sits inside a translate(10 20) group; the paint is evaluated in the use
  // element's space, so the remap maps use space -> consumer local space: translate(-10, -20).
  EXPECT_THAT(instance->resolvedFill,
              IsPaintReferenceWithRemap(
                  RemapIs(testing::_, TransformEq(Transform2d::Translate(Vector2d(-10.0, -20.0))),
                          /*resolveAtDrawTimeValue=*/false, /*seededFromStrokeValue=*/false)));
}

TEST_F(RenderingContextContextPaintTest, MarkerContextPaintsResolveAtDrawTime) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <linearGradient id="lg">
        <stop offset="0" stop-color="white"/>
        <stop offset="1" stop-color="green"/>
      </linearGradient>
      <marker id="m1" refX="10" refY="10" markerWidth="20" markerHeight="20">
        <path id="consumer" d="M 10 0 16 20 H 4 Z" fill="context-fill" stroke="context-stroke"/>
      </marker>
      <path id="host" fill="url(#lg)" stroke="green" d="M 20 30 L 180 30 L 100 170 Z"
            marker-start="url(#m1)"/>
    </svg>
  )svg");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  const auto* instance = findInstanceByDataId(document.registry(), "consumer");
  ASSERT_THAT(instance, NotNull());

  // A gradient inherited into marker content is evaluated against the referencing shape's
  // bounding box, with the concrete transform substituted per marker placement at draw time.
  EXPECT_THAT(instance->resolvedFill,
              IsPaintReferenceWithRemap(
                  RemapIs(BoxEq(Vector2Eq(20.0, 30.0), Vector2Eq(180.0, 170.0)), testing::_,
                          /*resolveAtDrawTimeValue=*/true, /*seededFromStrokeValue=*/false)));

  // Solid context paints pass through unchanged.
  EXPECT_THAT(instance->resolvedStroke, IsSolidPaint(css::Color(css::RGBA(0, 0x80, 0, 0xFF))));
}

TEST_F(RenderingContextContextPaintTest, NestedUseTakesInnermostContext) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <defs>
        <path id="consumer" stroke="context-fill" fill="context-stroke"
              d="M 10 10 H 90 V 90 Z"/>
      </defs>
      <use id="use2" href="#consumer" stroke="green" fill="blue"/>
      <use id="use1" href="#use2" stroke="red" fill="yellow"/>
    </svg>
  )");

  RenderingContext ctx(document.registry());
  ctx.instantiateRenderTree(false, warningSink_);

  // Every instance of the consumer (one per <use>) takes its paints from the *innermost*
  // referencing <use> (use2): the context element is the element referencing the shadow tree
  // the consumer was instantiated into.
  int matched = 0;
  Registry& registry = document.registry();
  for (const Entity entity : registry.view<RenderingInstanceComponent>()) {
    const auto& instance = registry.get<RenderingInstanceComponent>(entity);
    const auto* idComponent = registry.try_get<IdComponent>(instance.dataEntity);
    if (!idComponent || idComponent->id() != "consumer" || !instance.isShadow(registry)) {
      continue;
    }

    ++matched;
    EXPECT_THAT(instance.resolvedFill, IsSolidPaint(css::Color(css::RGBA(0, 0x80, 0, 0xFF))));
    EXPECT_THAT(instance.resolvedStroke, IsSolidPaint(css::Color(css::RGBA(0, 0, 0xFF, 0xFF))));
  }
  EXPECT_EQ(matched, 2);
}

}  // namespace donner::svg::components
