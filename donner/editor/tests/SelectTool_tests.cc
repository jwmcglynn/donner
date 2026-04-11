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

  svg::SVGElement elementById(std::string_view id) {
    auto element = app.document().document().querySelector(id);
    EXPECT_TRUE(element.has_value()) << "no element matching " << id;
    return *element;
  }

  Transform2d transformOf(std::string_view id) {
    auto element = app.document().document().querySelector(id);
    EXPECT_TRUE(element.has_value());
    return element->cast<svg::SVGGraphicsElement>().transform();
  }

  bool selectionIs(std::string_view id) {
    const auto& selection = app.selectedElement();
    if (!selection.has_value()) {
      return false;
    }
    return *selection == elementById(id);
  }

  EditorApp app;
  SelectTool tool;
};

TEST_F(SelectToolTest, ClickInsideElementSelectsIt) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));

  EXPECT_TRUE(app.hasSelection());
  EXPECT_TRUE(selectionIs("#r1"));
  EXPECT_TRUE(tool.isDragging());
}

TEST_F(SelectToolTest, ClickInEmptySpaceClearsSelection) {
  app.setSelection(elementById("#r1"));
  EXPECT_TRUE(app.hasSelection());

  tool.onMouseDown(app, Vector2d(180.0, 180.0));
  EXPECT_FALSE(app.hasSelection());
  EXPECT_FALSE(tool.isDragging());
}

TEST_F(SelectToolTest, ClickOnDifferentElementSwitchesSelection) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  ASSERT_TRUE(selectionIs("#r1"));

  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  tool.onMouseDown(app, Vector2d(110.0, 110.0));

  EXPECT_TRUE(selectionIs("#r2"));
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

TEST_F(SelectToolTest, DragWithMoveRecordsUndoEntry) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  tool.onMouseMove(app, Vector2d(40.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(40.0, 35.0));

  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  EXPECT_TRUE(app.canUndo());
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Move element");
}

TEST_F(SelectToolTest, ClickWithoutDragDoesNotRecordUndo) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  tool.onMouseUp(app, Vector2d(15.0, 15.0));

  EXPECT_EQ(app.undoTimeline().entryCount(), 0u);
  EXPECT_FALSE(app.canUndo());
}

TEST_F(SelectToolTest, UndoRestoresElementToPreDragPosition) {
  // Drag r1 by (25, 20) and flush.
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  tool.onMouseMove(app, Vector2d(40.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(40.0, 35.0));
  ASSERT_TRUE(app.flushFrame());

  // Undo through EditorApp (which routes through the command queue).
  app.undo();
  ASSERT_TRUE(app.flushFrame());

  // r1's transform should be back to identity.
  const Transform2d after = transformOf("#r1");
  EXPECT_DOUBLE_EQ(after.data[4], 0.0);
  EXPECT_DOUBLE_EQ(after.data[5], 0.0);
}

TEST_F(SelectToolTest, UndoOfUndoReappliesTheDrag) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  tool.onMouseMove(app, Vector2d(40.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(40.0, 35.0));
  ASSERT_TRUE(app.flushFrame());

  // Undo the drag.
  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[4], 0.0);

  // Break the chain and undo again — should re-apply the drag delta.
  app.undoTimeline().breakUndoChain();
  app.undo();
  ASSERT_TRUE(app.flushFrame());

  const Transform2d after = transformOf("#r1");
  EXPECT_DOUBLE_EQ(after.data[4], 25.0);
  EXPECT_DOUBLE_EQ(after.data[5], 20.0);
}

TEST_F(SelectToolTest, MultiStepDragCollapsesToSingleUndoEntry) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  tool.onMouseMove(app, Vector2d(20.0, 15.0), /*buttonHeld=*/true);
  tool.onMouseMove(app, Vector2d(30.0, 20.0), /*buttonHeld=*/true);
  tool.onMouseMove(app, Vector2d(50.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 35.0));

  // 60fps drag → many SetTransform commands, but exactly one undo entry.
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
}

TEST_F(SelectToolTest, RedoAfterUndoRestoresPostDragState) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  tool.onMouseMove(app, Vector2d(40.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(40.0, 35.0));
  ASSERT_TRUE(app.flushFrame());

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[4], 0.0);
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[5], 0.0);

  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[4], 25.0);
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[5], 20.0);
}

TEST_F(SelectToolTest, UndoRedoCyclesStayConsistent) {
  // Drag by (25, 20).
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  tool.onMouseMove(app, Vector2d(40.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(40.0, 35.0));
  ASSERT_TRUE(app.flushFrame());

  // Three undo/redo cycles — each pair should leave the element at its
  // post-drag position.
  for (int i = 0; i < 3; ++i) {
    app.undo();
    ASSERT_TRUE(app.flushFrame());
    EXPECT_DOUBLE_EQ(transformOf("#r1").data[4], 0.0) << "iteration " << i;

    app.redo();
    ASSERT_TRUE(app.flushFrame());
    EXPECT_DOUBLE_EQ(transformOf("#r1").data[4], 25.0) << "iteration " << i;
  }
}

TEST_F(SelectToolTest, RedoWithNothingToRedoIsNoOp) {
  // Nothing in the timeline → redo is a no-op.
  app.redo();
  EXPECT_FALSE(app.flushFrame());
}

TEST_F(SelectToolTest, TwoDifferentDragsBothUndoableInOrder) {
  // Drag r1 by (25, 20).
  tool.onMouseDown(app, Vector2d(15.0, 15.0));
  tool.onMouseMove(app, Vector2d(40.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(40.0, 35.0));
  ASSERT_TRUE(app.flushFrame());

  // Drag r2 by (10, 5).
  tool.onMouseDown(app, Vector2d(120.0, 120.0));
  tool.onMouseMove(app, Vector2d(130.0, 125.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(130.0, 125.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(app.undoTimeline().entryCount(), 2u);

  // Undo: r2 back to identity, r1 still moved.
  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_DOUBLE_EQ(transformOf("#r2").data[4], 0.0);
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[4], 25.0);

  // Undo again: r1 back to identity too.
  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[4], 0.0);
  EXPECT_DOUBLE_EQ(transformOf("#r2").data[4], 0.0);
}

}  // namespace
}  // namespace donner::editor
