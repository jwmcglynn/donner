/**
 * Tests for DonnerController::hitTestLink: resolving the enclosing `<a>` hyperlink at a point, for
 * both click activation and hover affordances. Link hit-testing is DOM/geometry only, so it is
 * exercised identically across renderer variants.
 */

#include "donner/svg/DonnerController.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"

using testing::ElementsAre;
using testing::Eq;
using testing::Optional;

namespace donner::svg {

class DonnerControllerTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    ParseWarningSink parseSink;
    auto maybeResult = parser::SVGParser::ParseSVG(input, parseSink);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }
};

// --- Click activation ---

/// @test A click directly on the painted child of an `<a>` resolves to that link, and reports both
/// the enclosing `<a>` and the concrete element hit.
TEST_F(DonnerControllerTest, ClickInsideLinkDirectChild) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="https://example.com/">
        <rect id="r" x="10" y="10" width="80" height="80" fill="red"/>
      </a>
    </svg>
  )");
  DonnerController controller(document);

  std::optional<DonnerController::LinkHit> link = controller.hitTestLink(Vector2d(50, 50));
  ASSERT_TRUE(link.has_value());
  EXPECT_THAT(link->href, Eq(RcString("https://example.com/")));
  EXPECT_THAT(link->linkElement.href(), Optional(RcString("https://example.com/")));
  // The concrete element under the point is the nested <rect>, not the <a> container.
  EXPECT_THAT(link->hitElement.id(), Eq(RcString("r")));
  EXPECT_EQ(link->hitElement, *document.querySelector("#r"));
}

/// @test A click on a shape nested several levels below an `<a>` (through a `<g>`) still resolves
/// to the enclosing link (SVG enclosing-`<a>` semantics: the whole subtree of a link is clickable).
TEST_F(DonnerControllerTest, ClickOnNestedDescendant) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="#target">
        <g>
          <g>
            <circle id="c" cx="50" cy="50" r="40" fill="blue"/>
          </g>
        </g>
      </a>
    </svg>
  )");
  DonnerController controller(document);

  std::optional<DonnerController::LinkHit> link = controller.hitTestLink(Vector2d(50, 50));
  ASSERT_TRUE(link.has_value());
  EXPECT_THAT(link->href, Eq(RcString("#target")));
  EXPECT_THAT(link->hitElement.id(), Eq(RcString("c")));
}

/// @test A click on a painted element that is not inside any `<a>` returns no link.
TEST_F(DonnerControllerTest, ClickOutsideAnyLink) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="10" y="10" width="80" height="80" fill="green"/>
    </svg>
  )");
  DonnerController controller(document);

  EXPECT_THAT(controller.hitTestLink(Vector2d(50, 50)), Eq(std::nullopt));
}

/// @test A click on empty canvas (no painted element) returns no link.
TEST_F(DonnerControllerTest, ClickOnEmptyArea) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="https://example.com/">
        <rect x="10" y="10" width="20" height="20" fill="red"/>
      </a>
    </svg>
  )");
  DonnerController controller(document);

  // (90, 90) is outside the small rect.
  EXPECT_THAT(controller.hitTestLink(Vector2d(90, 90)), Eq(std::nullopt));
}

// --- Overlap / z-order ---

/// @test When a linked shape is painted on top of another shape, the click resolves to the top
/// link.
TEST_F(DonnerControllerTest, OverlappingTopLinkWins) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="0" y="0" width="100" height="100" fill="green"/>
      <a href="#top">
        <rect id="top" x="20" y="20" width="60" height="60" fill="red"/>
      </a>
    </svg>
  )");
  DonnerController controller(document);

  std::optional<DonnerController::LinkHit> link = controller.hitTestLink(Vector2d(50, 50));
  ASSERT_TRUE(link.has_value());
  EXPECT_THAT(link->href, Eq(RcString("#top")));
  EXPECT_THAT(link->hitElement.id(), Eq(RcString("top")));
}

/// @test An unlinked shape painted on top of a linked shape occludes the link: the topmost element
/// wins, and since it is not inside an `<a>`, no link is returned.
TEST_F(DonnerControllerTest, TopUnlinkedShapeOccludesLink) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="#below">
        <rect x="0" y="0" width="100" height="100" fill="green"/>
      </a>
      <rect x="20" y="20" width="60" height="60" fill="red"/>
    </svg>
  )");
  DonnerController controller(document);

  // Inside the top unlinked rect: occluded, no link.
  EXPECT_THAT(controller.hitTestLink(Vector2d(50, 50)), Eq(std::nullopt));
  // Outside the top rect but still over the linked full-canvas rect: link resolves.
  std::optional<DonnerController::LinkHit> edge = controller.hitTestLink(Vector2d(5, 5));
  ASSERT_TRUE(edge.has_value());
  EXPECT_THAT(edge->href, Eq(RcString("#below")));
}

// --- Transformed coordinates ---

/// @test Link hit-testing accounts for transforms on the link subtree: the query point is in canvas
/// coordinates, and a shape moved by a transform is hit at its transformed location.
TEST_F(DonnerControllerTest, TransformedCoordinates) {
  SVGDocument document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <a href="#moved">
        <rect id="m" x="0" y="0" width="40" height="40" transform="translate(100, 100)" fill="red"/>
      </a>
    </svg>
  )svg");
  DonnerController controller(document);

  // The rect's local (0..40) box is translated to canvas (100..140).
  EXPECT_THAT(controller.hitTestLink(Vector2d(20, 20)), Eq(std::nullopt));
  std::optional<DonnerController::LinkHit> link = controller.hitTestLink(Vector2d(120, 120));
  ASSERT_TRUE(link.has_value());
  EXPECT_THAT(link->href, Eq(RcString("#moved")));
  EXPECT_THAT(link->hitElement.id(), Eq(RcString("m")));
}

// --- href resolution: the raw target is returned verbatim; the app resolves navigation ---

/// @test A fragment target is returned raw.
TEST_F(DonnerControllerTest, HrefFragmentReturnedRaw) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="#section-2"><rect x="0" y="0" width="100" height="100"/></a>
    </svg>
  )");
  DonnerController controller(document);
  EXPECT_THAT(controller.hitTestLink(Vector2d(50, 50))->href, Eq(RcString("#section-2")));
}

/// @test A relative target is returned raw (Donner does not resolve it against the base URL).
TEST_F(DonnerControllerTest, HrefRelativeReturnedRaw) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="../pages/other.svg"><rect x="0" y="0" width="100" height="100"/></a>
    </svg>
  )");
  DonnerController controller(document);
  EXPECT_THAT(controller.hitTestLink(Vector2d(50, 50))->href, Eq(RcString("../pages/other.svg")));
}

/// @test An absolute target is returned raw, including via the legacy `xlink:href` alias.
TEST_F(DonnerControllerTest, HrefAbsoluteViaXlinkReturnedRaw) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
         viewBox="0 0 100 100">
      <a xlink:href="https://example.com/path?q=1"><rect x="0" y="0" width="100" height="100"/></a>
    </svg>
  )");
  DonnerController controller(document);
  EXPECT_THAT(controller.hitTestLink(Vector2d(50, 50))->href,
              Eq(RcString("https://example.com/path?q=1")));
}

/// @test An `<a>` with no target is not a link: a hit inside it returns no link.
TEST_F(DonnerControllerTest, LinkWithoutHrefIsNotALink) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a><rect x="0" y="0" width="100" height="100" fill="red"/></a>
    </svg>
  )");
  DonnerController controller(document);
  EXPECT_THAT(controller.hitTestLink(Vector2d(50, 50)), Eq(std::nullopt));
}

/// @test An `<a>` with an empty href (`href=""`) is not a link, matching `SVGAElement::href()`
/// reporting no target for an empty string.
TEST_F(DonnerControllerTest, EmptyHrefIsNotALink) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href=""><rect x="0" y="0" width="100" height="100" fill="red"/></a>
    </svg>
  )");
  DonnerController controller(document);
  EXPECT_THAT(controller.hitTestLink(Vector2d(50, 50)), Eq(std::nullopt));
}

/// @test Nested `<a>` elements resolve to the innermost enclosing link with a target (nearest
/// ancestor wins).
TEST_F(DonnerControllerTest, NestedLinksResolveToInnermost) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="#outer">
        <a href="#inner">
          <rect x="0" y="0" width="100" height="100" fill="red"/>
        </a>
      </a>
    </svg>
  )");
  DonnerController controller(document);
  std::optional<DonnerController::LinkHit> link = controller.hitTestLink(Vector2d(50, 50));
  ASSERT_TRUE(link.has_value());
  EXPECT_THAT(link->href, Eq(RcString("#inner")));
}

/// @test An outer `<a>` still resolves when an inner `<a>` has no target: the walk skips the
/// targetless inner link and climbs to the outer one.
TEST_F(DonnerControllerTest, TargetlessInnerLinkFallsThroughToOuter) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="#outer">
        <a>
          <rect x="0" y="0" width="100" height="100" fill="red"/>
        </a>
      </a>
    </svg>
  )");
  DonnerController controller(document);
  std::optional<DonnerController::LinkHit> link = controller.hitTestLink(Vector2d(50, 50));
  ASSERT_TRUE(link.has_value());
  EXPECT_THAT(link->href, Eq(RcString("#outer")));
}

/// @test `display: none` on the linked shape removes it from the render tree, so it is not
/// hit-tested and no link is returned.
TEST_F(DonnerControllerTest, DisplayNoneSuppressesLink) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="#x">
        <rect x="0" y="0" width="100" height="100" fill="red" display="none"/>
      </a>
    </svg>
  )");
  DonnerController controller(document);
  EXPECT_THAT(controller.hitTestLink(Vector2d(50, 50)), Eq(std::nullopt));
}

/// @test `pointer-events: none` on the linked shape makes it non-interactive, so no link is
/// returned even though the point is geometrically inside it.
TEST_F(DonnerControllerTest, PointerEventsNoneSuppressesLink) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="#x">
        <rect x="0" y="0" width="100" height="100" fill="red" pointer-events="none"/>
      </a>
    </svg>
  )");
  DonnerController controller(document);
  EXPECT_THAT(controller.hitTestLink(Vector2d(50, 50)), Eq(std::nullopt));
}

TEST_F(DonnerControllerTest, CursorAtReturnsInheritedTypedValue) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g style="cursor: url(primary.cur) 3 4, copy">
        <rect x="10" y="10" width="35" height="80" fill="red"/>
      </g>
      <rect x="55" y="10" width="35" height="80" fill="blue"/>
    </svg>
  )");
  DonnerController controller(document);

  const std::optional<Cursor> inherited = controller.cursorAt(Vector2d(25, 50));
  ASSERT_TRUE(inherited.has_value());
  ASSERT_EQ(inherited->images.size(), 1u);
  EXPECT_EQ(inherited->images[0], (CursorImage{RcString("primary.cur"), Vector2d(3.0, 4.0)}));
  EXPECT_EQ(inherited->fallback, CursorType::Copy);

  const std::optional<Cursor> initial = controller.cursorAt(Vector2d(75, 50));
  ASSERT_TRUE(initial.has_value());
  EXPECT_TRUE(initial->images.empty());
  EXPECT_EQ(initial->fallback, CursorType::Auto);
  EXPECT_EQ(controller.cursorAt(Vector2d(5, 5)), std::nullopt);
}

// --- Hover affordances: the app drives the same query on pointer-move ---

/// @test Hovering inside a link reports it (same query as click), so the app can show a pointer
/// cursor or hover highlight.
TEST_F(DonnerControllerTest, HoverInsideLink) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="https://hover.example/">
        <rect x="10" y="10" width="80" height="80" fill="red"/>
      </a>
    </svg>
  )");
  DonnerController controller(document);
  std::optional<DonnerController::LinkHit> hover = controller.hitTestLink(Vector2d(50, 50));
  ASSERT_TRUE(hover.has_value());
  EXPECT_THAT(hover->href, Eq(RcString("https://hover.example/")));
}

/// @test Hovering over a nested descendant of a link reports the enclosing link, so the whole link
/// region shows the hover affordance.
TEST_F(DonnerControllerTest, HoverOnNestedDescendant) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="#deep">
        <g><rect id="r" x="10" y="10" width="80" height="80" fill="red"/></g>
      </a>
    </svg>
  )");
  DonnerController controller(document);
  std::optional<DonnerController::LinkHit> hover = controller.hitTestLink(Vector2d(50, 50));
  ASSERT_TRUE(hover.has_value());
  EXPECT_THAT(hover->href, Eq(RcString("#deep")));
  EXPECT_THAT(hover->hitElement.id(), Eq(RcString("r")));
}

/// @test As the pointer leaves the link's painted region, the query stops reporting the link, so
/// the app clears the hover affordance.
TEST_F(DonnerControllerTest, HoverLeavingLink) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <a href="#l">
        <rect x="10" y="10" width="30" height="30" fill="red"/>
      </a>
    </svg>
  )");
  DonnerController controller(document);
  // Inside the small rect: link present.
  ASSERT_TRUE(controller.hitTestLink(Vector2d(20, 20)).has_value());
  // Moved outside the rect: link cleared.
  EXPECT_THAT(controller.hitTestLink(Vector2d(80, 80)), Eq(std::nullopt));
}

/// @test Hovering over an area with no link (an unlinked shape) reports nothing.
TEST_F(DonnerControllerTest, HoverWithNoLink) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="10" y="10" width="80" height="80" fill="green"/>
    </svg>
  )");
  DonnerController controller(document);
  EXPECT_THAT(controller.hitTestLink(Vector2d(50, 50)), Eq(std::nullopt));
}

// --- <use> instances: hits resolve to the <use> element in the document tree ---

constexpr std::string_view kUseDocument = R"(
  <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
    <defs><rect id="r" width="10" height="10" fill="blue"/></defs>
    <rect id="base" width="16" height="16" fill="green"/>
    <use id="u" href="#r"/>
  </svg>
)";

/// @test A captured hit snapshot over `<use>` content reports the `<use>` element, not the
/// instantiated shadow content (SVG 2 retargets events on `<use>` instances to the `<use>`).
TEST_F(DonnerControllerTest, CapturedHitOverUseTargetsTheUseElement) {
  SVGDocument document = ParseSVG(kUseDocument);
  DonnerController controller(document);
  const auto snapshot = controller.captureHitTest();
  ASSERT_TRUE(snapshot->complete());
  EXPECT_THAT(snapshot->findIntersecting(Vector2d(5, 5)), Optional(*document.querySelector("#u")));
  EXPECT_THAT(snapshot->findIntersecting(Vector2d(13, 13)),
              Optional(*document.querySelector("#base")));
}

/// @test The live point queries resolve `<use>` content to the `<use>` element, front to back.
TEST_F(DonnerControllerTest, FindIntersectingOverUseTargetsTheUseElement) {
  SVGDocument document = ParseSVG(kUseDocument);
  DonnerController controller(document);
  const SVGElement use = *document.querySelector("#u");
  const SVGElement base = *document.querySelector("#base");
  EXPECT_THAT(controller.findIntersecting(Vector2d(5, 5)), Optional(use));
  EXPECT_THAT(controller.findIntersecting(Vector2d(5, 5), HitTestMode::EditorSelection),
              Optional(use));
  EXPECT_THAT(controller.findAllIntersecting(Vector2d(5, 5)), ElementsAre(use, base));
}

/// @test Overlapping shapes instantiated by one `<use>` report that `<use>` element once.
TEST_F(DonnerControllerTest, FindAllIntersectingReportsAUseElementOnce) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <defs><g id="pair">
        <rect width="10" height="10" fill="blue"/><rect width="8" height="8" fill="red"/>
      </g></defs>
      <rect id="base" width="16" height="16" fill="green"/>
      <use id="u" href="#pair"/>
    </svg>
  )");
  DonnerController controller(document);
  EXPECT_THAT(controller.findAllIntersecting(Vector2d(5, 5)),
              ElementsAre(*document.querySelector("#u"), *document.querySelector("#base")));
}

/// @test Content reached through nested `<use>` elements resolves to the outermost `<use>` in the
/// document tree.
TEST_F(DonnerControllerTest, NestedUseTargetsTheOutermostUseElement) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <defs>
        <rect id="r" width="10" height="10" fill="blue"/>
        <g id="inner-group"><use id="inner" href="#r"/></g>
      </defs>
      <use id="outer" href="#inner-group"/>
    </svg>
  )");
  DonnerController controller(document);
  const SVGElement outer = *document.querySelector("#outer");
  EXPECT_THAT(controller.findIntersecting(Vector2d(5, 5)), Optional(outer));
  EXPECT_THAT(controller.captureHitTest()->findIntersecting(Vector2d(5, 5)), Optional(outer));
}

/// @test Rebuilding the render tree (as every animation tick does) recreates `<use>` shadow content
/// but the hit target stays the same document element.
TEST_F(DonnerControllerTest, UseTargetIsStableAcrossAnimationTicks) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <defs><rect id="r" width="10" height="10" fill="blue">
        <animate attributeName="width" from="10" to="16" dur="1s" repeatCount="indefinite"/>
      </rect></defs>
      <use id="u" href="#r"/>
    </svg>
  )");
  DonnerController controller(document);
  Renderer renderer;
  const SVGElement use = *document.querySelector("#u");
  for (const double time : {0.0, 0.25, 0.5}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    renderer.draw(document);
    const auto snapshot = controller.captureHitTest();
    ASSERT_TRUE(snapshot->complete());
    EXPECT_THAT(snapshot->findIntersecting(Vector2d(5, 5)), Optional(use));
  }
}

/// @test A link enclosing a `<use>` is found through the `<use>` element, which is the hit element.
TEST_F(DonnerControllerTest, LinkEnclosingUseResolvesThroughTheUseElement) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <defs><rect id="r" width="10" height="10" fill="blue"/></defs>
      <a href="#target"><use id="u" href="#r"/></a>
    </svg>
  )");
  DonnerController controller(document);
  const std::optional<DonnerController::LinkHit> link = controller.hitTestLink(Vector2d(5, 5));
  ASSERT_TRUE(link.has_value());
  EXPECT_THAT(link->href, Eq(RcString("#target")));
  EXPECT_EQ(link->hitElement, *document.querySelector("#u"));
}

/// @test The cursor over `<use>` content comes from the instantiated content's own style.
TEST_F(DonnerControllerTest, CursorOverUseUsesTheInstanceStyle) {
  SVGDocument document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <defs><rect id="r" width="10" height="10" fill="blue" style="cursor: copy"/></defs>
      <use href="#r" style="cursor: move"/>
    </svg>
  )");
  DonnerController controller(document);
  const std::optional<Cursor> cursor = controller.cursorAt(Vector2d(5, 5));
  ASSERT_TRUE(cursor.has_value());
  EXPECT_EQ(cursor->fallback, CursorType::Copy);
}

/// @test Pattern and marker content is painted as part of its referencing element and is never a
/// hit target itself; the referencing element is hit by its own geometry.
TEST_F(DonnerControllerTest, PatternAndMarkerContentIsNotAHitTarget) {
  SVGDocument document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
      <defs>
        <pattern id="p" width="20" height="20" patternUnits="userSpaceOnUse">
          <rect width="20" height="20" fill="blue"/>
        </pattern>
        <marker id="m" markerWidth="20" markerHeight="20" markerUnits="userSpaceOnUse"
                overflow="visible">
          <rect width="20" height="20" fill="red"/>
        </marker>
      </defs>
      <rect id="filled" width="20" height="20" fill="url(#p)"/>
      <path id="line" d="M 50 50 L 90 50" stroke="black" marker-start="url(#m)"/>
    </svg>
  )svg");
  DonnerController controller(document);
  const SVGElement filled = *document.querySelector("#filled");
  EXPECT_THAT(controller.findAllIntersecting(Vector2d(5, 5)), ElementsAre(filled));
  EXPECT_THAT(controller.findIntersecting(Vector2d(60, 60)), Eq(std::nullopt));
  const auto snapshot = controller.captureHitTest();
  ASSERT_TRUE(snapshot->complete());
  EXPECT_THAT(snapshot->findIntersecting(Vector2d(5, 5)), Optional(filled));
  EXPECT_THAT(snapshot->findIntersecting(Vector2d(60, 60)), Eq(std::nullopt));
}

}  // namespace donner::svg
