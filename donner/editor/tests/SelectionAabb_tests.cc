#include "donner/editor/SelectionAabb.h"

#include <gtest/gtest.h>

#include "donner/base/MathUtils.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectTool.h"
#include "donner/svg/SVGGraphicsElement.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTwoRectSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
            <rect id="r1" x="20"  y="20"  width="40" height="40" fill="red"/>
            <rect id="r2" x="140" y="140" width="20" height="30" fill="blue"/>
          </svg>)svg";

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

TEST(SelectionAabbTest, SnapshotSelectionWorldBoundsMovesWithDrag) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));

  SelectTool tool;

  tool.onMouseDown(app, Vector2d(40.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(app.hasSelection());

  std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  ASSERT_EQ(bounds.size(), 1u);
  EXPECT_EQ(bounds[0], Box2d::FromXYWH(20.0, 20.0, 40.0, 40.0));

  // Drag preview leaves DOM world-bounds unchanged (preview is applied
  // on top, not as a DOM mutation in composited mode).
  tool.onMouseMove(app, Vector2d(70.0, 55.0), /*buttonHeld=*/true);
  ASSERT_TRUE(tool.activeDragPreview().has_value());

  bounds = SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  ASSERT_EQ(bounds.size(), 1u);
  EXPECT_EQ(bounds[0], Box2d::FromXYWH(20.0, 20.0, 40.0, 40.0));
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

  ASSERT_EQ(bounds.size(), 1u);
  const Transform2d documentFromElement =
      triangle->cast<svg::SVGGraphicsElement>().elementFromWorld();
  Box2d expected = Box2d::CreateEmpty(documentFromElement.transformPosition(Vector2d(0.0, 0.0)));
  expected.addPoint(documentFromElement.transformPosition(Vector2d(100.0, 0.0)));
  expected.addPoint(documentFromElement.transformPosition(Vector2d(0.0, 10.0)));

  EXPECT_NEAR(bounds[0].topLeft.x, expected.topLeft.x, 1e-6);
  EXPECT_NEAR(bounds[0].topLeft.y, expected.topLeft.y, 1e-6);
  EXPECT_NEAR(bounds[0].bottomRight.x, expected.bottomRight.x, 1e-6);
  EXPECT_NEAR(bounds[0].bottomRight.y, expected.bottomRight.y, 1e-6);

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

  ASSERT_EQ(bounds.size(), 2u);
  EXPECT_EQ(bounds[0], Box2d::FromXYWH(70.0, 80.0, 10.0, 20.0));
  EXPECT_EQ(bounds[1], Box2d::FromXYWH(100.0, 110.0, 30.0, 40.0));
}

}  // namespace
}  // namespace donner::editor
