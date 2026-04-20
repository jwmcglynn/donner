#include "donner/editor/SelectionAabb.h"

#include <gtest/gtest.h>

#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectTool.h"

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
  tool.setCompositedDragPreviewEnabled(true);

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

}  // namespace
}  // namespace donner::editor
