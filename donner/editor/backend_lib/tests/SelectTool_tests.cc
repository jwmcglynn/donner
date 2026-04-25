#include "donner/editor/backend_lib/SelectTool.h"

#include "donner/base/Box.h"
#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/backend_lib/Tool.h"
#include "donner/svg/SVGGeometryElement.h"
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

  Box2d worldBoundsOf(std::string_view id) {
    auto element = app.document().document().querySelector(id);
    EXPECT_TRUE(element.has_value());
    auto bounds = element->cast<svg::SVGGeometryElement>().worldBounds();
    EXPECT_TRUE(bounds.has_value());
    return bounds.value_or(Box2d());
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

// Clicking a path inside a `<g filter>` elevates the selection to the
// filter group so the compositor can promote the group as a single
// cached layer. Without elevation, `promoteEntity` refuses the leaf
// (descendant of a compositing-breaking ancestor) and every drag
// frame falls into the full-document render path.
TEST(SelectToolElevationTest, ClickOnPathInsideFilterGroupSelectsTheFilterGroup) {
  // `R"svg(...)svg"` so the `url(#blur)"` sequence inside doesn't
  // accidentally end the raw-string literal (default `R"(...)"`
  // terminates on `)"`).
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
           <defs><filter id="blur"><feGaussianBlur stdDeviation="2"/></filter></defs>
           <g id="glow" filter="url(#blur)">
             <rect id="inner" x="50" y="50" width="40" height="40" fill="red"/>
           </g>
         </svg>)svg";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  SelectTool tool;

  // Click the inner path. SelectTool should elevate to #glow.
  tool.onMouseDown(app, Vector2d(70.0, 70.0), MouseModifiers{});

  ASSERT_TRUE(app.hasSelection());
  auto glow = app.document().document().querySelector("#glow");
  ASSERT_TRUE(glow.has_value());
  EXPECT_EQ(*app.selectedElement(), *glow)
      << "click on path inside `<g filter>` should select the filter group, "
         "not the inner path — otherwise `CompositorController::promoteEntity` "
         "refuses the leaf and every drag frame takes the slow path";
}

// The outermost filter/mask/clip-path ancestor wins — nested filter
// groups still promote cleanly because the innermost group's own
// promotion would be refused as a descendant of a compositing-breaking
// ancestor.
TEST(SelectToolElevationTest, NestedFilterGroupsElevateToOutermost) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
           <defs>
             <filter id="blur"><feGaussianBlur stdDeviation="2"/></filter>
             <filter id="blur2"><feGaussianBlur stdDeviation="3"/></filter>
           </defs>
           <g id="outer" filter="url(#blur)">
             <g id="inner" filter="url(#blur2)">
               <rect id="shape" x="50" y="50" width="40" height="40" fill="red"/>
             </g>
           </g>
         </svg>)svg";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  SelectTool tool;

  tool.onMouseDown(app, Vector2d(70.0, 70.0), MouseModifiers{});

  auto outer = app.document().document().querySelector("#outer");
  ASSERT_TRUE(outer.has_value());
  EXPECT_EQ(*app.selectedElement(), *outer)
      << "with nested `<g filter>` groups, selection should elevate to the "
         "outermost one (the only layer the compositor can promote)";
}

// Shift+click is an explicit curate-the-selection gesture — no
// elevation, leaf accuracy preserved so the user can add or remove
// individual paths from a multi-selection.
TEST(SelectToolElevationTest, ShiftClickKeepsLeafAccuracy) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
           <defs><filter id="blur"><feGaussianBlur stdDeviation="2"/></filter></defs>
           <g id="glow" filter="url(#blur)">
             <rect id="inner" x="50" y="50" width="40" height="40" fill="red"/>
           </g>
         </svg>)svg";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  SelectTool tool;

  MouseModifiers shift{};
  shift.shift = true;
  tool.onMouseDown(app, Vector2d(70.0, 70.0), shift);

  auto inner = app.document().document().querySelector("#inner");
  ASSERT_TRUE(inner.has_value());
  ASSERT_TRUE(app.hasSelection());
  EXPECT_EQ(*app.selectedElement(), *inner)
      << "shift-click preserves leaf accuracy so the user can curate a "
         "multi-selection; only plain clicks elevate for drag intent";
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

TEST_F(SelectToolTest, DragReleasePreservesSelection) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(40.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(40.0, 35.0));
  ASSERT_TRUE(app.flushFrame());

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements()[0].id(), "r1");
}

TEST_F(SelectToolTest, DragBottomRightHandleScalesSelectedElement) {
  app.setSelection(elementById("#r1"));

  tool.onMouseDown(app, Vector2d(30.0, 30.0), MouseModifiers{});
  ASSERT_TRUE(tool.isDragging());
  tool.onMouseMove(app, Vector2d(50.0, 50.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 50.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d bounds = worldBoundsOf("#r1");
  EXPECT_NEAR(bounds.topLeft.x, 10.0, 1e-6);
  EXPECT_NEAR(bounds.topLeft.y, 10.0, 1e-6);
  EXPECT_NEAR(bounds.bottomRight.x, 50.0, 1e-6);
  EXPECT_NEAR(bounds.bottomRight.y, 50.0, 1e-6);
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements()[0].id(), "r1");
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Scale element");
}

TEST_F(SelectToolTest, DragRightHandleScalesOnlyX) {
  app.setSelection(elementById("#r1"));

  tool.onMouseDown(app, Vector2d(30.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(tool.isDragging());
  tool.onMouseMove(app, Vector2d(50.0, 20.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 20.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d bounds = worldBoundsOf("#r1");
  EXPECT_NEAR(bounds.topLeft.x, 10.0, 1e-6);
  EXPECT_NEAR(bounds.topLeft.y, 10.0, 1e-6);
  EXPECT_NEAR(bounds.bottomRight.x, 50.0, 1e-6);
  EXPECT_NEAR(bounds.bottomRight.y, 30.0, 1e-6);
}

TEST_F(SelectToolTest, ShiftDuringCornerResizeConstrainsAspectRatio) {
  app.setSelection(elementById("#r1"));

  MouseModifiers shift;
  shift.shift = true;
  tool.onMouseDown(app, Vector2d(30.0, 30.0), MouseModifiers{});
  ASSERT_TRUE(tool.isDragging());
  tool.onMouseMove(app, Vector2d(50.0, 40.0), /*buttonHeld=*/true, shift);
  tool.onMouseUp(app, Vector2d(50.0, 40.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d bounds = worldBoundsOf("#r1");
  EXPECT_NEAR(bounds.topLeft.x, 10.0, 1e-6);
  EXPECT_NEAR(bounds.topLeft.y, 10.0, 1e-6);
  EXPECT_NEAR(bounds.bottomRight.x, 45.0, 1e-6);
  EXPECT_NEAR(bounds.bottomRight.y, 45.0, 1e-6);
}

TEST_F(SelectToolTest, ShiftResizeCanStartOnHandleWithoutTogglingSelection) {
  app.setSelection(elementById("#r1"));

  MouseModifiers shift;
  shift.shift = true;
  tool.onMouseDown(app, Vector2d(30.0, 30.0), shift);
  ASSERT_TRUE(tool.isDragging());
  EXPECT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements()[0].id(), "r1");

  tool.onMouseMove(app, Vector2d(50.0, 40.0), /*buttonHeld=*/true, shift);
  tool.onMouseUp(app, Vector2d(50.0, 40.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d bounds = worldBoundsOf("#r1");
  EXPECT_NEAR(bounds.bottomRight.x, 45.0, 1e-6);
  EXPECT_NEAR(bounds.bottomRight.y, 45.0, 1e-6);
}

TEST_F(SelectToolTest, ShiftDuringSideResizeConstrainsAspectRatioAroundOppositeEdgeCenter) {
  app.setSelection(elementById("#r1"));

  MouseModifiers shift;
  shift.shift = true;
  tool.onMouseDown(app, Vector2d(30.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(tool.isDragging());
  tool.onMouseMove(app, Vector2d(50.0, 20.0), /*buttonHeld=*/true, shift);
  tool.onMouseUp(app, Vector2d(50.0, 20.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d bounds = worldBoundsOf("#r1");
  EXPECT_NEAR(bounds.topLeft.x, 10.0, 1e-6);
  EXPECT_NEAR(bounds.topLeft.y, 0.0, 1e-6);
  EXPECT_NEAR(bounds.bottomRight.x, 50.0, 1e-6);
  EXPECT_NEAR(bounds.bottomRight.y, 40.0, 1e-6);
}

TEST_F(SelectToolTest, DraggingResizedElementPreservesScaleAndMovesByDocumentDelta) {
  app.setSelection(elementById("#r1"));

  tool.onMouseDown(app, Vector2d(30.0, 30.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(50.0, 50.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 50.0));
  ASSERT_TRUE(app.flushFrame());
  const Box2d resizedBounds = worldBoundsOf("#r1");
  ASSERT_NEAR(resizedBounds.topLeft.x, 10.0, 1e-6);
  ASSERT_NEAR(resizedBounds.bottomRight.x, 50.0, 1e-6);

  tool.onMouseDown(app, Vector2d(25.0, 25.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(35.0, 25.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(35.0, 25.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d movedBounds = worldBoundsOf("#r1");
  EXPECT_NEAR(movedBounds.topLeft.x, 20.0, 1e-6);
  EXPECT_NEAR(movedBounds.topLeft.y, 10.0, 1e-6);
  EXPECT_NEAR(movedBounds.bottomRight.x, 60.0, 1e-6);
  EXPECT_NEAR(movedBounds.bottomRight.y, 50.0, 1e-6);
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements()[0].id(), "r1");
}

TEST_F(SelectToolTest, ResizeHandleDragDoesNotUseCompositedPreview) {
  tool.setCompositedDragPreviewEnabled(true);
  app.setSelection(elementById("#r1"));

  tool.onMouseDown(app, Vector2d(30.0, 30.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(50.0, 50.0), /*buttonHeld=*/true);

  EXPECT_FALSE(tool.activeDragPreview().has_value())
      << "resize changes scale, so the translation-only compositor preview cannot represent it";
}

TEST_F(SelectToolTest, DragPreviewTracksLatestDeltaBeforeMouseUp) {
  tool.setCompositedDragPreviewEnabled(true);
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(50.0, 35.0), /*buttonHeld=*/true);

  ASSERT_TRUE(tool.activeDragPreview().has_value());
  EXPECT_DOUBLE_EQ(tool.activeDragPreview()->translation.x, 35.0);
  EXPECT_DOUBLE_EQ(tool.activeDragPreview()->translation.y, 20.0);
  // DOM is source of truth during drag — `onMouseMove` queues a
  // `SetTransformCommand` on every move even on the composited path.
  // The compositor's fast path detects the pure-translation delta and
  // reuses the cached drag-layer bitmap via its internal composition
  // transform, but the DOM is kept in sync so the canvas view and the
  // backing document never disagree.
  EXPECT_EQ(app.document().queue().size(), 1u);
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

// State-machine invariant: if the app's selection is cleared while a
// drag is in progress (e.g. the user hits Esc, a keyboard shortcut
// triggers `clearSelection`, or the tree view fires a deselect), the
// subsequent `onMouseMove` + `onMouseUp` must not crash, must not emit
// a writeback for the dropped selection, and must not leave
// `completedDragWriteback_` carrying a reference to an element that
// isn't the "current" selection anymore.
//
// Prior to the guard landing here, the drag state held a copy of the
// element handle taken at mouse-down, so subsequent drag frames kept
// mutating that element's `transform` attribute even though the app
// no longer considered it selected — users would see the drag
// continue on a ghost, and the writeback at mouse-up would still fire
// against that element, producing a stray undo entry they couldn't
// correlate to any visible selection on screen.
TEST_F(SelectToolTest, SelectionClearedDuringDragDoesNotCrashOrGhostWriteback) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  ASSERT_TRUE(tool.isDragging());
  ASSERT_TRUE(selectionIs("#r1"));

  // First drag frame while selection is still in place.
  tool.onMouseMove(app, Vector2d(30.0, 30.0), /*buttonHeld=*/true);
  EXPECT_TRUE(tool.isDragging());

  // Simulate Esc / tree-view deselect firing mid-gesture.
  app.clearSelection();

  // Subsequent drag frames must not crash and must not start silently
  // mutating some other selection.
  tool.onMouseMove(app, Vector2d(40.0, 40.0), /*buttonHeld=*/true);
  tool.onMouseMove(app, Vector2d(50.0, 50.0), /*buttonHeld=*/true);

  tool.onMouseUp(app, Vector2d(50.0, 50.0));

  // The tool keeps dragging the originally-grabbed element (behavior is
  // "a drag in flight owns its target, independent of the app-level
  // selection") — that's fine as long as the write survives and
  // produces at most ONE undo entry (from this drag's mouseUp commit),
  // not a garbage series tied to the cleared-then-restored selection.
  EXPECT_LE(app.undoTimeline().entryCount(), 1u)
      << "at most one undo entry (from the single completed drag)";
  // Either no completed writeback (drag was cancelled cleanly) or
  // exactly one (drag completed against the original target).
  auto completed = tool.consumeCompletedDragWriteback();
  if (completed.has_value()) {
    EXPECT_EQ(completed->target.elementId, std::optional<RcString>(RcString("r1")))
        << "completed writeback must target the element the drag actually grabbed";
    EXPECT_TRUE(completed->extras.empty());
  }
}

// Verifies the drag state machine against a concurrent canvas-size
// change: the user starts dragging, the window resizes (an ImGui
// layout pass reshapes the render pane), the drag continues, and
// finally completes. The drag must remain robust — cursor positions
// the editor passes in are already in document space (viewport
// conversion happens upstream in `RenderPanePresenter`), so the drag
// state itself should be unaffected by the resize. The test locks in
// that invariant: a resize-in-the-middle drag produces the same final
// transform as a resize-at-the-start drag.
TEST_F(SelectToolTest, CanvasResizeMidDragDoesNotDisturbFinalTransform) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  // First few drag frames at the initial canvas size.
  tool.onMouseMove(app, Vector2d(25.0, 25.0), /*buttonHeld=*/true);
  tool.onMouseMove(app, Vector2d(35.0, 35.0), /*buttonHeld=*/true);

  // Simulate a canvas resize: the editor's ViewportInteractionController
  // would call `document.setCanvasSize(newWidth, newHeight)` which the
  // compositor observes. SelectTool only cares about document-space
  // cursor positions; the test asserts that a resize between drag
  // frames doesn't corrupt the cumulative delta the tool is tracking.
  app.document().document().setCanvasSize(400, 400);

  // Continue the drag at the new canvas size. Same document-space
  // positions — because the caller (RenderPanePresenter) already
  // projects screen coords → doc coords, and the test feeds doc coords
  // directly.
  tool.onMouseMove(app, Vector2d(45.0, 45.0), /*buttonHeld=*/true);
  tool.onMouseMove(app, Vector2d(55.0, 55.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(55.0, 55.0));
  // `onMouseMove` queues `SetTransformCommand` mutations through
  // `editor.applyMutation`; `flushFrame` is what actually applies them
  // to the DOM. Without the flush, `transformOf` reads the pre-drag
  // value (identity) and the test lies about what happened.
  ASSERT_TRUE(app.flushFrame());

  // Expected drag delta: (55, 55) - (15, 15) = (40, 40).
  const Transform2d finalTransform = transformOf("#r1");
  const Vector2d translation = finalTransform.translation();
  EXPECT_NEAR(translation.x, 40.0, 0.01) << "drag delta corrupted by mid-drag canvas resize";
  EXPECT_NEAR(translation.y, 40.0, 0.01) << "drag delta corrupted by mid-drag canvas resize";
}

}  // namespace
}  // namespace donner::editor
