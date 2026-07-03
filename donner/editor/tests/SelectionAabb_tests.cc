#include "donner/editor/SelectionAabb.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/MathUtils.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectTool.h"
#include "donner/svg/DocumentState.h"
#include "donner/svg/SVGGraphicsElement.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTwoRectSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
            <rect id="r1" x="20"  y="20"  width="40" height="40" fill="red"/>
            <rect id="r2" x="140" y="140" width="20" height="30" fill="blue"/>
          </svg>)svg";

auto BoxFromXYWHIs(double x, double y, double width, double height) {
  return BoxEq(Vector2d(x, y), Vector2d(x + width, y + height));
}

auto BoxNear(const Box2d& expected, double tolerance) {
  return BoxEq(Vector2Eq(testing::DoubleNear(expected.topLeft.x, tolerance),
                         testing::DoubleNear(expected.topLeft.y, tolerance)),
               Vector2Eq(testing::DoubleNear(expected.bottomRight.x, tolerance),
                         testing::DoubleNear(expected.bottomRight.y, tolerance)));
}

TEST(SelectionAabbTest, SnapshotSelectionWorldBoundsSkipsNonGeometryWithoutBounds) {
  constexpr std::string_view kMixedSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
              <g id="group"><title>label</title></g>
              <rect id="rect" x="10" y="15" width="20" height="25" fill="red"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kMixedSvg));
  auto group = app.document().document().querySelector("#group");
  auto rect = app.document().document().querySelector("#rect");
  ASSERT_TRUE(group.has_value());
  ASSERT_TRUE(rect.has_value());

  const std::vector<svg::SVGElement> selection = {*group, *rect};
  const std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(selection));

  EXPECT_THAT(bounds, testing::ElementsAre(BoxFromXYWHIs(10.0, 15.0, 20.0, 25.0)));
}

TEST(SelectionAabbTest, SnapshotBoundsAllowConcurrentDom) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  const std::vector<svg::SVGElement> selection = {*rect};

  const std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(selection));

  EXPECT_THAT(bounds, testing::ElementsAre(BoxFromXYWHIs(20.0, 20.0, 40.0, 40.0)));
}

TEST(SelectionAabbTest, SnapshotWorldBoundsRejectsSourcelessElements) {
  svg::SVGDocument document;
  const svg::SVGElement root = document.svgElement();
  const std::vector<svg::SVGElement> selection = {root};

  EXPECT_TRUE(CollectRenderableGeometry(root).empty());
  EXPECT_TRUE(SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(selection)).empty());
}

TEST(SelectionAabbTest, SnapshotSelectionWorldBoundsMovesWithDrag) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));

  SelectTool tool;

  tool.onMouseDown(app, Vector2d(40.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(app.hasSelection());

  std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  EXPECT_THAT(bounds, testing::ElementsAre(BoxFromXYWHIs(20.0, 20.0, 40.0, 40.0)));

  // Drag preview leaves DOM world-bounds unchanged (preview is applied
  // on top, not as a DOM mutation in composited mode).
  tool.onMouseMove(app, Vector2d(70.0, 55.0), /*buttonHeld=*/true);
  ASSERT_TRUE(tool.activeDragPreview().has_value());

  bounds = SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  EXPECT_THAT(bounds, testing::ElementsAre(BoxFromXYWHIs(20.0, 20.0, 40.0, 40.0)));
}

TEST(SelectionAabbTest, SnapshotUnionsGeometryDescendantsForGroupSelection) {
  constexpr std::string_view kGroupedSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <defs>
                <filter id="blur"><feGaussianBlur stdDeviation="2"/></filter>
              </defs>
              <g id="grp" filter="url(#blur)">
                <rect id="child_a" x="20"  y="30"  width="40" height="40" fill="red"/>
                <rect id="child_b" x="140" y="150" width="20" height="30" fill="blue"/>
              </g>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kGroupedSvg));
  auto group = app.document().document().querySelector("#grp");
  ASSERT_TRUE(group.has_value());

  const std::vector<svg::SVGElement> selection = {*group};
  const std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(selection));

  // Union of (20,30,40,40) ∪ (140,150,20,30) → (20,30)-(160,180).
  EXPECT_THAT(bounds, testing::ElementsAre(BoxFromXYWHIs(20.0, 30.0, 140.0, 150.0)));
}

TEST(SelectionAabbTest, SnapshotSkipsNonRenderedContainerDescendants) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <g id="grp">
                <defs>
                  <clipPath id="clip">
                    <rect id="hidden" x="5" y="5" width="5" height="5"/>
                  </clipPath>
                </defs>
                <rect id="visible" x="80" y="80" width="40" height="40" fill="green"/>
              </g>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  auto group = app.document().document().querySelector("#grp");
  ASSERT_TRUE(group.has_value());

  const std::vector<svg::SVGElement> selection = {*group};
  const std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(selection));

  // Hidden rect inside <defs><clipPath> must not contribute; the
  // bounds are just the visible child.
  EXPECT_THAT(bounds, testing::ElementsAre(BoxFromXYWHIs(80.0, 80.0, 40.0, 40.0)));
}

TEST(SelectionAabbTest, SnapshotSkipsGeometryWithoutWorldBounds) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <g id="grp">
                <path id="empty" d=""/>
                <rect id="visible" x="80" y="80" width="40" height="40" fill="green"/>
              </g>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  auto group = app.document().document().querySelector("#grp");
  ASSERT_TRUE(group.has_value());

  const std::vector<svg::SVGElement> selection = {*group};
  const std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(selection));

  ASSERT_EQ(bounds.size(), 1u);
  EXPECT_EQ(bounds[0], Box2d::FromXYWH(80.0, 80.0, 40.0, 40.0));
}

TEST(SelectionAabbTest, CollectRenderableGeometrySkipsAllNonRenderedContainerTypes) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <defs id="defs">
                <rect id="defs_rect" x="1" y="1" width="2" height="2"/>
              </defs>
              <clipPath id="clip"><rect id="clip_rect" x="2" y="2" width="2" height="2"/></clipPath>
              <mask id="mask"><rect id="mask_rect" x="3" y="3" width="2" height="2"/></mask>
              <filter id="filter"><feGaussianBlur stdDeviation="1"/></filter>
              <pattern id="pattern"><rect id="pattern_rect" x="4" y="4" width="2" height="2"/></pattern>
              <linearGradient id="linear"><stop offset="0"/></linearGradient>
              <radialGradient id="radial"><stop offset="1"/></radialGradient>
              <symbol id="symbol"><rect id="symbol_rect" x="5" y="5" width="2" height="2"/></symbol>
              <marker id="marker"><path id="marker_path" d="M0 0 L1 1"/></marker>
              <style id="style">rect { fill: red; }</style>
              <rect id="visible" x="80" y="80" width="40" height="40" fill="green"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  constexpr std::string_view kContainerIds[] = {
      "defs", "clip", "mask", "filter", "pattern", "linear", "radial", "symbol", "marker", "style",
  };
  for (std::string_view id : kContainerIds) {
    auto container = app.document().document().querySelector(std::string("#") + std::string(id));
    ASSERT_TRUE(container.has_value()) << id;
    EXPECT_TRUE(CollectRenderableGeometry(*container).empty()) << id;
  }

  const std::vector<svg::SVGGeometryElement> geometry =
      CollectRenderableGeometry(app.document().document().svgElement());
  ASSERT_EQ(geometry.size(), 1u);
  EXPECT_EQ(geometry.front().id(), "visible");
}

TEST(SelectionAabbTest, SnapshotUsesTightTransformedPathBounds) {
  constexpr std::string_view kTriangleSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="-20 0 120 120">
              <path id="triangle" d="M 0 0 L 100 0 L 0 10 Z" fill="red"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTriangleSvg));
  auto triangle = app.document().document().querySelector("#triangle");
  ASSERT_TRUE(triangle.has_value());
  triangle->cast<svg::SVGGraphicsElement>().setTransform(
      Transform2d::Rotate(MathConstants<double>::kPi / 4.0));

  const std::vector<svg::SVGElement> selection = {*triangle};
  const std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(selection));

  const Transform2d documentFromElement =
      triangle->cast<svg::SVGGraphicsElement>().elementFromWorld();
  Box2d expected = Box2d::CreateEmpty(documentFromElement.transformPosition(Vector2d(0.0, 0.0)));
  expected.addPoint(documentFromElement.transformPosition(Vector2d(100.0, 0.0)));
  expected.addPoint(documentFromElement.transformPosition(Vector2d(0.0, 10.0)));

  ASSERT_THAT(bounds, testing::ElementsAre(BoxNear(expected, 1e-6)));

  const Box2d looseBounds =
      documentFromElement.transformBox(Box2d::FromXYWH(0.0, 0.0, 100.0, 10.0));
  EXPECT_LT(bounds[0].height(), looseBounds.height())
      << "settled selection bounds must be the tight transformed path, not the selected "
         "path's transformed local AABB";
}

TEST(SelectionAabbTest, SnapshotOccludingWorldBoundsIncludesOnlyLaterPaintedGeometry) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <rect id="behind" x="1" y="1" width="2" height="3" fill="black"/>
              <g id="selected">
                <rect id="selected_child" x="20" y="20" width="40" height="40" fill="red"/>
              </g>
              <rect id="front_a" x="70" y="80" width="10" height="20" fill="blue"/>
              <g id="front_group">
                <rect id="front_b" x="100" y="110" width="30" height="40" fill="green"/>
              </g>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  auto selected = app.document().document().querySelector("#selected");
  ASSERT_TRUE(selected.has_value());

  const std::vector<svg::SVGElement> selection = {*selected};
  const std::vector<Box2d> bounds =
      SnapshotSelectionOccludingWorldBounds(std::span<const svg::SVGElement>(selection));

  EXPECT_THAT(bounds, testing::ElementsAre(BoxFromXYWHIs(70.0, 80.0, 10.0, 20.0),
                                           BoxFromXYWHIs(100.0, 110.0, 30.0, 40.0)));
}

TEST(SelectionAabbTest, SnapshotOccludingWorldBoundsRejectsEmptyAndMultiSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  EXPECT_TRUE(SnapshotSelectionOccludingWorldBounds({}).empty());

  const std::vector<svg::SVGElement> multiSelection = {*r1, *r2};
  EXPECT_TRUE(
      SnapshotSelectionOccludingWorldBounds(std::span<const svg::SVGElement>(multiSelection))
          .empty());
}

TEST(SelectionAabbTest, SnapshotOccludingWorldBoundsRejectsSourcelessElements) {
  svg::SVGDocument document;
  const svg::SVGElement root = document.svgElement();
  const std::vector<svg::SVGElement> selection = {root};

  EXPECT_TRUE(
      SnapshotSelectionOccludingWorldBounds(std::span<const svg::SVGElement>(selection)).empty());
}

TEST(SelectionAabbTest, RefreshSelectionBoundsCachePromotesOnlyDisplayedVersionAndClearsEmpty) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  SelectionBoundsCache cache;
  const std::vector<svg::SVGElement> selection = {*rect};
  RefreshSelectionBoundsCache(cache, std::span<const svg::SVGElement>(selection),
                              /*currentDocVersion=*/7, /*displayedDocVersion=*/6);

  ASSERT_EQ(cache.pendingBoundsDoc.size(), 1u);
  EXPECT_TRUE(cache.displayedBoundsDoc.empty());
  PromoteSelectionBoundsIfReady(cache, /*displayedDocVersion=*/6);
  EXPECT_TRUE(cache.displayedBoundsDoc.empty());

  PromoteSelectionBoundsIfReady(cache, /*displayedDocVersion=*/7);
  ASSERT_EQ(cache.displayedBoundsDoc.size(), 1u);
  EXPECT_TRUE(cache.pendingBoundsDoc.empty());
  EXPECT_EQ(cache.pendingVersion, 0u);

  RefreshSelectionBoundsCache(cache, {}, /*currentDocVersion=*/8, /*displayedDocVersion=*/8);
  EXPECT_TRUE(cache.lastSelection.empty());
  EXPECT_TRUE(cache.displayedBoundsDoc.empty());
  EXPECT_TRUE(cache.displayedOccludingBoundsDoc.empty());
}

TEST(SelectionAabbTest, SnapshotSelectionWorldBoundsIncludesTextInkBounds) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <text id="t" x="50" y="80" font-size="20" font-family="sans-serif">Hello</text>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  auto text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());

  const std::vector<svg::SVGElement> selection = {*text};
  const std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(selection));

  // The text root contributes its laid-out ink bounds: glyphs start at the
  // x="50" pen position and sit on the y="80" baseline (ascenders above it).
  ASSERT_EQ(bounds.size(), 1u);
  EXPECT_GT(bounds[0].size().x, 0.0);
  EXPECT_GT(bounds[0].size().y, 0.0);
  EXPECT_NEAR(bounds[0].topLeft.x, 50.0, 3.0);
  EXPECT_LT(bounds[0].topLeft.y, 80.0);
  EXPECT_GT(bounds[0].bottomRight.y, 60.0);
}

TEST(SelectionAabbTest, SnapshotSelectionWorldBoundsMapsTextThroughItsTransform) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 400">
              <text id="plain" x="50" y="80" font-size="20" font-family="sans-serif">Hello</text>
              <text id="moved" x="50" y="80" font-size="20" font-family="sans-serif"
                    transform="translate(100 40)">Hello</text>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  auto plain = app.document().document().querySelector("#plain");
  auto moved = app.document().document().querySelector("#moved");
  ASSERT_TRUE(plain.has_value());
  ASSERT_TRUE(moved.has_value());

  const std::vector<svg::SVGElement> selection = {*plain, *moved};
  const std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(selection));

  ASSERT_EQ(bounds.size(), 2u);
  EXPECT_NEAR(bounds[1].topLeft.x, bounds[0].topLeft.x + 100.0, 1e-6);
  EXPECT_NEAR(bounds[1].topLeft.y, bounds[0].topLeft.y + 40.0, 1e-6);
}

TEST(SelectionAabbTest, SnapshotSelectionWorldBoundsIncludesTextInsideSelectedGroup) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <g id="group">
                <rect x="20" y="20" width="40" height="40" fill="red"/>
                <text x="50" y="180" font-size="20" font-family="sans-serif">Hello</text>
              </g>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  auto group = app.document().document().querySelector("#group");
  ASSERT_TRUE(group.has_value());

  const std::vector<svg::SVGElement> selection = {*group};
  const std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(selection));

  // The group AABB unions the rect with the text ink bounds, so it must
  // extend down to the text sitting on the y="180" baseline.
  ASSERT_EQ(bounds.size(), 1u);
  EXPECT_LE(bounds[0].topLeft.y, 20.0 + 1e-6);
  EXPECT_GT(bounds[0].bottomRight.y, 160.0);
}

TEST(SelectionAabbTest, SnapshotSelectionWorldBoundsUsesAuthoredBoxForBoxText) {
  // Box text: the selection rect is the region the user dragged (recorded as
  // data-donner-text-box-*), not the laid-out glyph ink. The box top edge
  // sits one font-size above the first baseline.
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 400">
              <text id="t" x="50" y="80" font-size="20" font-family="sans-serif"
                    data-donner-text-box-width="200" data-donner-text-box-height="120">Hi</text>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  auto text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());

  const std::vector<svg::SVGElement> selection = {*text};
  const std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(selection));

  ASSERT_EQ(bounds.size(), 1u);
  EXPECT_NEAR(bounds[0].topLeft.x, 50.0, 1e-6);
  EXPECT_NEAR(bounds[0].topLeft.y, 60.0, 1e-6);
  EXPECT_NEAR(bounds[0].size().x, 200.0, 1e-6);
  EXPECT_NEAR(bounds[0].size().y, 120.0, 1e-6);
}

TEST(SelectionAabbTest, TextWorldFrameBoundsMapsAuthoredBoxThroughTransform) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 400">
              <text id="t" x="50" y="80" font-size="20" font-family="sans-serif"
                    data-donner-text-box-width="200" data-donner-text-box-height="120"
                    transform="translate(100 40)">Hi</text>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  auto text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());

  const std::optional<Box2d> frame = text->cast<svg::SVGTextElement>().withWriteAccess(
      [&text](svg::DocumentWriteAccess&, EntityHandle) {
        return TextWorldFrameBounds(text->cast<svg::SVGTextElement>());
      });

  ASSERT_TRUE(frame.has_value());
  EXPECT_NEAR(frame->topLeft.x, 150.0, 1e-6);
  EXPECT_NEAR(frame->topLeft.y, 100.0, 1e-6);
  EXPECT_NEAR(frame->size().x, 200.0, 1e-6);
  EXPECT_NEAR(frame->size().y, 120.0, 1e-6);
}

TEST(SelectionAabbTest, TextWorldFrameBoundsFallsBackToInkForPointText) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <text id="t" x="50" y="80" font-size="20" font-family="sans-serif">Hello</text>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  auto text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());

  const auto boundsOf = [&](auto&& fn) {
    return text->cast<svg::SVGTextElement>().withWriteAccess(
        [&text, &fn](svg::DocumentWriteAccess&, EntityHandle) {
          return fn(text->cast<svg::SVGTextElement>());
        });
  };
  const std::optional<Box2d> frame =
      boundsOf([](const svg::SVGTextElement& element) { return TextWorldFrameBounds(element); });
  const std::optional<Box2d> ink =
      boundsOf([](const svg::SVGTextElement& element) { return TextWorldInkBounds(element); });

  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(ink.has_value());
  EXPECT_EQ(*frame, *ink);
}

TEST(SelectionAabbTest, SnapshotOccludingBoundsAllowConcurrentDom) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <rect id="selected" x="20" y="20" width="40" height="40" fill="red"/>
              <rect id="front" x="70" y="80" width="10" height="20" fill="blue"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);
  auto selected = app.document().document().querySelector("#selected");
  ASSERT_TRUE(selected.has_value());

  const std::vector<svg::SVGElement> selection = {*selected};
  const std::vector<Box2d> bounds =
      SnapshotSelectionOccludingWorldBounds(std::span<const svg::SVGElement>(selection));

  EXPECT_THAT(bounds, testing::ElementsAre(BoxFromXYWHIs(70.0, 80.0, 10.0, 20.0)));
}

}  // namespace
}  // namespace donner::editor
