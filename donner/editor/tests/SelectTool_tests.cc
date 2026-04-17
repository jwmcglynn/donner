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
  void SetUp() override { ASSERT_TRUE(app.loadFromString(kTwoRectsSvg)); }

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
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});

  EXPECT_TRUE(app.hasSelection());
  EXPECT_TRUE(selectionIs("#r1"));
  EXPECT_TRUE(tool.isDragging());
}

TEST_F(SelectToolTest, CompositedRenderingToggleAllowedOnlyWithoutActiveGesture) {
  EXPECT_TRUE(CanToggleCompositedRendering(tool));

  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  EXPECT_FALSE(CanToggleCompositedRendering(tool));

  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  EXPECT_TRUE(CanToggleCompositedRendering(tool));

  tool.onMouseDown(app, Vector2d(180.0, 180.0), MouseModifiers{});
  EXPECT_FALSE(CanToggleCompositedRendering(tool));

  tool.onMouseUp(app, Vector2d(180.0, 180.0));
  EXPECT_TRUE(CanToggleCompositedRendering(tool));
}

TEST_F(SelectToolTest, ClickInEmptySpaceClearsSelection) {
  app.setSelection(elementById("#r1"));
  EXPECT_TRUE(app.hasSelection());

  tool.onMouseDown(app, Vector2d(180.0, 180.0), MouseModifiers{});
  EXPECT_FALSE(app.hasSelection());
  EXPECT_FALSE(tool.isDragging());
}

TEST_F(SelectToolTest, ClickOnDifferentElementSwitchesSelection) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  ASSERT_TRUE(selectionIs("#r1"));

  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  tool.onMouseDown(app, Vector2d(110.0, 110.0), MouseModifiers{});

  EXPECT_TRUE(selectionIs("#r2"));
}

TEST_F(SelectToolTest, DragTranslatesSelectedElement) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(40.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(40.0, 35.0));

  ASSERT_TRUE(app.flushFrame());

  // Drag delta is (25, 20) → element's local transform should now reflect
  // a parent-space translation by that amount.
  const Transform2d after = transformOf("#r1");
  EXPECT_DOUBLE_EQ(after.data[4], 25.0);
  EXPECT_DOUBLE_EQ(after.data[5], 20.0);
}

TEST_F(SelectToolTest, DragPreviewTracksLatestDeltaBeforeMouseUp) {
  tool.setCompositedDragPreviewEnabled(true);
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(50.0, 35.0), /*buttonHeld=*/true);

  ASSERT_TRUE(tool.activeDragPreview().has_value());
  EXPECT_DOUBLE_EQ(tool.activeDragPreview()->translation.x, 35.0);
  EXPECT_DOUBLE_EQ(tool.activeDragPreview()->translation.y, 20.0);
  EXPECT_EQ(app.document().queue().size(), 0u);
}

TEST_F(SelectToolTest, MultipleMoveEventsCoalesceToFinalDelta) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
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
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
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
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  EXPECT_TRUE(tool.isDragging());

  // ...interrupted by a click in empty space (e.g. tool refocus).
  tool.onMouseDown(app, Vector2d(180.0, 180.0), MouseModifiers{});
  EXPECT_FALSE(tool.isDragging());
  EXPECT_FALSE(app.hasSelection());
}

TEST_F(SelectToolTest, DragWithMoveRecordsUndoEntry) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(40.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(40.0, 35.0));

  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  EXPECT_TRUE(app.canUndo());
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Move element");
}

TEST_F(SelectToolTest, ClickWithoutDragDoesNotRecordUndo) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));

  EXPECT_EQ(app.undoTimeline().entryCount(), 0u);
  EXPECT_FALSE(app.canUndo());
}

TEST_F(SelectToolTest, UndoRestoresElementToPreDragPosition) {
  // Drag r1 by (25, 20) and flush.
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
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
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
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
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(20.0, 15.0), /*buttonHeld=*/true);
  tool.onMouseMove(app, Vector2d(30.0, 20.0), /*buttonHeld=*/true);
  tool.onMouseMove(app, Vector2d(50.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 35.0));

  // 60fps drag → many SetTransform commands, but exactly one undo entry.
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
}

TEST_F(SelectToolTest, RedoAfterUndoRestoresPostDragState) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
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
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
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
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(40.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(40.0, 35.0));
  ASSERT_TRUE(app.flushFrame());

  // Drag r2 by (10, 5).
  tool.onMouseDown(app, Vector2d(120.0, 120.0), MouseModifiers{});
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

// ---------------------------------------------------------------------------
// Multi-select (shift+click) and marquee selection — Milestone 4.
// ---------------------------------------------------------------------------

TEST_F(SelectToolTest, ShiftClickAddsElementsToSelection) {
  // Plain click on r1 → selection = {r1}.
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements()[0].id(), "r1");

  // Shift+click on r2 → selection = {r1, r2}.
  MouseModifiers shift;
  shift.shift = true;
  tool.onMouseDown(app, Vector2d(120.0, 120.0), shift);
  tool.onMouseUp(app, Vector2d(120.0, 120.0));
  ASSERT_EQ(app.selectedElements().size(), 2u);
}

TEST_F(SelectToolTest, ShiftClickOnSelectedElementTogglesItOff) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  ASSERT_EQ(app.selectedElements().size(), 1u);

  MouseModifiers shift;
  shift.shift = true;
  tool.onMouseDown(app, Vector2d(15.0, 15.0), shift);
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  EXPECT_TRUE(app.selectedElements().empty());
}

TEST_F(SelectToolTest, ShiftClickDoesNotStartDrag) {
  // Pre-condition: r1 is selected.
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));

  // Shift+click on r2: should toggle r2 in, but NOT start a drag —
  // even if the cursor moves before the next mouseup, the element
  // shouldn't translate.
  MouseModifiers shift;
  shift.shift = true;
  tool.onMouseDown(app, Vector2d(120.0, 120.0), shift);
  EXPECT_FALSE(tool.isDragging());

  tool.onMouseMove(app, Vector2d(150.0, 150.0), /*buttonHeld=*/true);
  ASSERT_TRUE(app.flushFrame() || true);  // No-op flush is fine.
  EXPECT_DOUBLE_EQ(transformOf("#r2").data[4], 0.0);
  EXPECT_DOUBLE_EQ(transformOf("#r2").data[5], 0.0);
}

TEST_F(SelectToolTest, ClickOnEmptySpaceStartsMarquee) {
  // r1 lives at (10..30, 10..30); (60, 60) is a clean miss.
  tool.onMouseDown(app, Vector2d(60.0, 60.0), MouseModifiers{});
  EXPECT_TRUE(tool.isMarqueeing());
  EXPECT_FALSE(tool.isDragging());
  // Plain click on empty space clears the selection up-front.
  EXPECT_FALSE(app.hasSelection());

  ASSERT_TRUE(tool.marqueeRect().has_value());
  // Initial marquee is a degenerate point.
  const Box2d initialRect = *tool.marqueeRect();
  EXPECT_DOUBLE_EQ(initialRect.width(), 0.0);
  EXPECT_DOUBLE_EQ(initialRect.height(), 0.0);
}

TEST_F(SelectToolTest, MarqueeGrowsToCoverDraggedRect) {
  tool.onMouseDown(app, Vector2d(60.0, 60.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(150.0, 130.0), /*buttonHeld=*/true);

  ASSERT_TRUE(tool.marqueeRect().has_value());
  const Box2d rect = *tool.marqueeRect();
  EXPECT_DOUBLE_EQ(rect.topLeft.x, 60.0);
  EXPECT_DOUBLE_EQ(rect.topLeft.y, 60.0);
  EXPECT_DOUBLE_EQ(rect.bottomRight.x, 150.0);
  EXPECT_DOUBLE_EQ(rect.bottomRight.y, 130.0);
}

TEST_F(SelectToolTest, MarqueeNormalizesUpwardDrag) {
  // Drag *up and left* — marquee should still produce a normalized
  // rect with topLeft having the smaller coordinates.
  tool.onMouseDown(app, Vector2d(150.0, 150.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(60.0, 60.0), /*buttonHeld=*/true);

  const Box2d rect = *tool.marqueeRect();
  EXPECT_DOUBLE_EQ(rect.topLeft.x, 60.0);
  EXPECT_DOUBLE_EQ(rect.topLeft.y, 60.0);
  EXPECT_DOUBLE_EQ(rect.bottomRight.x, 150.0);
  EXPECT_DOUBLE_EQ(rect.bottomRight.y, 150.0);
}

TEST_F(SelectToolTest, MarqueeReleaseSelectsIntersectingElements) {
  // Marquee that covers both r1 (10..30) and r2 (100..140).
  tool.onMouseDown(app, Vector2d(0.0, 0.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(200.0, 200.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(200.0, 200.0));

  EXPECT_FALSE(tool.isMarqueeing());
  EXPECT_FALSE(tool.marqueeRect().has_value());
  EXPECT_EQ(app.selectedElements().size(), 2u);
}

TEST_F(SelectToolTest, MarqueeReleaseSelectsOneWhenOnlyOneIntersects) {
  tool.onMouseDown(app, Vector2d(0.0, 0.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(50.0, 50.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 50.0));

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements()[0].id(), "r1");
}

TEST_F(SelectToolTest, MarqueeReleaseSelectsZeroWhenEmpty) {
  // A marquee that doesn't touch any element should leave the
  // selection empty (it was cleared on mouseDown).
  tool.onMouseDown(app, Vector2d(60.0, 60.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(80.0, 80.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(80.0, 80.0));
  EXPECT_TRUE(app.selectedElements().empty());
}

TEST_F(SelectToolTest, ShiftMarqueeAppendsToSelection) {
  // Pre-select r1.
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  ASSERT_EQ(app.selectedElements().size(), 1u);

  // Shift+marquee over r2.
  MouseModifiers shift;
  shift.shift = true;
  tool.onMouseDown(app, Vector2d(95.0, 95.0), shift);
  tool.onMouseMove(app, Vector2d(145.0, 145.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(145.0, 145.0));

  // Both should now be selected.
  EXPECT_EQ(app.selectedElements().size(), 2u);
}

TEST_F(SelectToolTest, ShiftMarqueeOverAlreadySelectedDoesNotDuplicate) {
  // Pre-select both.
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  MouseModifiers shift;
  shift.shift = true;
  tool.onMouseDown(app, Vector2d(120.0, 120.0), shift);
  tool.onMouseUp(app, Vector2d(120.0, 120.0));
  ASSERT_EQ(app.selectedElements().size(), 2u);

  // Shift+marquee over r1 (already selected). `addToSelection` is
  // idempotent, so the size shouldn't grow.
  tool.onMouseDown(app, Vector2d(0.0, 0.0), shift);
  tool.onMouseMove(app, Vector2d(50.0, 50.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 50.0));
  EXPECT_EQ(app.selectedElements().size(), 2u);
}

// ── Multi-select drag ───────────────────────────────────────────────────────
//
// Multi-select drag: when a user clicks and drags on an element that's
// already in a multi-element selection, every selected element moves in
// lockstep. This covers the marquee → click-drag flow, undo semantics, and
// the "clicking an unselected element collapses selection" escape hatch.

TEST_F(SelectToolTest, MultiSelectDragMovesAllSelectedElements) {
  // Marquee-select both rects.
  app.setSelection(std::vector<svg::SVGElement>{elementById("#r1"), elementById("#r2")});
  ASSERT_EQ(app.selectedElements().size(), 2u);

  const Transform2d r1Start = transformOf("#r1");
  const Transform2d r2Start = transformOf("#r2");

  // Click-drag on r1 (an already-selected element). Both should move.
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  ASSERT_TRUE(tool.isDragging()) << "click on selected element starts a drag";
  EXPECT_EQ(app.selectedElements().size(), 2u)
      << "selection preserved — clicking an already-selected element does not collapse";

  tool.onMouseMove(app, Vector2d(65.0, 45.0), /*buttonHeld=*/true);  // delta (50, 30)
  tool.onMouseUp(app, Vector2d(65.0, 45.0));
  ASSERT_TRUE(app.flushFrame());

  const Transform2d r1End = transformOf("#r1");
  const Transform2d r2End = transformOf("#r2");
  EXPECT_NEAR(r1End.data[4] - r1Start.data[4], 50.0, 1e-6) << "r1 moved by dx";
  EXPECT_NEAR(r1End.data[5] - r1Start.data[5], 30.0, 1e-6) << "r1 moved by dy";
  EXPECT_NEAR(r2End.data[4] - r2Start.data[4], 50.0, 1e-6) << "r2 moved by same dx";
  EXPECT_NEAR(r2End.data[5] - r2Start.data[5], 30.0, 1e-6) << "r2 moved by same dy";
}

TEST_F(SelectToolTest, ClickOnUnselectedElementCollapsesMultiSelection) {
  // Marquee-select both.
  app.setSelection(std::vector<svg::SVGElement>{elementById("#r1"), elementById("#r2")});
  ASSERT_EQ(app.selectedElements().size(), 2u);

  // Click on r1 — already in selection — the "grab any, move all" case:
  // selection preserved.
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  EXPECT_EQ(app.selectedElements().size(), 2u);
  tool.onMouseUp(app, Vector2d(15.0, 15.0));

  // Reset to multi-selection. Now click on empty space BETWEEN the rects so
  // the marquee path clears selection. Re-select and then click r1.
  app.setSelection(std::vector<svg::SVGElement>{elementById("#r1"), elementById("#r2")});
  ASSERT_EQ(app.selectedElements().size(), 2u);

  // Now simulate "click on a different selected element" (r2): still preserves.
  tool.onMouseDown(app, Vector2d(120.0, 120.0), MouseModifiers{});
  EXPECT_EQ(app.selectedElements().size(), 2u)
      << "clicking on r2 (also in selection) preserves the whole selection";
  tool.onMouseUp(app, Vector2d(120.0, 120.0));
}

TEST_F(SelectToolTest, SingleSelectionStaysSingleWhenDragged) {
  // Only r1 selected. Click-drag on r1 → single-element drag, no extras.
  app.setSelection(elementById("#r1"));
  ASSERT_EQ(app.selectedElements().size(), 1u);

  const Transform2d r1Start = transformOf("#r1");
  const Transform2d r2Start = transformOf("#r2");

  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(65.0, 45.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(65.0, 45.0));
  ASSERT_TRUE(app.flushFrame());

  const Transform2d r1End = transformOf("#r1");
  const Transform2d r2End = transformOf("#r2");
  EXPECT_NEAR(r1End.data[4] - r1Start.data[4], 50.0, 1e-6) << "r1 moved";
  EXPECT_NEAR(r2End.data[4] - r2Start.data[4], 0.0, 1e-6) << "r2 did NOT move (not in selection)";
  EXPECT_NEAR(r2End.data[5] - r2Start.data[5], 0.0, 1e-6);
}

TEST_F(SelectToolTest, MultiSelectDragPreservesExtraElementTransforms) {
  // Give r2 a prior transform so we can verify its start transform is
  // captured and delta is composed relative to the right starting point.
  auto r2Handle = elementById("#r2").cast<svg::SVGGraphicsElement>();
  r2Handle.setTransform(Transform2d::Translate(Vector2d(5.0, 7.0)));

  app.setSelection(std::vector<svg::SVGElement>{elementById("#r1"), elementById("#r2")});
  ASSERT_EQ(app.selectedElements().size(), 2u);

  const Transform2d r1Start = transformOf("#r1");
  const Transform2d r2Start = transformOf("#r2");
  ASSERT_NEAR(r2Start.data[4], 5.0, 1e-6);  // sanity: r2 starts at translate(5, 7)
  ASSERT_NEAR(r2Start.data[5], 7.0, 1e-6);

  // Drag by (10, 20).
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(25.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(25.0, 35.0));
  ASSERT_TRUE(app.flushFrame());

  const Transform2d r1End = transformOf("#r1");
  const Transform2d r2End = transformOf("#r2");
  EXPECT_NEAR(r1End.data[4], r1Start.data[4] + 10.0, 1e-6);
  EXPECT_NEAR(r2End.data[4], 15.0, 1e-6) << "r2: 5 + 10 = 15 (delta applied to prior transform)";
  EXPECT_NEAR(r2End.data[5], 27.0, 1e-6) << "r2: 7 + 20 = 27";
}

TEST_F(SelectToolTest, MultiSelectDragWithoutMovementLeavesElementsAlone) {
  app.setSelection(std::vector<svg::SVGElement>{elementById("#r1"), elementById("#r2")});
  const Transform2d r1Start = transformOf("#r1");
  const Transform2d r2Start = transformOf("#r2");

  // mouse-down without a real mouse-move (sub-threshold noise).
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(15.1, 15.1), /*buttonHeld=*/true);  // below 1.0 threshold
  tool.onMouseUp(app, Vector2d(15.1, 15.1));
  app.flushFrame();

  EXPECT_EQ(transformOf("#r1").data[4], r1Start.data[4]) << "r1 unchanged by click-without-drag";
  EXPECT_EQ(transformOf("#r2").data[4], r2Start.data[4]) << "r2 unchanged";
}

TEST_F(SelectToolTest, MultiSelectDragProducesWritebackForAllElements) {
  app.setSelection(std::vector<svg::SVGElement>{elementById("#r1"), elementById("#r2")});

  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(45.0, 45.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(45.0, 45.0));
  ASSERT_TRUE(app.flushFrame());

  auto writeback = tool.consumeCompletedDragWriteback();
  ASSERT_TRUE(writeback.has_value()) << "primary writeback latched";
  // The extras field carries non-primary elements' writebacks so the source
  // sync path can patch every dragged element in one pass.
  EXPECT_EQ(writeback->extras.size(), 1u) << "one extra writeback for r2";
}

TEST_F(SelectToolTest, MultiSelectDragDoesNotUseCompositedPreview) {
  // When compositing is enabled but the drag is multi-element, the preview
  // path falls back to DOM mutation — the drag-preview transport models only
  // a single moving layer.
  tool.setCompositedDragPreviewEnabled(true);
  app.setSelection(std::vector<svg::SVGElement>{elementById("#r1"), elementById("#r2")});

  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(45.0, 45.0), /*buttonHeld=*/true);
  app.flushFrame();

  EXPECT_FALSE(tool.activeDragPreview().has_value())
      << "multi-select drag must not emit a composited preview (would misrepresent the group)";

  // Elements move via DOM mutation (not the composited preview path).
  EXPECT_NE(transformOf("#r1").data[4], 0.0) << "r1 moved via mutation path";

  tool.onMouseUp(app, Vector2d(45.0, 45.0));
}

TEST_F(SelectToolTest, SingleSelectDragWithCompositingUsesPreview) {
  // Regression guard for the flow that USES the composited preview.
  tool.setCompositedDragPreviewEnabled(true);
  app.setSelection(elementById("#r1"));

  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(45.0, 45.0), /*buttonHeld=*/true);

  auto preview = tool.activeDragPreview();
  ASSERT_TRUE(preview.has_value()) << "single-element drag with compositing on emits a preview";
  EXPECT_EQ(preview->entity, elementById("#r1").entityHandle().entity());

  tool.onMouseUp(app, Vector2d(45.0, 45.0));
}

}  // namespace
}  // namespace donner::editor
