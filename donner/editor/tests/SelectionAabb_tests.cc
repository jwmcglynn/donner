#include "donner/editor/SelectionAabb.h"

#include <gtest/gtest.h>

#include <cmath>

#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectTool.h"

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
  ASSERT_TRUE(tool.activeDragPreview().has_value());

  bounds = SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  rects = ComputeSelectionAabbScreenRects(viewport, std::span<const Box2d>(bounds));
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_EQ(rects[0], Box2d::FromXYWH(20.0, 20.0, 40.0, 40.0));

  const Box2d previewRect = Box2d(rects[0].topLeft + tool.activeDragPreview()->translation,
                                  rects[0].bottomRight + tool.activeDragPreview()->translation);
  EXPECT_EQ(previewRect, Box2d::FromXYWH(50.0, 35.0, 40.0, 40.0));
}

}  // namespace
}  // namespace donner::editor
