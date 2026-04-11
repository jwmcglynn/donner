#include "donner/editor/SelectTool.h"

#include "donner/editor/EditorApp.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTwoRectsSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
         <rect id="r1" x="10" y="10" width="20" height="20" fill="red"/>
         <rect id="r2" x="100" y="100" width="40" height="40" fill="blue"/>
       </svg>)";

class SelectToolTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(app.loadFromString(kTwoRectsSvg));
  }

  Entity entityById(std::string_view id) {
    auto element = app.document().document().querySelector(id);
    EXPECT_TRUE(element.has_value()) << "no element matching " << id;
    return element->entityHandle().entity();
  }

  Transform2d transformOf(std::string_view id) {
    auto element = app.document().document().querySelector(id);
    EXPECT_TRUE(element.has_value());
    return element->cast<svg::SVGGraphicsElement>().transform();
  }

  EditorApp app;
  SelectTool tool;
};

TEST_F(SelectToolTest, ClickInsideElementSelectsIt) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));

  EXPECT_TRUE(app.hasSelection());
  EXPECT_EQ(app.selectedEntity(), entityById("#r1"));
  EXPECT_TRUE(tool.isDragging());
}

TEST_F(SelectToolTest, ClickInEmptySpaceClearsSelection) {
  app.setSelection(entityById("#r1"));
  EXPECT_TRUE(app.hasSelection());

  tool.onMouseDown(app, Vector2d(180.0, 180.0));
  EXPECT_FALSE(app.hasSelection());
  EXPECT_FALSE(tool.isDragging());
}

TEST_F(SelectToolTest, ClickOnDifferentElementSwitchesSelection) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  ASSERT_EQ(app.selectedEntity(), entityById("#r1"));

  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  tool.onMouseDown(app, Vector2d(110.0, 110.0));

  EXPECT_EQ(app.selectedEntity(), entityById("#r2"));
}

TEST_F(SelectToolTest, DragTranslatesSelectedElement) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  tool.onMouseMove(app, Vector2d(40.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(40.0, 35.0));

  ASSERT_TRUE(app.flushFrame());

  // Drag delta is (25, 20) → element's local transform should now reflect
  // a parent-space translation by that amount.
  const Transform2d after = transformOf("#r1");
  EXPECT_DOUBLE_EQ(after.data[4], 25.0);
  EXPECT_DOUBLE_EQ(after.data[5], 20.0);
}

TEST_F(SelectToolTest, MultipleMoveEventsCoalesceToFinalDelta) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  tool.onMouseMove(app, Vector2d(20.0, 15.0), /*buttonHeld=*/true);
  tool.onMouseMove(app, Vector2d(30.0, 20.0), /*buttonHeld=*/true);
  tool.onMouseMove(app, Vector2d(50.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 35.0));

  // Three SetTransform commands queued, all for the same entity.
  EXPECT_EQ(app.document().queue().size(), 3u);
  ASSERT_TRUE(app.flushFrame());

  // After flush, the final transform reflects only the last move's delta:
  // start=(15,15), end=(50,35) → delta=(35,20).
  const Transform2d after = transformOf("#r1");
  EXPECT_DOUBLE_EQ(after.data[4], 35.0);
  EXPECT_DOUBLE_EQ(after.data[5], 20.0);
}

TEST_F(SelectToolTest, MoveWithoutButtonHeldIsHover) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  tool.onMouseUp(app, Vector2d(15.0, 15.0));

  // Hover after the drag — should NOT translate the element.
  tool.onMouseMove(app, Vector2d(50.0, 50.0), /*buttonHeld=*/false);

  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_FALSE(tool.isDragging());
}

TEST_F(SelectToolTest, MoveWithoutDownIsIgnored) {
  // Mouse move with button held but no preceding mouse-down — could happen
  // if the user starts dragging from outside the viewport. Should not
  // crash and should not produce any commands.
  tool.onMouseMove(app, Vector2d(50.0, 50.0), /*buttonHeld=*/true);

  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_FALSE(tool.isDragging());
}

TEST_F(SelectToolTest, MissedClickEndsAnyPriorDragSilently) {
  // Drag in progress on r1...
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  EXPECT_TRUE(tool.isDragging());

  // ...interrupted by a click in empty space (e.g. tool refocus).
  tool.onMouseDown(app, Vector2d(180.0, 180.0));
  EXPECT_FALSE(tool.isDragging());
  EXPECT_FALSE(app.hasSelection());
}

}  // namespace
}  // namespace donner::editor
