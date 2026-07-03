#include "donner/editor/LayersPanel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/editor/CompositorDebugPanel.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LayerTreeModel.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/tests/RenderTreeTestUtils.h"

namespace donner::editor {
namespace {

constexpr std::string_view kSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <g id="groupA">
    <rect id="rectTop" x="0" y="0" width="10" height="10" fill="red"/>
    <rect id="rectBottom" x="0" y="20" width="10" height="10" fill="blue"/>
  </g>
  <circle id="leaf" cx="50" cy="50" r="5" fill="purple"/>
</svg>)";

// Index of the first visible row with display name `name`, or -1.
int RowIndex(const LayersPanel& panel, std::string_view name) {
  const auto& rows = panel.rows();
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    if (rows[i].displayName == name) {
      return i;
    }
  }
  return -1;
}

// The Layers panel is a distinct type from the compositor diagnostics panel.
static_assert(!std::is_same_v<LayersPanel, CompositorDebugPanel>,
              "LayersPanel must be a distinct type from CompositorDebugPanel");

TEST(LayersPanelTest, PlainClickSelectsRowElement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  const int leafIdx = RowIndex(panel, "leaf");
  ASSERT_GE(leafIdx, 0);
  const svg::SVGElement leafElement = panel.rows()[leafIdx].element;
  panel.handleRowClick(app, static_cast<std::size_t>(leafIdx), LayersPanel::ClickModifiers{});

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), leafElement);
  EXPECT_TRUE(panel.consumeSelectionChanged());
}

TEST(LayersPanelTest, ShiftClickSelectsContiguousRange) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  LayersPanel panel;
  panel.refreshSnapshot(app);
  // At least the root <svg>, groupA, and leaf rows are visible without
  // expanding anything; that is enough to exercise a contiguous range.
  ASSERT_GE(panel.visibleRowCount(), 2u);

  // Anchor on the first row, then shift-click the last visible row: the range
  // selection should span more than one element.
  panel.handleRowClick(app, 0, LayersPanel::ClickModifiers{});
  LayersPanel::ClickModifiers shift;
  shift.shift = true;
  panel.handleRowClick(app, panel.visibleRowCount() - 1, shift);

  EXPECT_GT(app.selectedElements().size(), 1u)
      << "shift-click should select a contiguous range of rows";
}

TEST(LayersPanelTest, CtrlClickTogglesMembership) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  const int leafIdx = RowIndex(panel, "leaf");
  const int groupAIdx = RowIndex(panel, "groupA");
  ASSERT_GE(leafIdx, 0);
  ASSERT_GE(groupAIdx, 0);

  panel.handleRowClick(app, static_cast<std::size_t>(leafIdx), LayersPanel::ClickModifiers{});
  EXPECT_EQ(app.selectedElements().size(), 1u);

  LayersPanel::ClickModifiers ctrl;
  ctrl.ctrl = true;
  panel.handleRowClick(app, static_cast<std::size_t>(groupAIdx), ctrl);
  EXPECT_EQ(app.selectedElements().size(), 2u) << "ctrl-click adds to selection";

  panel.handleRowClick(app, static_cast<std::size_t>(groupAIdx), ctrl);
  EXPECT_EQ(app.selectedElements().size(), 1u) << "ctrl-click again toggles it back off";
}

TEST(LayersPanelTest, EveryVisibleRowHasThumbnailOrSwatch) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  ASSERT_GT(panel.visibleRowCount(), 0u);
  for (const LayerTreeRow& row : panel.rows()) {
    EXPECT_TRUE(panel.hasThumbnailOrSwatch(row.stableId))
        << "row '" << row.displayName << "' must have a thumbnail handle or fallback swatch";
    EXPECT_TRUE(panel.rowFallbackSwatch(row.stableId).has_value())
        << "row '" << row.displayName << "' must expose a fallback swatch color";
  }
}

TEST(LayersPanelTest, RowsAreDomBacked) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  const int leafIdx = RowIndex(panel, "leaf");
  ASSERT_GE(leafIdx, 0);
  // Each Layers row maps to a real DOM element (distinct from the compositor
  // tile panel, which describes render payloads, not editable elements).
  const xml::XMLQualifiedNameRef tagName = panel.rows()[leafIdx].element.tagName();
  EXPECT_FALSE(std::string_view(tagName.name).empty());
  EXPECT_EQ(std::string_view(tagName.name), "circle");
}

// Parse `svg` into `app`, asserting success. `EditorApp` is non-copyable and
// non-movable, so callers construct it in place and pass it by reference
// (mirroring the existing tests' `EditorApp app; app.loadFromString(...)`).
void LoadDocument(EditorApp& app, std::string_view svg) {
  ASSERT_TRUE(app.loadFromString(svg));
}

// Copy of the snapshot row whose display name matches `name`, or nullopt.
std::optional<LayerTreeRow> FindRow(const LayersPanel& panel, std::string_view name) {
  const int index = RowIndex(panel, name);
  if (index < 0) {
    return std::nullopt;
  }
  return panel.rows()[static_cast<std::size_t>(index)];
}

using svg::tests::CountRenderingInstances;
using svg::tests::CountRenderingInstancesForDataEntity;

// ---------------------------------------------------------------------------
// Inline rename + drag-to-reorder (wires the DOM-level rename/reorder engines)
// ---------------------------------------------------------------------------

TEST(LayersPanelTest, RowRenameChangesIdAndSelectsRow) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const int index = RowIndex(panel, "rect1");
  ASSERT_GE(index, 0);
  const svg::SVGElement element = panel.rows()[static_cast<std::size_t>(index)].element;

  EXPECT_TRUE(panel.handleRowRename(app, static_cast<std::size_t>(index), "renamed"));
  EXPECT_TRUE(app.document().flushFrame());

  // The element kept its identity but took the new id, and the rename selected it.
  EXPECT_EQ(element.id(), RcString("renamed"));
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), element);
  EXPECT_TRUE(panel.consumeSelectionChanged());

  panel.refreshSnapshot(app);
  EXPECT_EQ(RowIndex(panel, "rect1"), -1);
  EXPECT_GE(RowIndex(panel, "renamed"), 0);
}

TEST(LayersPanelTest, RowRenameRepointsReferences) {
  EditorApp app;
  LoadDocument(app, R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="r" x="0" y="0" width="10" height="10"/>
    <use id="u" href="#r"/>
  </svg>)svg");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const int rIndex = RowIndex(panel, "r");
  ASSERT_GE(rIndex, 0);

  // Rename the rect via its Layers row; the engine repoints the <use href>.
  EXPECT_TRUE(panel.handleRowRename(app, static_cast<std::size_t>(rIndex), "r2"));
  EXPECT_TRUE(app.document().flushFrame());

  const std::optional<svg::SVGElement> use = app.document().document().querySelector("#u");
  ASSERT_TRUE(use.has_value());
  EXPECT_EQ(use->getAttribute("href"), std::optional(RcString("#r2")));
}

TEST(LayersPanelTest, RowRenameRejectsDuplicateId) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="a" x="0" y="0" width="10" height="10"/>
    <rect id="b" x="0" y="20" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const int aIndex = RowIndex(panel, "a");
  ASSERT_GE(aIndex, 0);

  // Renaming "a" to the already-used "b" is refused by the engine.
  EXPECT_FALSE(panel.handleRowRename(app, static_cast<std::size_t>(aIndex), "b"));
  EXPECT_FALSE(app.document().flushFrame());
}

TEST(LayersPanelTest, RowRenameOutOfRangeIsNoOp) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  EXPECT_FALSE(panel.handleRowRename(app, panel.rows().size() + 5u, "x"));
  EXPECT_FALSE(app.document().flushFrame());
}

TEST(LayersPanelTest, RowReorderMovesElementAmongSiblings) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <g id="g">
      <rect id="a" x="0" y="0" width="10" height="10"/>
      <rect id="b" x="0" y="20" width="10" height="10"/>
    </g>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const int aIndex = RowIndex(panel, "a");
  const int bIndex = RowIndex(panel, "b");
  ASSERT_GE(aIndex, 0);
  ASSERT_GE(bIndex, 0);
  const svg::SVGElement a = panel.rows()[static_cast<std::size_t>(aIndex)].element;

  // Drag "a" onto "b" (downward): "a" moves to after "b". Now "b" precedes "a".
  EXPECT_TRUE(panel.handleRowReorder(app, static_cast<std::size_t>(aIndex),
                                     static_cast<std::size_t>(bIndex)));
  EXPECT_TRUE(app.document().flushFrame());

  panel.refreshSnapshot(app);
  EXPECT_LT(RowIndex(panel, "b"), RowIndex(panel, "a"));
  EXPECT_FALSE(a.nextSibling().has_value()) << "a should now be the last child of g";
}

TEST(LayersPanelTest, RowReorderRejectsCrossParent) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <g id="g1"><rect id="a" x="0" y="0" width="10" height="10"/></g>
    <g id="g2"><rect id="b" x="0" y="20" width="10" height="10"/></g>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const int aIndex = RowIndex(panel, "a");
  const int bIndex = RowIndex(panel, "b");
  ASSERT_GE(aIndex, 0);
  ASSERT_GE(bIndex, 0);

  // "a" and "b" live under different groups; the move is rejected.
  EXPECT_FALSE(panel.handleRowReorder(app, static_cast<std::size_t>(aIndex),
                                      static_cast<std::size_t>(bIndex)));
  EXPECT_FALSE(app.document().flushFrame());
}

TEST(LayersPanelTest, RowReorderSameIndexIsNoOp) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="a" x="0" y="0" width="10" height="10"/>
    <rect id="b" x="0" y="20" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const int aIndex = RowIndex(panel, "a");
  ASSERT_GE(aIndex, 0);
  EXPECT_FALSE(panel.handleRowReorder(app, static_cast<std::size_t>(aIndex),
                                      static_cast<std::size_t>(aIndex)));
  EXPECT_FALSE(app.document().flushFrame());
}

TEST(LayersPanelTest, RowZOrderBringsElementForward) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <g id="g">
      <rect id="a" x="0" y="0" width="10" height="10"/>
      <rect id="b" x="0" y="20" width="10" height="10"/>
    </g>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const int aIndex = RowIndex(panel, "a");
  ASSERT_GE(aIndex, 0);

  // "a" paints first (back). Bring it to front -> it moves after "b".
  EXPECT_TRUE(panel.handleRowZOrder(app, static_cast<std::size_t>(aIndex),
                                    EditorApp::ZOrder::BringToFront));
  EXPECT_TRUE(app.document().flushFrame());

  panel.refreshSnapshot(app);
  EXPECT_LT(RowIndex(panel, "b"), RowIndex(panel, "a"));
  // The reorder selected the acted-on row.
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front().id(), RcString("a"));
}

TEST(LayersPanelTest, RowZOrderOutOfRangeIsNoOp) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="a" x="0" y="0" width="10" height="10"/>
  </svg>)");
  LayersPanel panel;
  panel.refreshSnapshot(app);
  EXPECT_FALSE(
      panel.handleRowZOrder(app, panel.rows().size() + 5u, EditorApp::ZOrder::BringForward));
  EXPECT_FALSE(app.document().flushFrame());
}

// ---------------------------------------------------------------------------
// Feature 1: show/hide eye button
// ---------------------------------------------------------------------------

TEST(LayersPanelTest, EyeClickTogglesVisibility) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const int index = RowIndex(panel, "rect1");
  ASSERT_GE(index, 0);
  ASSERT_TRUE(panel.rows()[static_cast<std::size_t>(index)].isVisible);

  // Hide: the eye click issues a display:none style mutation, so after the
  // queued command is applied and the snapshot rebuilt the row reads hidden.
  panel.handleEyeClick(app, static_cast<std::size_t>(index));
  EXPECT_TRUE(app.document().flushFrame());
  panel.refreshSnapshot(app);
  const std::optional<LayerTreeRow> hidden = FindRow(panel, "rect1");
  ASSERT_TRUE(hidden.has_value());
  EXPECT_FALSE(hidden->isVisible);

  // Show: toggling again restores visibility.
  panel.handleEyeClick(app, static_cast<std::size_t>(RowIndex(panel, "rect1")));
  EXPECT_TRUE(app.document().flushFrame());
  panel.refreshSnapshot(app);
  const std::optional<LayerTreeRow> shown = FindRow(panel, "rect1");
  ASSERT_TRUE(shown.has_value());
  EXPECT_TRUE(shown->isVisible);
}

TEST(LayersPanelTest, RefreshSnapshotPreservesVisibilityRenderInvalidation) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const int index = RowIndex(panel, "rect1");
  ASSERT_GE(index, 0);
  const svg::SVGElement target = panel.rows()[static_cast<std::size_t>(index)].element;

  panel.handleEyeClick(app, static_cast<std::size_t>(index));
  EXPECT_TRUE(app.document().flushFrame());

  ASSERT_TRUE(svg::tests::ElementHasRenderInstanceDirtyFlag(target));

  panel.refreshSnapshot(app);

  EXPECT_TRUE(svg::tests::ElementHasRenderInstanceDirtyFlag(target))
      << "Layer thumbnails must not consume the render invalidation before the canvas renderer.";
  EXPECT_TRUE(svg::tests::DocumentNeedsFullStyleRecompute(app.document().document()));
}

TEST(LayersPanelTest, RefreshSnapshotSkipsLiveThumbnailRenderWhileCanvasInvalidationPending) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40">
    <rect id="background" width="40" height="40" fill="red"/>
    <rect id="foreground" x="10" y="10" width="10" height="10" fill="blue"/>
  </svg>)");

  svg::Renderer renderer;
  renderer.draw(app.document().document());

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const int index = RowIndex(panel, "background");
  ASSERT_GE(index, 0);
  const svg::SVGElement target = panel.rows()[static_cast<std::size_t>(index)].element;
  const Entity targetEntity = target.unsafeEntityHandle().entity();
  ASSERT_GT(CountRenderingInstancesForDataEntity(app.document().document(), targetEntity), 0);

  panel.handleEyeClick(app, static_cast<std::size_t>(index));
  EXPECT_TRUE(app.document().flushFrame());

  const int renderInstancesBeforeRefresh =
      CountRenderingInstancesForDataEntity(app.document().document(), targetEntity);
  ASSERT_GT(renderInstancesBeforeRefresh, 0)
      << "The live render tree must still contain the pre-hide instance until the canvas "
         "compositor consumes the invalidation.";

  panel.refreshSnapshot(app);

  EXPECT_EQ(CountRenderingInstancesForDataEntity(app.document().document(), targetEntity),
            renderInstancesBeforeRefresh)
      << "Layer thumbnail refresh must not prepare the live render tree before the canvas "
         "compositor consumes a pending invalidation.";
}

TEST(LayersPanelTest, RefreshSnapshotUsesLiveRenderTreeForThumbnailsWhenCanvasIsClean) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40">
    <rect id="background" width="40" height="40" fill="red"/>
    <rect id="foreground" x="10" y="10" width="10" height="10" fill="blue"/>
  </svg>)");
  ASSERT_EQ(CountRenderingInstances(app.document().document()), 0);

  LayersPanel panel;
  panel.refreshSnapshot(app);

  EXPECT_GT(CountRenderingInstances(app.document().document()), 0)
      << "Layer thumbnails must render through the live SVGDocument graph when no canvas "
         "invalidation is pending; reparsing a duplicate thumbnail SVGDocument is not allowed.";
  ASSERT_GE(RowIndex(panel, "background"), 0);
  ASSERT_GE(RowIndex(panel, "foreground"), 0);
}

// Regression for the "hidden layer ghost": a show/hide (or lock) click queues a
// DOM mutation but changes no selection, so it emits no `selectionChanged_`
// signal. EditorShell relies on `consumeQueuedMutation()` to know it must flush
// the queued mutation and re-render; without it the canvas kept presenting the
// pre-toggle frame. Pin that the mutation handlers latch the signal.
TEST(LayersPanelTest, EyeAndLockClicksSignalQueuedMutation) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const int index = RowIndex(panel, "rect1");
  ASSERT_GE(index, 0);

  // No interaction yet: nothing queued.
  EXPECT_FALSE(panel.consumeQueuedMutation());

  // Hide queues a `display` mutation and must signal a re-render.
  panel.handleEyeClick(app, static_cast<std::size_t>(index));
  EXPECT_TRUE(panel.consumeQueuedMutation())
      << "hiding a layer must signal EditorShell to flush + re-render, else the canvas ghosts";
  // Consuming clears the latch.
  EXPECT_FALSE(panel.consumeQueuedMutation());
  EXPECT_TRUE(app.document().flushFrame());
  panel.refreshSnapshot(app);

  // Lock likewise queues a mutation and signals.
  panel.handleLockClick(app, static_cast<std::size_t>(RowIndex(panel, "rect1")));
  EXPECT_TRUE(panel.consumeQueuedMutation());
}

// An out-of-range eye click queues no mutation, so it must not signal a
// re-render (no spurious flush/repaint on a dead click).
TEST(LayersPanelTest, NoOpClickDoesNotSignalQueuedMutation) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  panel.handleEyeClick(app, panel.rows().size() + 5u);
  EXPECT_FALSE(panel.consumeQueuedMutation());
}

TEST(LayersPanelTest, EyeClickOutOfRangeIsNoOp) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);

  // The seam bounds-checks the row index; an out-of-range click queues nothing.
  // (The live-app null guard is enforced at the render() call site, mirroring
  // the existing selection-click guard, so it is exercised there rather than
  // through this non-ImGui seam.)
  panel.handleEyeClick(app, panel.rows().size() + 5u);
  EXPECT_FALSE(app.document().flushFrame());
}

// ---------------------------------------------------------------------------
// Feature 2: lock + edit-gating
// ---------------------------------------------------------------------------

TEST(LayersPanelTest, LockClickSetsLockedAttribute) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const std::optional<LayerTreeRow> row = FindRow(panel, "rect1");
  ASSERT_TRUE(row.has_value());
  EXPECT_FALSE(IsLocked(row->element));
  EXPECT_FALSE(row->isLocked);

  // Lock: the attribute is set and the rebuilt row reflects it.
  panel.handleLockClick(app, static_cast<std::size_t>(RowIndex(panel, "rect1")));
  EXPECT_TRUE(app.document().flushFrame());
  panel.refreshSnapshot(app);
  const std::optional<LayerTreeRow> locked = FindRow(panel, "rect1");
  ASSERT_TRUE(locked.has_value());
  EXPECT_TRUE(IsLocked(locked->element));
  EXPECT_TRUE(locked->isLocked);

  // Unlock: toggling again clears the locked state by REMOVING the attribute
  // (not by writing data-donner-locked="false").
  panel.handleLockClick(app, static_cast<std::size_t>(RowIndex(panel, "rect1")));
  EXPECT_TRUE(app.document().flushFrame());
  panel.refreshSnapshot(app);
  const std::optional<LayerTreeRow> unlocked = FindRow(panel, "rect1");
  ASSERT_TRUE(unlocked.has_value());
  EXPECT_FALSE(IsLocked(unlocked->element));
  EXPECT_FALSE(unlocked->isLocked);
  EXPECT_FALSE(unlocked->element.getAttribute("data-donner-locked").has_value())
      << "unlock should remove data-donner-locked, not set it to \"false\"";
}

TEST(LayersPanelTest, LockSurvivesLoadFromSource) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" data-donner-locked="true" x="0" y="0" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const std::optional<LayerTreeRow> row = FindRow(panel, "rect1");
  ASSERT_TRUE(row.has_value());
  // A `data-donner-locked` attribute present in the loaded source must be
  // detected — otherwise locks do not survive save/reload (or any full reparse).
  EXPECT_TRUE(IsLocked(row->element))
      << "data-donner-locked loaded from source was not detected by IsLocked";
  EXPECT_TRUE(row->isLocked);
}

TEST(LayersPanelTest, IsLockedReportsDescendantsLocked) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <g id="group1">
      <rect id="rect1" x="0" y="0" width="10" height="10"/>
    </g>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const std::optional<LayerTreeRow> group = FindRow(panel, "group1");
  const std::optional<LayerTreeRow> child = FindRow(panel, "rect1");
  ASSERT_TRUE(group.has_value());
  ASSERT_TRUE(child.has_value());
  EXPECT_FALSE(IsLocked(group->element));
  EXPECT_FALSE(IsLocked(child->element));

  // Locking the group locks the group and its descendant.
  panel.handleLockClick(app, static_cast<std::size_t>(RowIndex(panel, "group1")));
  EXPECT_TRUE(app.document().flushFrame());
  panel.refreshSnapshot(app);
  const std::optional<LayerTreeRow> lockedGroup = FindRow(panel, "group1");
  const std::optional<LayerTreeRow> lockedChild = FindRow(panel, "rect1");
  ASSERT_TRUE(lockedGroup.has_value());
  ASSERT_TRUE(lockedChild.has_value());
  EXPECT_TRUE(IsLocked(lockedGroup->element));
  EXPECT_TRUE(IsLocked(lockedChild->element));
  EXPECT_TRUE(lockedChild->isLocked);

  // Unlocking the group releases the descendant too.
  panel.handleLockClick(app, static_cast<std::size_t>(RowIndex(panel, "group1")));
  EXPECT_TRUE(app.document().flushFrame());
  panel.refreshSnapshot(app);
  const std::optional<LayerTreeRow> unlockedGroup = FindRow(panel, "group1");
  const std::optional<LayerTreeRow> unlockedChild = FindRow(panel, "rect1");
  ASSERT_TRUE(unlockedGroup.has_value());
  ASSERT_TRUE(unlockedChild.has_value());
  EXPECT_FALSE(IsLocked(unlockedGroup->element));
  EXPECT_FALSE(IsLocked(unlockedChild->element));
}

TEST(LayersPanelTest, LockedElementTransformIsNoOp) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="locked" x="0" y="0" width="10" height="10"/>
    <rect id="free" x="20" y="0" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const svg::SVGElement lockedElement = FindRow(panel, "locked")->element;
  const svg::SVGElement freeElement = FindRow(panel, "free")->element;

  // Lock the first rect.
  panel.handleLockClick(app, static_cast<std::size_t>(RowIndex(panel, "locked")));
  EXPECT_TRUE(app.document().flushFrame());
  ASSERT_TRUE(IsLocked(lockedElement));

  const Transform2d translation = Transform2d::Translate({5.0, 7.0});

  // A transform on the locked element is dropped: its transform stays identity.
  app.applyMutation(EditorCommand::SetTransformCommand(lockedElement, translation));
  (void)app.document().flushFrame();
  EXPECT_TRUE(lockedElement.cast<svg::SVGGraphicsElement>().transform().isIdentity());

  // The same transform on the unlocked element is applied.
  app.applyMutation(EditorCommand::SetTransformCommand(freeElement, translation));
  EXPECT_TRUE(app.document().flushFrame());
  EXPECT_FALSE(freeElement.cast<svg::SVGGraphicsElement>().transform().isIdentity());
}

TEST(LayersPanelTest, LockedElementDeleteIsNoOp) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="locked" x="0" y="0" width="10" height="10"/>
    <rect id="free" x="20" y="0" width="10" height="10"/>
  </svg>)");

  LayersPanel panel;
  panel.refreshSnapshot(app);
  const svg::SVGElement lockedElement = FindRow(panel, "locked")->element;
  const svg::SVGElement freeElement = FindRow(panel, "free")->element;

  panel.handleLockClick(app, static_cast<std::size_t>(RowIndex(panel, "locked")));
  EXPECT_TRUE(app.document().flushFrame());
  ASSERT_TRUE(IsLocked(lockedElement));

  // Deleting a locked element is dropped: it stays attached to the tree.
  app.applyMutation(EditorCommand::DeleteElementCommand(lockedElement));
  (void)app.document().flushFrame();
  EXPECT_TRUE(lockedElement.parentElement().has_value());

  // Deleting the unlocked element detaches it from the tree.
  app.applyMutation(EditorCommand::DeleteElementCommand(freeElement));
  EXPECT_TRUE(app.document().flushFrame());
  EXPECT_FALSE(freeElement.parentElement().has_value());
}

// ---------------------------------------------------------------------------
// Rendered thumbnails (Layers-panel previews are real Donner rasters, not
// ImGui-synthesized vector silhouettes -- CLAUDE.md "No Rendering Vector
// Graphics With ImGui").
// ---------------------------------------------------------------------------

namespace {

constexpr int kMaxThumbnailWidthPx = 42;
constexpr int kMaxThumbnailHeightPx = 24;

// Read the RGBA pixel at (x, y) from a renderer bitmap (row-bytes aware).
std::array<int, 4> ThumbnailPixelAt(const svg::RendererBitmap& bitmap, int x, int y) {
  const std::size_t index =
      static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
  return {bitmap.pixels[index + 0], bitmap.pixels[index + 1], bitmap.pixels[index + 2],
          bitmap.pixels[index + 3]};
}

template <typename Predicate>
int CountThumbnailPixelsMatching(const svg::RendererBitmap& bitmap, Predicate predicate) {
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      if (predicate(ThumbnailPixelAt(bitmap, x, y))) {
        ++count;
      }
    }
  }
  return count;
}

template <typename Predicate>
double AverageThumbnailXMatching(const svg::RendererBitmap& bitmap, Predicate predicate) {
  double sumX = 0.0;
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      if (predicate(ThumbnailPixelAt(bitmap, x, y))) {
        sumX += static_cast<double>(x);
        ++count;
      }
    }
  }
  return count == 0 ? 0.0 : sumX / static_cast<double>(count);
}

bool IsBrightWarmThumbnailPixel(const std::array<int, 4>& px) {
  return px[3] > 180 && px[0] > 170 && px[1] > 110 && px[2] < 130;
}

bool IsDarkOpaqueThumbnailPixel(const std::array<int, 4>& px) {
  return px[3] > 220 && px[0] < 45 && px[1] < 60 && px[2] < 80;
}

std::string ReadFixture(std::string_view path) {
  std::ifstream input{donner::Runfiles::instance().Rlocation(std::string(path))};
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

}  // namespace

TEST(LayersPanelTest, RowThumbnailIsRealRender) {
  // A red rect row yields a thumbnail whose center pixel is red -- proving the
  // preview is a Donner raster of the element, not a normalized point list.
  EditorApp app;
  ASSERT_TRUE(
      app.loadFromString(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
    <rect id="solidRed" x="10" y="10" width="80" height="80" fill="rgb(220,0,0)"/>
  </svg>)SVG"));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  const std::optional<LayerTreeRow> row = FindRow(panel, "solidRed");
  ASSERT_TRUE(row.has_value());

  const svg::RendererBitmap* thumbnail = panel.rowThumbnail(row->stableId);
  ASSERT_NE(thumbnail, nullptr) << "geometry row must have a rendered thumbnail bitmap";
  ASSERT_FALSE(thumbnail->empty());
  ASSERT_EQ(thumbnail->dimensions, Vector2i(24, 24));

  const std::array<int, 4> center = ThumbnailPixelAt(*thumbnail, 12, 12);
  EXPECT_NEAR(center[0], 220, 6) << "thumbnail center red channel";
  EXPECT_NEAR(center[1], 0, 6) << "thumbnail center green channel";
  EXPECT_NEAR(center[2], 0, 6) << "thumbnail center blue channel";
  EXPECT_NEAR(center[3], 255, 6) << "thumbnail center alpha";
}

TEST(LayersPanelTest, RowThumbnailIsCroppedToElementBounds) {
  // A small, far-off element should still fill its thumbnail. Rendering the
  // whole document viewport would leave the center transparent.
  EditorApp app;
  ASSERT_TRUE(
      app.loadFromString(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="1000">
    <rect id="smallOffOrigin" x="900" y="900" width="20" height="20" fill="rgb(20,180,40)"/>
  </svg>)SVG"));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  const std::optional<LayerTreeRow> row = FindRow(panel, "smallOffOrigin");
  ASSERT_TRUE(row.has_value());

  const svg::RendererBitmap* thumbnail = panel.rowThumbnail(row->stableId);
  ASSERT_NE(thumbnail, nullptr) << "geometry row must have a rendered thumbnail bitmap";
  ASSERT_FALSE(thumbnail->empty());
  ASSERT_EQ(thumbnail->dimensions, Vector2i(24, 24));

  const std::array<int, 4> center = ThumbnailPixelAt(*thumbnail, 12, 12);
  EXPECT_NEAR(center[0], 20, 6) << "thumbnail center red channel";
  EXPECT_NEAR(center[1], 180, 6) << "thumbnail center green channel";
  EXPECT_NEAR(center[2], 40, 6) << "thumbnail center blue channel";
  EXPECT_NEAR(center[3], 255, 6) << "thumbnail center alpha";

  EXPECT_NEAR(ThumbnailPixelAt(*thumbnail, 0, 12)[3], 255, 6)
      << "the returned variable-sized thumbnail should be tight to the square element";
  EXPECT_NEAR(ThumbnailPixelAt(*thumbnail, 23, 12)[3], 255, 6)
      << "the returned variable-sized thumbnail should be tight to the square element";
  EXPECT_NEAR(ThumbnailPixelAt(*thumbnail, 12, 0)[3], 255, 6)
      << "row thumbnail should not leave top padding";
  EXPECT_NEAR(ThumbnailPixelAt(*thumbnail, 12, 23)[3], 255, 6)
      << "row thumbnail should not leave bottom padding";
}

TEST(LayersPanelTest, GroupThumbnailComposesChildren) {
  EditorApp app;
  ASSERT_TRUE(
      app.loadFromString(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
    <g id="halves">
      <rect x="0" y="0" width="50" height="100" fill="rgb(80,200,255)"/>
      <rect x="50" y="0" width="50" height="100" fill="rgb(210,210,0)"/>
    </g>
  </svg>)SVG"));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  const std::optional<LayerTreeRow> group = FindRow(panel, "halves");
  ASSERT_TRUE(group.has_value());

  const svg::RendererBitmap* thumbnail = panel.rowThumbnail(group->stableId);
  ASSERT_NE(thumbnail, nullptr) << "a group row composes its descendants into one thumbnail";
  ASSERT_FALSE(thumbnail->empty());
  ASSERT_EQ(thumbnail->dimensions, Vector2i(24, 24));

  const std::array<int, 4> leftPixel = ThumbnailPixelAt(*thumbnail, 6, 12);
  const std::array<int, 4> rightPixel = ThumbnailPixelAt(*thumbnail, 17, 12);
  EXPECT_NEAR(leftPixel[0], 80, 8) << "left half is blue (red channel)";
  EXPECT_NEAR(leftPixel[1], 200, 8) << "left half is blue (green channel)";
  EXPECT_NEAR(leftPixel[2], 255, 8) << "left half is blue";
  EXPECT_NEAR(rightPixel[0], 210, 8) << "right half is yellow (red channel)";
  EXPECT_NEAR(rightPixel[1], 210, 8) << "right half is yellow (green channel)";
}

TEST(LayersPanelTest, DonnerSplashDonnerRowThumbnailShowsLetterFill) {
  EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  const std::optional<LayerTreeRow> row = FindRow(panel, "Donner");
  ASSERT_TRUE(row.has_value());

  const svg::RendererBitmap* thumbnail = panel.rowThumbnail(row->stableId);
  ASSERT_NE(thumbnail, nullptr);
  ASSERT_FALSE(thumbnail->empty());
  ASSERT_GT(thumbnail->dimensions.x, thumbnail->dimensions.y)
      << "the Donner wordmark thumbnail should use a wide content-sized bitmap";
  ASSERT_LE(thumbnail->dimensions.x, kMaxThumbnailWidthPx);
  ASSERT_LE(thumbnail->dimensions.y, kMaxThumbnailHeightPx);

  EXPECT_NEAR(ThumbnailPixelAt(*thumbnail, 0, 0)[3], 0, 2)
      << "the Donner row thumbnail should not include the document background";
  EXPECT_GT(CountThumbnailPixelsMatching(*thumbnail, IsBrightWarmThumbnailPixel), 30)
      << "the Donner row thumbnail should show yellow/white letter fill pixels";
  EXPECT_NEAR(AverageThumbnailXMatching(*thumbnail, IsBrightWarmThumbnailPixel),
              (static_cast<double>(thumbnail->dimensions.x) - 1.0) * 0.5, 3.0)
      << "the Donner letters should be centered in the row thumbnail";
  EXPECT_LT(CountThumbnailPixelsMatching(*thumbnail, IsDarkOpaqueThumbnailPixel),
            thumbnail->dimensions.x * thumbnail->dimensions.y / 2)
      << "a dark thumbnail means the row is showing background-like pixels instead of letters";
}

TEST(LayersPanelTest, DonnerSplashSunburstRowThumbnailCentersBrightContent) {
  EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  const std::optional<LayerTreeRow> row = FindRow(panel, "Sunburst");
  ASSERT_TRUE(row.has_value());

  const svg::RendererBitmap* thumbnail = panel.rowThumbnail(row->stableId);
  ASSERT_NE(thumbnail, nullptr);
  ASSERT_FALSE(thumbnail->empty());
  ASSERT_LE(thumbnail->dimensions.x, kMaxThumbnailWidthPx);
  ASSERT_LE(thumbnail->dimensions.y, kMaxThumbnailHeightPx);

  const int warmPixels = CountThumbnailPixelsMatching(*thumbnail, IsBrightWarmThumbnailPixel);
  ASSERT_GT(warmPixels, 35) << "the Sunburst row thumbnail should include the sun";
  EXPECT_NEAR(AverageThumbnailXMatching(*thumbnail, IsBrightWarmThumbnailPixel),
              (static_cast<double>(thumbnail->dimensions.x) - 1.0) * 0.5, 3.0)
      << "the sun should be centered in the row thumbnail instead of clipped to an edge";
}

TEST(LayersPanelTest, DonnerSplashSunburstRowThumbnailUsesFullElementBounds) {
  EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  const std::optional<LayerTreeRow> row = FindRow(panel, "Sunburst");
  ASSERT_TRUE(row.has_value());

  const svg::RendererBitmap* thumbnail = panel.rowThumbnail(row->stableId);
  ASSERT_NE(thumbnail, nullptr);
  ASSERT_FALSE(thumbnail->empty());

  EXPECT_EQ(thumbnail->dimensions.x, 41);
  EXPECT_EQ(thumbnail->dimensions.y, 24);
}

TEST(LayersPanelTest, DonnerSplashBlueCenterBurstRowThumbnailUsesFullElementBounds) {
  EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  const std::optional<LayerTreeRow> row = FindRow(panel, "Blue_center_burst");
  ASSERT_TRUE(row.has_value());

  const svg::RendererBitmap* thumbnail = panel.rowThumbnail(row->stableId);
  ASSERT_NE(thumbnail, nullptr);
  ASSERT_FALSE(thumbnail->empty());

  EXPECT_EQ(thumbnail->dimensions.x, 21);
  EXPECT_EQ(thumbnail->dimensions.y, 24);
}

TEST(LayersPanelTest, DonnerSplashBackgroundStickerThumbnailIsStableAfterCanvasRender) {
  EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  svg::Renderer thumbnailRenderer;
  LayersPanel panel;
  panel.refreshSnapshot(app, &thumbnailRenderer);

  const std::optional<LayerTreeRow> firstRow = FindRow(panel, "Background_sticker");
  ASSERT_TRUE(firstRow.has_value());
  const svg::RendererBitmap* firstThumbnail = panel.rowThumbnail(firstRow->stableId);
  ASSERT_NE(firstThumbnail, nullptr);
  ASSERT_FALSE(firstThumbnail->empty());
  const svg::RendererBitmap first = *firstThumbnail;

  svg::Renderer canvasRenderer;
  canvasRenderer.draw(app.document().document());

  for (int refreshIndex = 0; refreshIndex < 3; ++refreshIndex) {
    panel.refreshSnapshot(app, &thumbnailRenderer);

    const std::optional<LayerTreeRow> row = FindRow(panel, "Background_sticker");
    ASSERT_TRUE(row.has_value());
    const svg::RendererBitmap* thumbnail = panel.rowThumbnail(row->stableId);
    ASSERT_NE(thumbnail, nullptr);
    ASSERT_FALSE(thumbnail->empty());
    CompareBitmapToBitmap(
        *thumbnail, first,
        "background_sticker_thumbnail_refresh_" + std::to_string(refreshIndex + 1),
        tests::PixelmatchIdentityParams());
  }
}

TEST(LayersPanelTest, RefreshSnapshotReusesThumbnailsWhenDocumentFrameIsUnchanged) {
  EditorApp app;
  ASSERT_TRUE(
      app.loadFromString(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50">
    <rect id="left" x="0" y="0" width="50" height="50" fill="red"/>
    <rect id="right" x="50" y="0" width="50" height="50" fill="blue"/>
  </svg>)SVG"));

  svg::Renderer renderer;
  LayersPanel panel;
  panel.refreshSnapshot(app, &renderer);
  const LayersPanel::ThumbnailRefreshStats firstStats = panel.thumbnailRefreshStats();
  ASSERT_GT(firstStats.renderedCount, 0u);
  ASSERT_EQ(firstStats.reusedCount, 0u);

  panel.refreshSnapshot(app, &renderer);
  const LayersPanel::ThumbnailRefreshStats secondStats = panel.thumbnailRefreshStats();
  EXPECT_EQ(secondStats.documentFrameVersion, firstStats.documentFrameVersion);
  EXPECT_EQ(secondStats.renderedCount, 0u)
      << "idle sidebar frames must reuse thumbnails instead of rasterizing every row";
  EXPECT_EQ(secondStats.reusedCount, firstStats.renderedCount);

  const std::optional<svg::SVGElement> left = app.document().document().querySelector("#left");
  ASSERT_TRUE(left.has_value());
  app.applyMutation(EditorCommand::SetAttributeCommand(*left, "fill", "green"));
  ASSERT_TRUE(app.flushFrame());

  svg::Renderer canvasRenderer;
  canvasRenderer.draw(app.document().document());
  panel.refreshSnapshot(app, &renderer);
  const LayersPanel::ThumbnailRefreshStats changedStats = panel.thumbnailRefreshStats();
  EXPECT_NE(changedStats.documentFrameVersion, firstStats.documentFrameVersion);
  EXPECT_GT(changedStats.renderedCount, 0u)
      << "a real document-frame change must invalidate cached layer thumbnails";
}

// The approved thumbnail PNGs are default-renderer goldens. The Geode wrapper
// keeps backend-independent layer panel behavior covered by the semantic tests
// below.
#if !defined(DONNER_EDITOR_WGPU)
struct LayerThumbnailGoldenCase {
  std::string_view displayName;
  std::string_view goldenPath;
};

constexpr std::array<LayerThumbnailGoldenCase, 10> kDonnerSplashLayerThumbnailGoldenCases = {{
    {"<g>[0]", "donner/editor/tests/testdata/layer_thumbnails/donner_splash_root_group.png"},
    {"Background", "donner/editor/tests/testdata/layer_thumbnails/donner_splash_background.png"},
    {"Sunburst", "donner/editor/tests/testdata/layer_thumbnails/donner_splash_sunburst.png"},
    {"Background_sticker",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_background_sticker.png"},
    {"Blue_center_burst",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_blue_center_burst.png"},
    {"Lightning_logo_hit_bursts",
     "donner/editor/tests/testdata/layer_thumbnails/"
     "donner_splash_lightning_logo_hit_bursts.png"},
    {"Donner", "donner/editor/tests/testdata/layer_thumbnails/donner_splash_donner.png"},
    {"Donner_line", "donner/editor/tests/testdata/layer_thumbnails/donner_splash_donner_line.png"},
    {"Lightning_glow_dark",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_lightning_glow_dark.png"},
    {"Lightning_glow_bright",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_lightning_glow_bright.png"},
}};

TEST(LayersPanelTest, DonnerSplashRowThumbnailsMatchApprovedRendererGoldens) {
  EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  for (const LayerThumbnailGoldenCase& testCase : kDonnerSplashLayerThumbnailGoldenCases) {
    const std::optional<LayerTreeRow> row = FindRow(panel, testCase.displayName);
    ASSERT_TRUE(row.has_value()) << "Missing layer row " << testCase.displayName;

    const svg::RendererBitmap* thumbnail = panel.rowThumbnail(row->stableId);
    ASSERT_NE(thumbnail, nullptr) << "Layer " << testCase.displayName
                                  << " must have a rendered thumbnail bitmap";
    CompareBitmapToGolden(*thumbnail, testCase.goldenPath, testCase.displayName,
                          tests::PixelmatchIdentityParams());
  }
}
#endif

// ---------------------------------------------------------------------------
// Render-path: the right-aligned eye/lock buttons must actually receive clicks.
// Regression for a QA-found "lock/hide buttons don't respond" — the full-width
// SpanAllColumns row Selectable needs AllowOverlap or it eats the buttons' clicks.
// ---------------------------------------------------------------------------

class LayersPanelImGuiTest : public ::testing::Test {
protected:
  struct CheckerboardDiagnostics {
    int lightRectCount = 0;
    float maxLightRectWidth = 0.0f;
    float maxLightRectHeight = 0.0f;
  };

  struct LabelAlignmentDiagnostics {
    bool foundPreview = false;
    bool foundLabel = false;
    float previewCenterY = 0.0f;
    float labelCenterY = 0.0f;
  };

  struct ThumbnailImageDiagnostics {
    ImVec2 uvBottomRight;
    ImVec2 size;
  };

  struct IconButtonDiagnostics {
    int providerCalls = 0;
    int nonEmptyBitmaps = 0;
    int retinaBitmaps = 0;
    int imageQuads = 0;
  };

  void SetUp() override {
    IMGUI_CHECKVERSION();
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.ConfigMacOSXBehaviors = false;
    io.Fonts->Build();
  }

  void TearDown() override {
    if (ctx_ != nullptr) {
      ImGui::DestroyContext(ctx_);
      ctx_ = nullptr;
    }
  }

  // Render one Layers-panel frame in a fixed window with the given mouse state.
  void Frame(LayersPanel& panel, EditorApp& app, const ImVec2& mouse, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 250), ImGuiCond_Always);
    ImGui::Begin("##layers_test", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    panel.render(&app);
    ImGui::End();
    ImGui::Render();
  }

  int CountRightAffordanceTextRuns(LayersPanel& panel, EditorApp& app) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.AddMousePosEvent(-1.0f, -1.0f);
    io.AddMouseButtonEvent(0, false);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 250), ImGuiCond_Always);
    ImGui::Begin("##layers_affordance_text_test", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    panel.render(&app);
    const ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    const float affordanceTextMinX =
        ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - 72.0f;

    int count = 0;
    for (int i = 0; i < drawList->VtxBuffer.Size;) {
      if (drawList->VtxBuffer[i].col != textColor) {
        ++i;
        continue;
      }

      ImVec2 min = drawList->VtxBuffer[i].pos;
      ImVec2 max = drawList->VtxBuffer[i].pos;
      int j = i + 1;
      for (; j < drawList->VtxBuffer.Size && drawList->VtxBuffer[j].col == textColor; ++j) {
        const ImVec2 pos = drawList->VtxBuffer[j].pos;
        min.x = std::min(min.x, pos.x);
        min.y = std::min(min.y, pos.y);
        max.x = std::max(max.x, pos.x);
        max.y = std::max(max.y, pos.y);
      }

      const float width = max.x - min.x;
      const float height = max.y - min.y;
      if (min.x >= affordanceTextMinX && width <= 18.0f &&
          height <= ImGui::GetTextLineHeight() + 2.0f) {
        ++count;
      }
      i = j;
    }

    ImGui::End();
    ImGui::Render();
    return count;
  }

  IconButtonDiagnostics MeasureIconButtons(LayersPanel& panel, EditorApp& app) {
    constexpr ImTextureID kIconTexture = static_cast<ImTextureID>(0x5678);
    IconButtonDiagnostics diagnostics;
    const LayersPanel::IconTextureProvider iconTextureProvider =
        [&diagnostics](std::uint64_t, const svg::RendererBitmap& bitmap) {
          ++diagnostics.providerCalls;
          if (!bitmap.empty() && bitmap.dimensions.x > 0 && bitmap.dimensions.y > 0) {
            ++diagnostics.nonEmptyBitmaps;
          }
          if (!bitmap.empty() && bitmap.dimensions.x >= 28 && bitmap.dimensions.y >= 28) {
            ++diagnostics.retinaBitmaps;
          }
          return LayersPanel::IconTexture{
              .texture = kIconTexture,
              .uvBottomRight = Vector2d(1.0, 1.0),
          };
        };

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.AddMousePosEvent(-1.0f, -1.0f);
    io.AddMouseButtonEvent(0, false);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 250), ImGuiCond_Always);
    ImGui::Begin("##layers_icon_button_test", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    panel.render(&app, {}, iconTextureProvider);
    const ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int cmdIndex = 0; cmdIndex < drawList->CmdBuffer.Size; ++cmdIndex) {
      const ImDrawCmd& cmd = drawList->CmdBuffer[cmdIndex];
      if (cmd.GetTexID() == kIconTexture) {
        diagnostics.imageQuads += static_cast<int>(cmd.ElemCount / 6u);
      }
    }
    ImGui::End();
    ImGui::Render();
    return diagnostics;
  }

  // Render one Layers-panel frame and return how many vertices in the window's
  // draw list carry the locked-rejection flash red (RGBA 0xFF,0x1A,0x1A, any
  // alpha). The flash row paints a filled rect in that color via the draw-list
  // channel split, so a positive count proves the row background flashed red.
  // Alpha is ignored so the count is stable across fade intensity (the
  // intensity-driven alpha is asserted at the seam level instead).
  int CountFlashRedVertices(LayersPanel& panel, EditorApp& app) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.AddMousePosEvent(-1.0f, -1.0f);  // mouse off-panel so hover chrome stays clear
    io.AddMouseButtonEvent(0, false);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 250), ImGuiCond_Always);
    ImGui::Begin("##layers_flash_test", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    panel.render(&app);
    const ImDrawList* drawList = ImGui::GetWindowDrawList();
    int count = 0;
    for (int i = 0; i < drawList->VtxBuffer.Size; ++i) {
      const ImU32 col = drawList->VtxBuffer[i].col;
      const int r = static_cast<int>((col >> IM_COL32_R_SHIFT) & 0xff);
      const int g = static_cast<int>((col >> IM_COL32_G_SHIFT) & 0xff);
      const int b = static_cast<int>((col >> IM_COL32_B_SHIFT) & 0xff);
      const int a = static_cast<int>((col >> IM_COL32_A_SHIFT) & 0xff);
      if (r == 0xff && g == 0x1a && b == 0x1a && a > 0) {
        ++count;
      }
    }
    ImGui::End();
    ImGui::Render();
    return count;
  }

  CheckerboardDiagnostics MeasureCheckerboard(LayersPanel& panel, EditorApp& app) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.AddMousePosEvent(-1.0f, -1.0f);
    io.AddMouseButtonEvent(0, false);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 250), ImGuiCond_Always);
    ImGui::Begin("##layers_checkerboard_test", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    panel.render(&app);
    const ImDrawList* drawList = ImGui::GetWindowDrawList();

    constexpr ImU32 kLightCheckerColor = IM_COL32(120, 120, 120, 255);
    CheckerboardDiagnostics diagnostics;
    for (int i = 0; i + 3 < drawList->VtxBuffer.Size;) {
      bool isLightRect = true;
      for (int j = 0; j < 4; ++j) {
        if (drawList->VtxBuffer[i + j].col != kLightCheckerColor) {
          isLightRect = false;
          break;
        }
      }
      if (!isLightRect) {
        ++i;
        continue;
      }

      ImVec2 min = drawList->VtxBuffer[i].pos;
      ImVec2 max = drawList->VtxBuffer[i].pos;
      for (int j = 1; j < 4; ++j) {
        const ImVec2 pos = drawList->VtxBuffer[i + j].pos;
        min.x = std::min(min.x, pos.x);
        min.y = std::min(min.y, pos.y);
        max.x = std::max(max.x, pos.x);
        max.y = std::max(max.y, pos.y);
      }
      ++diagnostics.lightRectCount;
      diagnostics.maxLightRectWidth = std::max(diagnostics.maxLightRectWidth, max.x - min.x);
      diagnostics.maxLightRectHeight = std::max(diagnostics.maxLightRectHeight, max.y - min.y);
      i += 4;
    }

    ImGui::End();
    ImGui::Render();
    return diagnostics;
  }

  LabelAlignmentDiagnostics MeasureLabelAlignment(LayersPanel& panel, EditorApp& app,
                                                  std::string_view label) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.AddMousePosEvent(-1.0f, -1.0f);
    io.AddMouseButtonEvent(0, false);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 250), ImGuiCond_Always);
    ImGui::Begin("##layers_label_alignment_test", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    panel.render(&app);
    const ImDrawList* drawList = ImGui::GetWindowDrawList();

    struct RectBounds {
      ImVec2 min;
      ImVec2 max;
    };

    constexpr ImU32 kLightCheckerColor = IM_COL32(120, 120, 120, 255);
    std::vector<RectBounds> lightRects;
    for (int i = 0; i + 3 < drawList->VtxBuffer.Size;) {
      bool isLightRect = true;
      for (int j = 0; j < 4; ++j) {
        if (drawList->VtxBuffer[i + j].col != kLightCheckerColor) {
          isLightRect = false;
          break;
        }
      }
      if (!isLightRect) {
        ++i;
        continue;
      }

      ImVec2 min = drawList->VtxBuffer[i].pos;
      ImVec2 max = drawList->VtxBuffer[i].pos;
      for (int j = 1; j < 4; ++j) {
        const ImVec2 pos = drawList->VtxBuffer[i + j].pos;
        min.x = std::min(min.x, pos.x);
        min.y = std::min(min.y, pos.y);
        max.x = std::max(max.x, pos.x);
        max.y = std::max(max.y, pos.y);
      }
      if (std::abs((max.x - min.x) - 4.0f) <= 0.001f &&
          std::abs((max.y - min.y) - 4.0f) <= 0.001f) {
        lightRects.push_back(RectBounds{.min = min, .max = max});
      }
      i += 4;
    }
    std::ranges::sort(lightRects, [](const RectBounds& lhs, const RectBounds& rhs) {
      if (std::abs(lhs.min.y - rhs.min.y) > 0.001f) {
        return lhs.min.y < rhs.min.y;
      }
      return lhs.min.x < rhs.min.x;
    });

    LabelAlignmentDiagnostics diagnostics;
    std::vector<RectBounds> previews;
    constexpr int kFullSizeLightRectsPerPreview = 18;
    for (std::size_t rectIndex = 0; rectIndex + kFullSizeLightRectsPerPreview <= lightRects.size();
         rectIndex += kFullSizeLightRectsPerPreview) {
      RectBounds preview = lightRects[rectIndex];
      for (std::size_t j = rectIndex + 1; j < rectIndex + kFullSizeLightRectsPerPreview; ++j) {
        preview.min.x = std::min(preview.min.x, lightRects[j].min.x);
        preview.min.y = std::min(preview.min.y, lightRects[j].min.y);
        preview.max.x = std::max(preview.max.x, lightRects[j].max.x);
        preview.max.y = std::max(preview.max.y, lightRects[j].max.y);
      }
      previews.push_back(preview);
    }
    if (!previews.empty()) {
      diagnostics.foundPreview = true;
      diagnostics.previewCenterY = (previews.front().min.y + previews.front().max.y) * 0.5f;
    }

    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    const float expectedLabelWidth =
        ImGui::CalcTextSize(label.data(), label.data() + label.size()).x;
    for (int i = 0; i < drawList->VtxBuffer.Size;) {
      if (drawList->VtxBuffer[i].col != textColor) {
        ++i;
        continue;
      }

      ImVec2 min = drawList->VtxBuffer[i].pos;
      ImVec2 max = drawList->VtxBuffer[i].pos;
      int j = i + 1;
      for (; j < drawList->VtxBuffer.Size && drawList->VtxBuffer[j].col == textColor; ++j) {
        const ImVec2 pos = drawList->VtxBuffer[j].pos;
        min.x = std::min(min.x, pos.x);
        min.y = std::min(min.y, pos.y);
        max.x = std::max(max.x, pos.x);
        max.y = std::max(max.y, pos.y);
      }

      const float width = max.x - min.x;
      if (diagnostics.foundPreview && min.x > 40.0f &&
          std::abs(width - expectedLabelWidth) <= 4.0f &&
          std::abs(((min.y + max.y) * 0.5f) - diagnostics.previewCenterY) < 16.0f) {
        diagnostics.foundLabel = true;
        diagnostics.labelCenterY = (min.y + max.y) * 0.5f;
        break;
      }
      i = j;
    }

    ImGui::End();
    ImGui::Render();
    return diagnostics;
  }

  std::optional<ThumbnailImageDiagnostics> ThumbnailImageDrawDiagnostics(
      LayersPanel& panel, EditorApp& app,
      const LayersPanel::ThumbnailTextureProvider& textureProvider, ImTextureID texture) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.AddMousePosEvent(-1.0f, -1.0f);
    io.AddMouseButtonEvent(0, false);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 250), ImGuiCond_Always);
    ImGui::Begin("##layers_thumbnail_uv_test", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    panel.render(&app, textureProvider);
    const ImDrawList* drawList = ImGui::GetWindowDrawList();
    std::optional<ThumbnailImageDiagnostics> diagnostics;
    for (int cmdIndex = 0; cmdIndex < drawList->CmdBuffer.Size; ++cmdIndex) {
      const ImDrawCmd& cmd = drawList->CmdBuffer[cmdIndex];
      if (cmd.GetTexID() != texture) {
        continue;
      }

      ImVec2 maxUv(0.0f, 0.0f);
      ImVec2 minPos(FLT_MAX, FLT_MAX);
      ImVec2 maxPos(-FLT_MAX, -FLT_MAX);
      for (unsigned int elemOffset = 0; elemOffset < cmd.ElemCount; ++elemOffset) {
        const int idxOffset = static_cast<int>(cmd.IdxOffset + elemOffset);
        const int vertexIndex =
            static_cast<int>(cmd.VtxOffset) + static_cast<int>(drawList->IdxBuffer[idxOffset]);
        const ImVec2 uv = drawList->VtxBuffer[vertexIndex].uv;
        const ImVec2 pos = drawList->VtxBuffer[vertexIndex].pos;
        maxUv.x = std::max(maxUv.x, uv.x);
        maxUv.y = std::max(maxUv.y, uv.y);
        minPos.x = std::min(minPos.x, pos.x);
        minPos.y = std::min(minPos.y, pos.y);
        maxPos.x = std::max(maxPos.x, pos.x);
        maxPos.y = std::max(maxPos.y, pos.y);
      }
      diagnostics = ThumbnailImageDiagnostics{
          .uvBottomRight = maxUv,
          .size = ImVec2(maxPos.x - minPos.x, maxPos.y - minPos.y),
      };
      break;
    }
    ImGui::End();
    ImGui::Render();
    return diagnostics;
  }

  ImGuiContext* ctx_ = nullptr;
};

TEST_F(LayersPanelImGuiTest, CheckerboardUsesFourPixelCells) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10" fill="red"/>
  </svg>)");
  LayersPanel panel;
  panel.refreshSnapshot(app);

  const CheckerboardDiagnostics checkerboard = MeasureCheckerboard(panel, app);
  EXPECT_EQ(checkerboard.lightRectCount, static_cast<int>(panel.visibleRowCount()) * 18);
  EXPECT_NEAR(checkerboard.maxLightRectWidth, 4.0f, 0.001f);
  EXPECT_NEAR(checkerboard.maxLightRectHeight, 4.0f, 0.001f);
}

TEST_F(LayersPanelImGuiTest, RowLabelIsVerticallyCenteredInThumbnailRow) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10" fill="red"/>
  </svg>)");
  LayersPanel panel;
  panel.refreshSnapshot(app);

  const LabelAlignmentDiagnostics alignment = MeasureLabelAlignment(panel, app, "rect1");
  ASSERT_TRUE(alignment.foundPreview) << "expected to find the rect row preview cell";
  ASSERT_TRUE(alignment.foundLabel) << "expected to find the rect row label text";
  EXPECT_NEAR(alignment.labelCenterY, alignment.previewCenterY, 0.75f)
      << "layer row text should be vertically centered against the rectangular thumbnail";
}

TEST_F(LayersPanelImGuiTest, ThumbnailImageUsesPayloadUv) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <g id="wide">
      <circle cx="5" cy="50" r="5" fill="red"/>
      <circle cx="95" cy="50" r="5" fill="blue"/>
    </g>
  </svg>)");
  LayersPanel panel;
  panel.refreshSnapshot(app);

  ImTextureID texture = static_cast<ImTextureID>(0x1234);
  const LayersPanel::ThumbnailTextureProvider textureProvider =
      [texture](std::uint64_t, const svg::RendererBitmap& bitmap) -> LayersPanel::ThumbnailTexture {
    if (bitmap.dimensions.x != 42 || bitmap.dimensions.y >= 24) {
      return LayersPanel::ThumbnailTexture{};
    }
    return LayersPanel::ThumbnailTexture{
        .texture = texture,
        .uvBottomRight = Vector2d(42.0 / 64.0, 5.0 / 8.0),
    };
  };

  const std::optional<ThumbnailImageDiagnostics> diagnostics =
      ThumbnailImageDrawDiagnostics(panel, app, textureProvider, texture);
  ASSERT_TRUE(diagnostics.has_value()) << "expected the row thumbnail to emit an image draw";
  EXPECT_NEAR(diagnostics->uvBottomRight.x, 42.0f / 64.0f, 0.001f);
  EXPECT_NEAR(diagnostics->uvBottomRight.y, 5.0f / 8.0f, 0.001f);
  EXPECT_NEAR(diagnostics->size.x, 42.0f, 0.001f);
  EXPECT_LT(diagnostics->size.y, 10.0f)
      << "wide, short thumbnails should draw at their returned content-sized height instead of "
         "stretching to the full row slot";
}

TEST_F(LayersPanelImGuiTest, DonnerSplashThumbnailImageDrawsAtRenderedBitmapSize) {
  EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  LayersPanel panel;
  panel.refreshSnapshot(app);

  const std::optional<LayerTreeRow> donnerRow = FindRow(panel, "Donner");
  ASSERT_TRUE(donnerRow.has_value());
  const svg::RendererBitmap* thumbnail = panel.rowThumbnail(donnerRow->stableId);
  ASSERT_NE(thumbnail, nullptr);
  ASSERT_FALSE(thumbnail->empty());

  ImTextureID texture = static_cast<ImTextureID>(0x3456);
  const LayersPanel::ThumbnailTextureProvider textureProvider =
      [targetStableId = donnerRow->stableId, texture](
          std::uint64_t stableId, const svg::RendererBitmap&) -> LayersPanel::ThumbnailTexture {
    if (stableId != targetStableId) {
      return LayersPanel::ThumbnailTexture{};
    }
    return LayersPanel::ThumbnailTexture{
        .texture = texture,
        .uvBottomRight = Vector2d(1.0, 1.0),
    };
  };

  const std::optional<ThumbnailImageDiagnostics> diagnostics =
      ThumbnailImageDrawDiagnostics(panel, app, textureProvider, texture);
  ASSERT_TRUE(diagnostics.has_value()) << "expected the Donner row thumbnail to emit an image draw";
  EXPECT_NEAR(diagnostics->size.x, static_cast<float>(thumbnail->dimensions.x), 0.001f);
  EXPECT_NEAR(diagnostics->size.y, static_cast<float>(thumbnail->dimensions.y), 0.001f);
}

TEST_F(LayersPanelImGuiTest, LayerAffordancesDoNotDrawTextGlyphPlaceholders) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
  </svg>)");
  LayersPanel panel;
  panel.refreshSnapshot(app);

  EXPECT_EQ(CountRightAffordanceTextRuns(panel, app), 0)
      << "layer visibility/lock affordances must be rendered SVG bitmap buttons, not the "
         "legacy one-character text placeholders";
}

TEST_F(LayersPanelImGuiTest, LayerAffordancesRenderSvgBitmapButtons) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
  </svg>)");
  LayersPanel panel;
  panel.refreshSnapshot(app);

  const IconButtonDiagnostics diagnostics = MeasureIconButtons(panel, app);
  EXPECT_GE(diagnostics.providerCalls, 2)
      << "visible/unlocked rows should request eye and lock icon textures";
  EXPECT_EQ(diagnostics.providerCalls, diagnostics.nonEmptyBitmaps)
      << "icon texture requests must carry Donner-rendered Bootstrap SVG bitmaps";
  EXPECT_EQ(diagnostics.providerCalls, diagnostics.retinaBitmaps)
      << "layer affordance icons are drawn at 14 logical px and must be rasterized at 2x or "
         "higher before ImGui scales them";
  EXPECT_GE(diagnostics.imageQuads, 2) << "eye/lock controls should emit image quads";
}

TEST_F(LayersPanelImGuiTest, LockButtonReceivesClick) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
  </svg>)");
  LayersPanel panel;
  panel.refreshSnapshot(app);
  ASSERT_TRUE(FindRow(panel, "rect1").has_value());
  ASSERT_FALSE(FindRow(panel, "rect1")->isLocked);

  // The lock button is the right-most affordance on row 0. With the window at
  // (0,0) sized 300×250 (no title bar), the content right edge is ~292px and the
  // lock button sits just inside it on the first row's line (~16px down).
  const ImVec2 lockButton(284.0f, 16.0f);
  Frame(panel, app, lockButton, /*down=*/false);  // hover / settle layout
  Frame(panel, app, lockButton, /*down=*/true);   // press
  Frame(panel, app, lockButton, /*down=*/false);  // release → SmallButton fires

  EXPECT_TRUE(app.document().flushFrame()) << "clicking the lock button should queue a mutation";
  panel.refreshSnapshot(app);
  const std::optional<LayerTreeRow> locked = FindRow(panel, "rect1");
  ASSERT_TRUE(locked.has_value());
  EXPECT_TRUE(locked->isLocked) << "lock button click was eaten by the row Selectable";
}

// ---------------------------------------------------------------------------
// Locked-rejection row flash: clicking a locked element flashes its Layers row
// red in sync with the canvas outline flash.
// ---------------------------------------------------------------------------

// Seam-level: an active flash whose element matches a visible row reports that
// row index + its fade intensity; an unset/zero-intensity flash reports none.
TEST(LayersPanelTest, LockedFlashReportsRowAndIntensity) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  LayersPanel panel;
  panel.refreshSnapshot(app);

  // No flash set: nothing flashes.
  EXPECT_EQ(panel.flashedRowIndex(), std::nullopt);
  EXPECT_FLOAT_EQ(panel.lockedRejectionFlashIntensity(), 0.0f);

  const int leafIdx = RowIndex(panel, "leaf");
  ASSERT_GE(leafIdx, 0);
  const svg::SVGElement leafElement = panel.rows()[static_cast<std::size_t>(leafIdx)].element;

  panel.setLockedRejectionFlash(
      LayersLockedRejectionFlash{.element = leafElement, .intensity = 0.6f});
  ASSERT_TRUE(panel.flashedRowIndex().has_value());
  EXPECT_EQ(*panel.flashedRowIndex(), static_cast<std::size_t>(leafIdx));
  EXPECT_FLOAT_EQ(panel.lockedRejectionFlashIntensity(), 0.6f);

  // A fully-faded (intensity 0) flash maps to no row even though it is set.
  panel.setLockedRejectionFlash(
      LayersLockedRejectionFlash{.element = leafElement, .intensity = 0.0f});
  EXPECT_EQ(panel.flashedRowIndex(), std::nullopt);

  // Clearing the flash drops it entirely.
  panel.setLockedRejectionFlash(
      LayersLockedRejectionFlash{.element = leafElement, .intensity = 0.6f});
  panel.setLockedRejectionFlash(std::nullopt);
  EXPECT_EQ(panel.flashedRowIndex(), std::nullopt);
  EXPECT_FLOAT_EQ(panel.lockedRejectionFlashIntensity(), 0.0f);
}

// ImGui-frame: an active flash paints the matching row's background red; no
// flash (or zero intensity) paints no red.
TEST_F(LayersPanelImGuiTest, LockedFlashPaintsRowBackgroundRed) {
  EditorApp app;
  LoadDocument(app, R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="rect1" x="0" y="0" width="10" height="10"/>
    <rect id="rect2" x="0" y="20" width="10" height="10"/>
  </svg>)");
  LayersPanel panel;
  panel.refreshSnapshot(app);
  const std::optional<LayerTreeRow> rect2Row = FindRow(panel, "rect2");
  ASSERT_TRUE(rect2Row.has_value());

  // No flash: the panel paints no flash-red anywhere.
  EXPECT_EQ(CountFlashRedVertices(panel, app), 0)
      << "Layers panel painted flash-red with no active locked-rejection flash";

  // Flash rect2's row: a red background rect appears (the row-rect quad
  // contributes flash-red vertices).
  panel.setLockedRejectionFlash(
      LayersLockedRejectionFlash{.element = rect2Row->element, .intensity = 1.0f});
  EXPECT_GT(CountFlashRedVertices(panel, app), 0)
      << "flashed locked row did not paint a red background";

  // Clearing the flash removes the red again.
  panel.setLockedRejectionFlash(std::nullopt);
  EXPECT_EQ(CountFlashRedVertices(panel, app), 0)
      << "row stayed red after the locked-rejection flash was cleared";
}

}  // namespace
}  // namespace donner::editor
