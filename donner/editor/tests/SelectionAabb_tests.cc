#include "donner/editor/SelectionAabb.h"

#include <gtest/gtest.h>

#include <cmath>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/backend_lib/SelectTool.h"
#include "donner/editor/backend_lib/Tool.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTwoRectSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
            <rect id="r1" x="20"  y="20"  width="40" height="40" fill="red"/>
            <rect id="r2" x="140" y="140" width="20" height="30" fill="blue"/>
          </svg>)svg";

::testing::AssertionResult NearBox(const Box2d& actual, const Box2d& expected, double tolerance) {
  const auto near = [tolerance](double a, double b) { return std::abs(a - b) <= tolerance; };
  if (!near(actual.topLeft.x, expected.topLeft.x) || !near(actual.topLeft.y, expected.topLeft.y) ||
      !near(actual.bottomRight.x, expected.bottomRight.x) ||
      !near(actual.bottomRight.y, expected.bottomRight.y)) {
    return ::testing::AssertionFailure()
           << "actual=" << actual << " expected=" << expected << " tolerance=" << tolerance;
  }

  return ::testing::AssertionSuccess();
}

#define EXPECT_NEAR_BOX(actual, expected, tolerance) \
  EXPECT_TRUE(NearBox((actual), (expected), (tolerance)))

ViewportState MakeViewport() {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d::Zero();
  viewport.paneSize = Vector2d(200.0, 200.0);
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0);
  viewport.resetTo100Percent();
  return viewport;
}

TEST(SelectionAabbTest, ComputesScreenRectsForEachSelectionBoundsAndCombined) {
  ViewportState viewport = MakeViewport();
  viewport.zoomAround(2.0, viewport.paneCenter());
  viewport.panBy(Vector2d(15.0, -5.0));

  const std::vector<Box2d> boundsDoc = {
      Box2d::FromXYWH(20.0, 20.0, 40.0, 40.0),
      Box2d::FromXYWH(140.0, 140.0, 20.0, 30.0),
  };

  const std::vector<Box2d> rects =
      ComputeSelectionAabbScreenRects(viewport, std::span<const Box2d>(boundsDoc));

  ASSERT_EQ(rects.size(), 3u);
  EXPECT_NEAR_BOX(rects[0], viewport.documentToScreen(boundsDoc[0]), 1e-9);
  EXPECT_NEAR_BOX(rects[1], viewport.documentToScreen(boundsDoc[1]), 1e-9);
  EXPECT_NEAR_BOX(rects[2], viewport.documentToScreen(Box2d::FromXYWH(20.0, 20.0, 140.0, 150.0)),
                  1e-9);
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

  ASSERT_EQ(bounds.size(), 1u);
  EXPECT_EQ(bounds[0], Box2d::FromXYWH(10.0, 15.0, 20.0, 25.0));
}

TEST(SelectionAabbTest, SelectionChangeThenDragRefreshProducesUpdatedRects) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));

  ViewportState viewport = MakeViewport();
  SelectTool tool;

  const Vector2d startScreen(40.0, 40.0);
  tool.onMouseDown(app, viewport.screenToDocument(startScreen), MouseModifiers{});
  ASSERT_TRUE(app.hasSelection());

  std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  std::vector<Box2d> rects =
      ComputeSelectionAabbScreenRects(viewport, std::span<const Box2d>(bounds));
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0], Box2d::FromXYWH(20.0, 20.0, 40.0, 40.0));

  const Vector2d dragTargetScreen = startScreen + Vector2d(30.0, 15.0);
  tool.onMouseMove(app, viewport.screenToDocument(dragTargetScreen), /*buttonHeld=*/true);
  app.flushFrame();

  bounds = SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  rects = ComputeSelectionAabbScreenRects(viewport, std::span<const Box2d>(bounds));
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0], Box2d::FromXYWH(50.0, 35.0, 40.0, 40.0));
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

  ASSERT_EQ(bounds.size(), 1u);
  // Union of (20,30,40,40) ∪ (140,150,20,30) → (20,30)-(160,180).
  EXPECT_EQ(bounds[0], Box2d::FromXYWH(20.0, 30.0, 140.0, 150.0));
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

  ASSERT_EQ(bounds.size(), 1u);
  // Hidden rect inside <defs><clipPath> must not contribute; the
  // bounds are just the visible child.
  EXPECT_EQ(bounds[0], Box2d::FromXYWH(80.0, 80.0, 40.0, 40.0));
}

}  // namespace
}  // namespace donner::editor
