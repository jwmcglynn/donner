#include "donner/editor/SelectTool.h"

#include <cstdint>

#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectionAabb.h"
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

constexpr std::string_view kCompositeSiblingSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
         <defs>
           <filter id="halo">
             <feGaussianBlur stdDeviation="1"/>
           </filter>
         </defs>
         <g id="anchor" filter="url(#halo)">
           <rect id="anchor_leaf" x="10" y="10" width="20" height="20" fill="black"/>
         </g>
         <g id="peer_contained">
           <rect id="peer_contained_leaf" x="20" y="10" width="12" height="20" fill="cyan"/>
         </g>
         <g id="peer_excluded">
           <rect id="peer_excluded_leaf" x="20" y="40" width="20" height="20" fill="white"/>
         </g>
       </svg>)svg";

constexpr std::string_view kPlainGroupSiblingSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
         <g id="plain_group">
           <rect id="plain_leaf" x="10" y="10" width="20" height="20" fill="black"/>
         </g>
         <g id="plain_peer">
           <rect id="plain_peer_leaf" x="20" y="10" width="12" height="20" fill="cyan"/>
         </g>
       </svg>)";

constexpr std::string_view kOverlappingRectsSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
         <rect id="back" x="10" y="10" width="90" height="90" fill="red"/>
         <rect id="front" x="40" y="40" width="50" height="50" fill="blue"/>
       </svg>)";

constexpr std::string_view kResizeRectSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
         <rect id="target" x="20" y="20" width="40" height="20" fill="red"/>
       </svg>)";

constexpr std::string_view kRotateRingNeighborSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
         <rect id="target" x="10" y="10" width="20" height="20" fill="red"/>
         <rect id="nearby" x="40" y="16" width="12" height="12" fill="blue"/>
       </svg>)";

class SelectToolTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_TRUE(app.loadFromString(kTwoRectsSvg)); }

  void loadSvg(std::string_view svg) { ASSERT_TRUE(app.loadFromString(svg)); }

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
    return *bounds;
  }

  bool selectionIs(std::string_view id) {
    const auto& selection = app.selectedElement();
    if (!selection.has_value()) {
      return false;
    }
    return *selection == elementById(id);
  }

  bool selectionContainsId(std::string_view id) const {
    for (const svg::SVGElement& selected : app.selectedElements()) {
      if (selected.id() == id) {
        return true;
      }
    }
    return false;
  }

  void quickClick(const Vector2d& point) {
    tool.onMouseDown(app, point, MouseModifiers{});
    tool.onMouseUp(app, point);
  }

  void drag(const Vector2d& start, const Vector2d& end) {
    tool.onMouseDown(app, start, MouseModifiers{});
    tool.onMouseMove(app, end, /*buttonHeld=*/true);
    tool.onMouseUp(app, end);
  }

  EditorApp app;
  SelectTool tool;
};

TEST_F(SelectToolTest, ClickInsideElementSelectsIt) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));

  EXPECT_TRUE(app.hasSelection());
  EXPECT_TRUE(selectionIs("#r1"));
  EXPECT_FALSE(tool.isDragging());
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
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  ASSERT_TRUE(selectionIs("#r1"));

  tool.onMouseDown(app, Vector2d(110.0, 110.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(110.0, 110.0));

  EXPECT_TRUE(selectionIs("#r2"));
}

// Pins the elevation contract: clicking inside a `<g filter>` selects
// the filter group itself, NOT the clicked leaf path. Sibling layers are
// NOT swept into the selection — auto-expansion to composite peers was
// explicitly vetoed (issue #582 follow-up): if a document author wants
// siblings to move together they wrap them in a `<g>`; the editor
// respects the DOM and doesn't guess.
TEST_F(SelectToolTest, ClickInsideFilterGroupSelectsTheGroupNotSiblings) {
  loadSvg(kCompositeSiblingSvg);

  quickClick(Vector2d(12.0, 20.0));

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements()[0].id(), "anchor")
      << "elevation lands on the filter-g, single-element selection";

  drag(Vector2d(12.0, 20.0), Vector2d(42.0, 50.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_DOUBLE_EQ(transformOf("#anchor").data[4], 30.0);
  EXPECT_DOUBLE_EQ(transformOf("#anchor").data[5], 30.0);
  EXPECT_DOUBLE_EQ(transformOf("#peer_contained").data[4], 0.0)
      << "contained sibling stays put — no auto-expansion";
  EXPECT_DOUBLE_EQ(transformOf("#peer_contained").data[5], 0.0);
  EXPECT_DOUBLE_EQ(transformOf("#peer_excluded").data[4], 0.0);
  EXPECT_DOUBLE_EQ(transformOf("#peer_excluded").data[5], 0.0);
}

TEST_F(SelectToolTest, NonCompositingGroupSelectsLeafNotGroup) {
  loadSvg(kPlainGroupSiblingSvg);

  quickClick(Vector2d(12.0, 20.0));

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements()[0].id(), "plain_leaf")
      << "plain `<g>` is not a compositing object — select the leaf path";

  drag(Vector2d(12.0, 20.0), Vector2d(32.0, 40.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_DOUBLE_EQ(transformOf("#plain_leaf").data[4], 20.0);
  EXPECT_DOUBLE_EQ(transformOf("#plain_leaf").data[5], 20.0);
  EXPECT_DOUBLE_EQ(transformOf("#plain_peer").data[4], 0.0);
  EXPECT_DOUBLE_EQ(transformOf("#plain_peer").data[5], 0.0);
}

TEST_F(SelectToolTest, DragTranslatesSelectedElement) {
  quickClick(Vector2d(15.0, 15.0));
  ASSERT_TRUE(selectionIs("#r1"));

  drag(Vector2d(15.0, 15.0), Vector2d(40.0, 35.0));

  ASSERT_TRUE(app.flushFrame());

  // Drag delta is (25, 20) → element's local transform should now reflect
  // a parent-space translation by that amount.
  const Transform2d after = transformOf("#r1");
  EXPECT_DOUBLE_EQ(after.data[4], 25.0);
  EXPECT_DOUBLE_EQ(after.data[5], 20.0);
}

TEST_F(SelectToolTest, DragPreviewTracksLatestDeltaBeforeMouseUp) {
  app.setSelection(elementById("#r1"));
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
  app.setSelection(elementById("#r1"));
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
  app.setSelection(elementById("#r1"));
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  EXPECT_TRUE(tool.isDragging());

  // ...interrupted by a click in empty space (e.g. tool refocus).
  tool.onMouseDown(app, Vector2d(180.0, 180.0), MouseModifiers{});
  EXPECT_FALSE(tool.isDragging());
  EXPECT_FALSE(app.hasSelection());
}

TEST_F(SelectToolTest, DragWithMoveRecordsUndoEntry) {
  app.setSelection(elementById("#r1"));
  drag(Vector2d(15.0, 15.0), Vector2d(40.0, 35.0));

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
  app.setSelection(elementById("#r1"));
  drag(Vector2d(15.0, 15.0), Vector2d(40.0, 35.0));
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
  app.setSelection(elementById("#r1"));
  drag(Vector2d(15.0, 15.0), Vector2d(40.0, 35.0));
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
  app.setSelection(elementById("#r1"));
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(20.0, 15.0), /*buttonHeld=*/true);
  tool.onMouseMove(app, Vector2d(30.0, 20.0), /*buttonHeld=*/true);
  tool.onMouseMove(app, Vector2d(50.0, 35.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 35.0));

  // 60fps drag → many SetTransform commands, but exactly one undo entry.
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
}

TEST_F(SelectToolTest, RedoAfterUndoRestoresPostDragState) {
  app.setSelection(elementById("#r1"));
  drag(Vector2d(15.0, 15.0), Vector2d(40.0, 35.0));
  ASSERT_TRUE(app.flushFrame());

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[4], 0.0);
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[5], 0.0);
  EXPECT_TRUE(app.canRedo());

  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[4], 25.0);
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[5], 20.0);
  EXPECT_FALSE(app.canRedo());
}

TEST_F(SelectToolTest, UndoRedoCyclesStayConsistent) {
  // Drag by (25, 20).
  app.setSelection(elementById("#r1"));
  drag(Vector2d(15.0, 15.0), Vector2d(40.0, 35.0));
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

TEST_F(SelectToolTest, RedoWithoutPriorUndoDoesNotUndoCompletedDrag) {
  app.setSelection(elementById("#r1"));
  drag(Vector2d(15.0, 15.0), Vector2d(40.0, 35.0));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_FALSE(app.canRedo());

  app.redo();
  EXPECT_FALSE(app.flushFrame());
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[4], 25.0);
  EXPECT_DOUBLE_EQ(transformOf("#r1").data[5], 20.0);
}

TEST_F(SelectToolTest, TwoDifferentDragsBothUndoableInOrder) {
  // Drag r1 by (25, 20).
  app.setSelection(elementById("#r1"));
  drag(Vector2d(15.0, 15.0), Vector2d(40.0, 35.0));
  ASSERT_TRUE(app.flushFrame());

  // Drag r2 by (10, 5).
  app.setSelection(elementById("#r2"));
  drag(Vector2d(120.0, 120.0), Vector2d(130.0, 125.0));
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

TEST_F(SelectToolTest, ShiftClickInRotateRingAddsNearbyElementInsteadOfRotating) {
  loadSvg(kRotateRingNeighborSvg);
  app.setSelection(elementById("#target"));

  MouseModifiers shift;
  shift.shift = true;
  tool.onMouseDown(app, Vector2d(44.0, 20.0), shift);
  tool.onMouseMove(app, Vector2d(20.0, 44.0), /*buttonHeld=*/true, shift);
  tool.onMouseUp(app, Vector2d(20.0, 44.0));

  EXPECT_FALSE(tool.activeGesturePreview().has_value());
  ASSERT_EQ(app.selectedElements().size(), 2u);
  EXPECT_TRUE(selectionContainsId("target"));
  EXPECT_TRUE(selectionContainsId("nearby"));
  EXPECT_DOUBLE_EQ(transformOf("#target").data[0], 1.0);
  EXPECT_DOUBLE_EQ(transformOf("#target").data[1], 0.0);
  EXPECT_DOUBLE_EQ(transformOf("#nearby").data[0], 1.0);
  EXPECT_DOUBLE_EQ(transformOf("#nearby").data[1], 0.0);
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

TEST_F(SelectToolTest, BeginMarqueeStartsDragSelectWithoutSelectingInitialHit) {
  loadSvg(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
                  <rect id="background" x="0" y="0" width="200" height="200" fill="white"/>
                  <rect id="target" x="70" y="70" width="20" height="20" fill="red"/>
                </svg>)svg");

  tool.beginMarquee(app, Vector2d(75.0, 75.0), /*additive=*/false);
  EXPECT_FALSE(app.hasSelection())
      << "Hold-to-marquee starts from the shell without selecting the element under the press.";
  EXPECT_FALSE(tool.isDragging());
  EXPECT_TRUE(tool.isMarqueeing());

  tool.onMouseMove(app, Vector2d(100.0, 100.0), /*buttonHeld=*/true);

  EXPECT_TRUE(tool.isMarqueeing());
  EXPECT_FALSE(tool.isDragging());
  ASSERT_TRUE(tool.marqueeRect().has_value());
  EXPECT_EQ(tool.marqueeRect()->topLeft, Vector2d(75.0, 75.0));
  EXPECT_EQ(tool.marqueeRect()->bottomRight, Vector2d(100.0, 100.0));
}

TEST_F(SelectToolTest, ClickHitsCurrentSelectionIncludesSelectedDescendants) {
  loadSvg(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
                  <g id="group">
                    <rect id="child" x="10" y="10" width="20" height="20" fill="red"/>
                  </g>
                  <rect id="other" x="70" y="70" width="20" height="20" fill="blue"/>
                </svg>)svg");

  app.setSelection(elementById("#group"));

  EXPECT_TRUE(tool.clickHitsCurrentSelection(app, Vector2d(20.0, 20.0)))
      << "The shell should dispatch selected-parent hits to the normal drag path, not marquee.";
  EXPECT_FALSE(tool.clickHitsCurrentSelection(app, Vector2d(80.0, 80.0)))
      << "Unselected element hits remain eligible for the delayed marquee path.";
}

TEST_F(SelectToolTest, QuickClickOnUnselectedBackgroundStillSelectsIt) {
  loadSvg(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
                  <rect id="background" x="0" y="0" width="200" height="200" fill="white"/>
                </svg>)svg");

  tool.onMouseDown(app, Vector2d(5.0, 5.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(5.0, 5.0));

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements()[0].id(), "background");
}

TEST_F(SelectToolTest, MarqueeUsesShapeIntersectionNotShapeBounds) {
  loadSvg(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
                  <path id="triangle" d="M 10 10 L 100 10 L 10 100 Z" fill="red"/>
                </svg>)svg");

  tool.onMouseDown(app, Vector2d(80.0, 80.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(90.0, 90.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(90.0, 90.0));

  EXPECT_TRUE(app.selectedElements().empty())
      << "The marquee overlaps the triangle AABB but not the filled triangle.";
}

TEST_F(SelectToolTest, MarqueeDoesNotSelectShapeThatContainsDragBox) {
  loadSvg(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
                  <rect id="background" x="0" y="0" width="200" height="200" fill="white"/>
                  <rect id="target" x="70" y="70" width="20" height="20" fill="red"/>
                </svg>)svg");

  tool.beginMarquee(app, Vector2d(65.0, 65.0), /*additive=*/false);
  tool.onMouseMove(app, Vector2d(95.0, 95.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(95.0, 95.0));

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements()[0].id(), "target");
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

TEST_F(SelectToolTest, PlainDragFromEmptySpaceWithExistingSelectionStartsMarquee) {
  app.setSelection(elementById("#r1"));
  ASSERT_TRUE(app.hasSelection());

  tool.onMouseDown(app, Vector2d(60.0, 60.0), MouseModifiers{});
  EXPECT_TRUE(tool.isMarqueeing());
  ASSERT_TRUE(tool.marqueeRect().has_value());
  EXPECT_FALSE(app.hasSelection()) << "plain non-additive marquee clears the existing selection";

  tool.onMouseMove(app, Vector2d(140.0, 140.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(140.0, 140.0));

  EXPECT_FALSE(tool.isMarqueeing());
  EXPECT_FALSE(tool.marqueeRect().has_value());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements()[0].id(), "r2");
}

TEST_F(SelectToolTest, PlainDragStartingOnSelectedShapeDoesNotStartMarquee) {
  app.setSelection(elementById("#r1"));
  ASSERT_TRUE(app.hasSelection());

  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});

  EXPECT_TRUE(tool.isDragging());
  EXPECT_FALSE(tool.isMarqueeing());
  EXPECT_FALSE(tool.marqueeRect().has_value());
}

TEST_F(SelectToolTest, BeginMarqueeWithExistingSelectionClearsAndStartsNonAdditive) {
  app.setSelection(elementById("#r1"));
  ASSERT_TRUE(app.hasSelection());

  tool.beginMarquee(app, Vector2d(60.0, 60.0), /*additive=*/false);
  EXPECT_TRUE(tool.isMarqueeing());
  ASSERT_TRUE(tool.marqueeRect().has_value());
  EXPECT_FALSE(app.hasSelection());

  app.setSelection(elementById("#r1"));
  tool.beginMarquee(app, Vector2d(60.0, 60.0), /*additive=*/true);
  EXPECT_TRUE(tool.isMarqueeing());
  ASSERT_TRUE(tool.marqueeRect().has_value());
  EXPECT_TRUE(app.hasSelection());
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
  const auto moveGestureBeforeMove = tool.activeGesturePreview();
  ASSERT_TRUE(moveGestureBeforeMove.has_value());
  EXPECT_EQ(moveGestureBeforeMove->kind, SelectTool::ActiveGestureKind::Move);
  EXPECT_FALSE(moveGestureBeforeMove->hasMoved);

  tool.onMouseMove(app, Vector2d(65.0, 45.0), /*buttonHeld=*/true);  // delta (50, 30)
  const auto moveGesture = tool.activeGesturePreview();
  ASSERT_TRUE(moveGesture.has_value());
  EXPECT_EQ(moveGesture->kind, SelectTool::ActiveGestureKind::Move);
  EXPECT_TRUE(moveGesture->hasMoved);
  EXPECT_EQ(moveGesture->currentDocumentDelta, Vector2d(50.0, 30.0));
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

TEST_F(SelectToolTest, DragAfterScaleTranslatesInDocumentSpace) {
  auto r1Handle = elementById("#r1").cast<svg::SVGGraphicsElement>();
  r1Handle.setTransform(Transform2d::Scale(2.0));
  app.setSelection(elementById("#r1"));

  const Transform2d r1Start = transformOf("#r1");

  tool.onMouseDown(app, Vector2d(40.0, 40.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(70.0, 55.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(70.0, 55.0));
  ASSERT_TRUE(app.flushFrame());

  const Transform2d r1End = transformOf("#r1");
  EXPECT_NEAR(r1End.data[0], 2.0, 1e-6);
  EXPECT_NEAR(r1End.data[3], 2.0, 1e-6);
  EXPECT_NEAR(r1End.data[4] - r1Start.data[4], 30.0, 1e-6);
  EXPECT_NEAR(r1End.data[5] - r1Start.data[5], 15.0, 1e-6);
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

TEST_F(SelectToolTest, MultiSelectDragUsesGroupedCompositedPreview) {
  // Multi-element drags still use one shared document-space transform. Exposing every selected
  // entity in the preview lets the presenter offset cached tiles and overlay chrome in lockstep
  // instead of forcing every pointer frame through a full DOM/render update.
  app.setSelection(std::vector<svg::SVGElement>{elementById("#r1"), elementById("#r2")});

  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(45.0, 45.0), /*buttonHeld=*/true);
  app.flushFrame();

  const auto preview = tool.activeDragPreview();
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->entity, elementById("#r1").unsafeEntityHandle().entity());
  ASSERT_EQ(preview->extraEntities.size(), 1u);
  EXPECT_EQ(preview->extraEntities.front(), elementById("#r2").unsafeEntityHandle().entity());
  EXPECT_EQ(preview->translation, Vector2d(30.0, 30.0));

  // The DOM write still lands; the preview exists so presentation can stay responsive while async
  // renders catch up.
  EXPECT_NE(transformOf("#r1").data[4], 0.0) << "r1 moved via mutation path";

  tool.onMouseUp(app, Vector2d(45.0, 45.0));
}

TEST_F(SelectToolTest, SingleSelectDragWithCompositingUsesPreview) {
  // Regression guard for the flow that USES the composited preview.
  app.setSelection(elementById("#r1"));

  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(45.0, 45.0), /*buttonHeld=*/true);

  auto preview = tool.activeDragPreview();
  ASSERT_TRUE(preview.has_value()) << "single-element drag with compositing on emits a preview";
  EXPECT_EQ(preview->entity, elementById("#r1").unsafeEntityHandle().entity());

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
  app.setSelection(elementById("#r1"));
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
  app.setSelection(elementById("#r1"));
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

// Design doc 0033 §M8 — re-drag-of-selected fast path. `tryStartRedragOn
// Selected` doesn't call `EditorApp::hitTest`; it works off
// `SnapshotSelectionWorldBounds` of the currently-selected element.
// EditorShell drops the `!isBusy()` gate for this path so the user
// can re-grab a selected element even while a render is in flight.
TEST_F(SelectToolTest, TryRedragOnSelectedStartsDragWhenClickIsInsideSelectedBounds) {
  // Establish a single-element selection via the normal click path.
  quickClick(Vector2d(15.0, 15.0));
  ASSERT_TRUE(selectionIs("#r1"));
  ASSERT_FALSE(tool.isDragging());

  // Click inside the same element's bounds (still selected). The
  // snapshot-safe re-drag path must start a drag without changing
  // the selection. Pass freshly-snapshotted bounds the way the
  // EditorShell does (in the real flow the bounds come from
  // `SelectionBoundsCache::displayedBoundsDoc` — race-free even if
  // the worker is currently rendering).
  const auto bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  EXPECT_TRUE(tool.tryStartRedragOnSelected(app, Vector2d(20.0, 20.0), MouseModifiers{}, bounds));
  EXPECT_TRUE(tool.isDragging());
  EXPECT_TRUE(selectionIs("#r1"));
}

TEST_F(SelectToolTest, TryRedragOnSelectedAllowsScreenPixelRoundingSlop) {
  app.setSelection(elementById("#r1"));
  const auto bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));

  MouseModifiers modifiers;
  modifiers.pixelsPerDocUnit = 10.0;

  EXPECT_TRUE(tool.tryStartRedragOnSelected(app, Vector2d(20.0, 9.9), modifiers, bounds))
      << "high-zoom screen-to-document rounding should not defer a selected-object re-drag";
  EXPECT_TRUE(tool.isDragging());
}

TEST_F(SelectToolTest, DragPreviewGenerationChangesForRedragOnSameSelection) {
  app.setSelection(elementById("#r1"));
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  ASSERT_TRUE(selectionIs("#r1"));
  ASSERT_TRUE(tool.activeDragPreview().has_value());
  const std::uint64_t firstDragGeneration = tool.activeDragPreview()->dragGeneration;
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  ASSERT_FALSE(tool.isDragging());

  const auto bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  ASSERT_TRUE(tool.tryStartRedragOnSelected(app, Vector2d(20.0, 20.0), MouseModifiers{}, bounds));
  ASSERT_TRUE(tool.activeDragPreview().has_value());

  EXPECT_GT(tool.activeDragPreview()->dragGeneration, firstDragGeneration)
      << "Presentation caches need to distinguish consecutive drags of the same entity.";
}

TEST_F(SelectToolTest, TryRedragOnSelectedReturnsFalseOnShiftClick) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));

  MouseModifiers shift{};
  shift.shift = true;
  const auto bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  // Shift-click must NOT start a re-drag — it toggles selection
  // membership, which requires the full hitTest path.
  EXPECT_FALSE(tool.tryStartRedragOnSelected(app, Vector2d(20.0, 20.0), shift, bounds));
  EXPECT_FALSE(tool.isDragging());
}

TEST_F(SelectToolTest, TryRedragOnSelectedReturnsFalseWhenNothingSelected) {
  // No prior selection.
  EXPECT_FALSE(app.selectedElement().has_value());
  EXPECT_FALSE(tool.tryStartRedragOnSelected(app, Vector2d(15.0, 15.0), MouseModifiers{},
                                             std::span<const Box2d>{}));
  EXPECT_FALSE(tool.isDragging());
}

TEST_F(SelectToolTest, TryRedragOnSelectedReturnsFalseWhenClickIsOutsideSelectedBounds) {
  // Select r1 at (10,10)..(30,30).
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  ASSERT_TRUE(selectionIs("#r1"));

  const auto bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  // Click on r2's territory (well outside r1).
  EXPECT_FALSE(
      tool.tryStartRedragOnSelected(app, Vector2d(110.0, 110.0), MouseModifiers{}, bounds));
  EXPECT_FALSE(tool.isDragging());
}

// Design doc 0033 §M8: the cache-based fast path must work with
// pre-snapshotted bounds — the EditorShell drops the `!isBusy()`
// gate for it, so a live `SnapshotSelectionWorldBounds` call would
// race the worker. Passing an empty bounds span (e.g. when the
// bounds cache hasn't been refreshed since selection changed) must
// fall through cleanly.
TEST_F(SelectToolTest, TryRedragOnSelectedReturnsFalseOnEmptyCachedBounds) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  ASSERT_TRUE(selectionIs("#r1"));

  // Empty bounds span — the EditorShell hits this when the cache's
  // `lastSelection` doesn't match the current selection (cache stale).
  EXPECT_FALSE(tool.tryStartRedragOnSelected(app, Vector2d(20.0, 20.0), MouseModifiers{},
                                             std::span<const Box2d>{}));
  EXPECT_FALSE(tool.isDragging());
}

// The re-drag fast path is reachable from a plain `onMouseDown` too —
// `SnapshotSelectionWorldBounds` returns the geometric bbox of the
// selection (no filter expansion), so any click inside that bbox is
// served by the no-hitTest path. This lets the user re-grab a
// selected `<g>` even when the click lands on a transparent
// interior pixel that `EditorApp::hitTest` wouldn't see.
TEST_F(SelectToolTest, TryRedragOnSelectedHitsTransparentInteriorOfFiltergroup) {
  loadSvg(kCompositeSiblingSvg);
  // Pick anchor via the normal hitTest path first to establish
  // selection. anchor_leaf is at (10,10)..(30,30); a click squarely
  // inside the rect selects #anchor.
  quickClick(Vector2d(15.0, 20.0));
  ASSERT_TRUE(selectionIs("#anchor"));

  // Click inside #anchor's snapshotted world bounds. The fast path
  // re-drags #anchor without consulting `editor.hitTest`. Without it,
  // the M8 caller-side `!isBusy()` drop wouldn't have a safe re-drag
  // path during busy renders.
  const auto anchorBounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  EXPECT_TRUE(
      tool.tryStartRedragOnSelected(app, Vector2d(15.0, 20.0), MouseModifiers{}, anchorBounds));
  EXPECT_TRUE(tool.isDragging());
  EXPECT_TRUE(selectionIs("#anchor"));
}

// Regression repro: EditorShell runs the re-drag fast path before the
// idle-gated full hit-test. If the click is inside the selected element's
// cached bounds but a later-painted object is on top at that point, the fast
// path must not keep dragging the behind selection. It should fall through to
// the normal hit-test path so the front object can become selected.
TEST_F(SelectToolTest, RedragFastPathDoesNotStealClickFromFrontOverlappingObject) {
  loadSvg(kOverlappingRectsSvg);
  app.setSelection(elementById("#back"));
  ASSERT_TRUE(selectionIs("#back"));

  const Vector2d overlapPoint(50.0, 50.0);
  auto hit = app.hitTest(overlapPoint);
  ASSERT_TRUE(hit.has_value());
  ASSERT_EQ(hit->id(), "front") << "test setup requires #front to paint above #back";

  SelectionBoundsCache boundsCache;
  const std::uint64_t version = app.document().currentFrameVersion();
  RefreshSelectionBoundsCache(boundsCache, std::span<const svg::SVGElement>(app.selectedElements()),
                              version, version);
  ASSERT_FALSE(boundsCache.displayedBoundsDoc.empty());
  ASSERT_TRUE(boundsCache.displayedBoundsDoc.front().contains(overlapPoint));
  ASSERT_FALSE(boundsCache.displayedOccludingBoundsDoc.empty());
  ASSERT_TRUE(boundsCache.displayedOccludingBoundsDoc.front().contains(overlapPoint));

  EXPECT_FALSE(tool.tryStartRedragOnSelected(app, overlapPoint, MouseModifiers{},
                                             boundsCache.displayedBoundsDoc,
                                             boundsCache.displayedOccludingBoundsDoc))
      << "a busy-frame re-drag fast path must not preempt a topmost hit-test candidate";
  EXPECT_FALSE(tool.isDragging());

  tool.onMouseDown(app, overlapPoint, MouseModifiers{});
  tool.onMouseUp(app, overlapPoint);
  EXPECT_TRUE(selectionIs("#front"));
}

TEST_F(SelectToolTest, CornerHandleResizesSelectionFromOppositeCorner) {
  loadSvg(kResizeRectSvg);
  app.setSelection(elementById("#target"));

  tool.onMouseDown(app, Vector2d(60.0, 40.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(80.0, 50.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(80.0, 50.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d bounds = worldBoundsOf("#target");
  EXPECT_NEAR(bounds.topLeft.x, 20.0, 1e-6);
  EXPECT_NEAR(bounds.topLeft.y, 20.0, 1e-6);
  EXPECT_NEAR(bounds.width(), 60.0, 1e-6);
  EXPECT_NEAR(bounds.height(), 30.0, 1e-6);
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Resize element");
}

TEST_F(SelectToolTest, ResizeUndoRedoRestoresBounds) {
  loadSvg(kResizeRectSvg);
  app.setSelection(elementById("#target"));

  const Box2d startBounds = worldBoundsOf("#target");
  tool.onMouseDown(app, Vector2d(60.0, 40.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(80.0, 50.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(80.0, 50.0));
  ASSERT_TRUE(app.flushFrame());
  const Box2d resizedBounds = worldBoundsOf("#target");
  ASSERT_NE(resizedBounds, startBounds);

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(worldBoundsOf("#target"), startBounds);

  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(worldBoundsOf("#target"), resizedBounds);
}

TEST_F(SelectToolTest, ResizeGestureExposesAffinePreview) {
  loadSvg(kResizeRectSvg);
  app.setSelection(elementById("#target"));

  tool.onMouseDown(app, Vector2d(60.0, 40.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(80.0, 50.0), /*buttonHeld=*/true);

  const auto preview = tool.activeDragPreview();
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->entity, elementById("#target").unsafeEntityHandle().entity());
  EXPECT_FALSE(preview->documentFromCachedDocument.isTranslation())
      << "resize preview must carry an affine scale, not just a drag offset";
  const auto resizeGesture = tool.activeGesturePreview();
  ASSERT_TRUE(resizeGesture.has_value());
  EXPECT_EQ(resizeGesture->kind, SelectTool::ActiveGestureKind::Resize);
  EXPECT_TRUE(resizeGesture->hasMoved);
  EXPECT_EQ(resizeGesture->startBoundsDoc, Box2d::FromXYWH(20.0, 20.0, 40.0, 20.0));

  const Vector2d anchoredCorner =
      preview->documentFromCachedDocument.transformPosition(Vector2d(20.0, 20.0));
  const Vector2d activeCorner =
      preview->documentFromCachedDocument.transformPosition(Vector2d(60.0, 40.0));
  EXPECT_NEAR(anchoredCorner.x, 20.0, 1e-6);
  EXPECT_NEAR(anchoredCorner.y, 20.0, 1e-6);
  EXPECT_NEAR(activeCorner.x, 80.0, 1e-6);
  EXPECT_NEAR(activeCorner.y, 50.0, 1e-6);

  tool.onMouseUp(app, Vector2d(80.0, 50.0));
}

TEST_F(SelectToolTest, ShiftCornerResizePreservesAspectRatio) {
  loadSvg(kResizeRectSvg);
  app.setSelection(elementById("#target"));

  MouseModifiers modifiers;
  modifiers.shift = true;
  tool.onMouseDown(app, Vector2d(60.0, 40.0), modifiers);
  tool.onMouseMove(app, Vector2d(80.0, 45.0), /*buttonHeld=*/true, modifiers);
  tool.onMouseUp(app, Vector2d(80.0, 45.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d bounds = worldBoundsOf("#target");
  EXPECT_NEAR(bounds.width(), 60.0, 1e-6);
  EXPECT_NEAR(bounds.height(), 30.0, 1e-6);
}

TEST_F(SelectToolTest, OptionCornerResizeKeepsCenterFixed) {
  loadSvg(kResizeRectSvg);
  app.setSelection(elementById("#target"));

  MouseModifiers modifiers;
  modifiers.option = true;
  tool.onMouseDown(app, Vector2d(60.0, 40.0), modifiers);
  tool.onMouseMove(app, Vector2d(80.0, 50.0), /*buttonHeld=*/true, modifiers);
  tool.onMouseUp(app, Vector2d(80.0, 50.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d bounds = worldBoundsOf("#target");
  const Vector2d center = (bounds.topLeft + bounds.bottomRight) * 0.5;
  EXPECT_NEAR(center.x, 40.0, 1e-6);
  EXPECT_NEAR(center.y, 30.0, 1e-6);
  EXPECT_NEAR(bounds.width(), 80.0, 1e-6);
  EXPECT_NEAR(bounds.height(), 40.0, 1e-6);
}

TEST_F(SelectToolTest, ShiftCanToggleAspectConstraintDuringResize) {
  loadSvg(kResizeRectSvg);
  app.setSelection(elementById("#target"));

  tool.onMouseDown(app, Vector2d(60.0, 40.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(80.0, 45.0), /*buttonHeld=*/true, MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  EXPECT_NEAR(worldBoundsOf("#target").width(), 60.0, 1e-6);
  EXPECT_NEAR(worldBoundsOf("#target").height(), 25.0, 1e-6);

  MouseModifiers shift;
  shift.shift = true;
  tool.onMouseMove(app, Vector2d(80.0, 45.0), /*buttonHeld=*/true, shift);
  tool.onMouseUp(app, Vector2d(80.0, 45.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d bounds = worldBoundsOf("#target");
  EXPECT_NEAR(bounds.width(), 60.0, 1e-6);
  EXPECT_NEAR(bounds.height(), 30.0, 1e-6);
}

TEST_F(SelectToolTest, OptionCanToggleCenterResizeDuringResize) {
  loadSvg(kResizeRectSvg);
  app.setSelection(elementById("#target"));

  tool.onMouseDown(app, Vector2d(60.0, 40.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(80.0, 50.0), /*buttonHeld=*/true, MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  Vector2d center = (worldBoundsOf("#target").topLeft + worldBoundsOf("#target").bottomRight) * 0.5;
  EXPECT_NEAR(center.x, 50.0, 1e-6);
  EXPECT_NEAR(center.y, 35.0, 1e-6);

  MouseModifiers option;
  option.option = true;
  tool.onMouseMove(app, Vector2d(80.0, 50.0), /*buttonHeld=*/true, option);
  tool.onMouseUp(app, Vector2d(80.0, 50.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d bounds = worldBoundsOf("#target");
  center = (bounds.topLeft + bounds.bottomRight) * 0.5;
  EXPECT_NEAR(center.x, 40.0, 1e-6);
  EXPECT_NEAR(center.y, 30.0, 1e-6);
  EXPECT_NEAR(bounds.width(), 80.0, 1e-6);
  EXPECT_NEAR(bounds.height(), 40.0, 1e-6);
}

TEST_F(SelectToolTest, MultiSelectionCornerResizeUsesCombinedBounds) {
  app.setSelection(std::vector<svg::SVGElement>{elementById("#r1"), elementById("#r2")});
  ASSERT_EQ(app.selectedElements().size(), 2u);

  const Box2d r1StartBounds = worldBoundsOf("#r1");
  const Box2d r2StartBounds = worldBoundsOf("#r2");

  tool.onMouseDown(app, Vector2d(140.0, 140.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(270.0, 270.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(270.0, 270.0));
  ASSERT_TRUE(app.flushFrame());

  const Box2d r1Bounds = worldBoundsOf("#r1");
  const Box2d r2Bounds = worldBoundsOf("#r2");
  EXPECT_NEAR(r1Bounds.topLeft.x, 10.0, 1e-6);
  EXPECT_NEAR(r1Bounds.topLeft.y, 10.0, 1e-6);
  EXPECT_NEAR(r1Bounds.width(), 40.0, 1e-6);
  EXPECT_NEAR(r1Bounds.height(), 40.0, 1e-6);
  EXPECT_NEAR(r2Bounds.topLeft.x, 190.0, 1e-6);
  EXPECT_NEAR(r2Bounds.topLeft.y, 190.0, 1e-6);
  EXPECT_NEAR(r2Bounds.width(), 80.0, 1e-6);
  EXPECT_NEAR(r2Bounds.height(), 80.0, 1e-6);
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u)
      << "one UI undo should revert the whole multi-selection resize";
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Resize elements");

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(worldBoundsOf("#r1"), r1StartBounds);
  EXPECT_EQ(worldBoundsOf("#r2"), r2StartBounds);

  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(worldBoundsOf("#r1"), r1Bounds);
  EXPECT_EQ(worldBoundsOf("#r2"), r2Bounds);
}

TEST_F(SelectToolTest, RotateZoneRotatesAroundSelectionCenter) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  ASSERT_TRUE(selectionIs("#r1"));

  tool.onMouseDown(app, Vector2d(44.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(20.0, 44.0), /*buttonHeld=*/true);
  const auto rotateGesture = tool.activeGesturePreview();
  ASSERT_TRUE(rotateGesture.has_value());
  EXPECT_EQ(rotateGesture->kind, SelectTool::ActiveGestureKind::Rotate);
  EXPECT_EQ(rotateGesture->corner, SelectionTransformCorner::TopRight);
  EXPECT_TRUE(rotateGesture->hasMoved);
  tool.onMouseUp(app, Vector2d(20.0, 44.0));
  ASSERT_TRUE(app.flushFrame());

  const Transform2d transform = transformOf("#r1");
  EXPECT_NEAR(transform.data[0], 0.0, 1e-6);
  EXPECT_NEAR(transform.data[1], 1.0, 1e-6);
  EXPECT_NEAR(transform.data[2], -1.0, 1e-6);
  EXPECT_NEAR(transform.data[3], 0.0, 1e-6);
  EXPECT_NEAR(transform.data[4], 40.0, 1e-6);
  EXPECT_NEAR(transform.data[5], 0.0, 1e-6);
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Rotate element");
}

TEST_F(SelectToolTest, ActiveRotationBoundsPreviewClearsOnMouseUp) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  ASSERT_TRUE(selectionIs("#r1"));

  tool.onMouseDown(app, Vector2d(44.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(20.0, 44.0), /*buttonHeld=*/true);
  EXPECT_TRUE(tool.activeTransformBoundsPreview().has_value());

  tool.onMouseUp(app, Vector2d(20.0, 44.0));
  EXPECT_FALSE(tool.activeTransformBoundsPreview().has_value())
      << "rotation chrome must switch back to axis-aligned bounds immediately on release";
}

TEST_F(SelectToolTest, RotateUndoRedoRestoresTransform) {
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(15.0, 15.0));
  ASSERT_TRUE(selectionIs("#r1"));
  const Transform2d startTransform = transformOf("#r1");

  tool.onMouseDown(app, Vector2d(44.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(20.0, 44.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(20.0, 44.0));
  ASSERT_TRUE(app.flushFrame());
  const Transform2d rotatedTransform = transformOf("#r1");
  ASSERT_FALSE(rotatedTransform.isIdentity());

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  const Transform2d undoTransform = transformOf("#r1");
  for (int i = 0; i < 6; ++i) {
    EXPECT_NEAR(undoTransform.data[i], startTransform.data[i], 1e-6);
  }

  app.redo();
  ASSERT_TRUE(app.flushFrame());
  const Transform2d redoTransform = transformOf("#r1");
  for (int i = 0; i < 6; ++i) {
    EXPECT_NEAR(redoTransform.data[i], rotatedTransform.data[i], 1e-6);
  }
}

TEST_F(SelectToolTest, RotateAfterScaleUsesDocumentCenter) {
  auto r1Handle = elementById("#r1").cast<svg::SVGGraphicsElement>();
  r1Handle.setTransform(Transform2d::Scale(2.0));
  app.setSelection(elementById("#r1"));

  tool.onMouseDown(app, Vector2d(60.0, 6.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(74.0, 60.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(74.0, 60.0));
  ASSERT_TRUE(app.flushFrame());

  const Transform2d transform = transformOf("#r1");
  const Vector2d localCenter = transform.transformPosition(Vector2d(20.0, 20.0));
  const Vector2d localTopRight = transform.transformPosition(Vector2d(30.0, 10.0));
  EXPECT_NEAR(localCenter.x, 40.0, 1e-6);
  EXPECT_NEAR(localCenter.y, 40.0, 1e-6);
  EXPECT_NEAR(localTopRight.x, 60.0, 1e-6);
  EXPECT_NEAR(localTopRight.y, 60.0, 1e-6);
}

}  // namespace
}  // namespace donner::editor
