#include "donner/editor/LayersPanel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>

#include "donner/editor/CompositorDebugPanel.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LayerTreeModel.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/renderer/RendererInterface.h"

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

// Read the RGBA pixel at (x, y) from a renderer bitmap (row-bytes aware).
std::array<int, 4> ThumbnailPixelAt(const svg::RendererBitmap& bitmap, int x, int y) {
  const std::size_t index =
      static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
  return {bitmap.pixels[index + 0], bitmap.pixels[index + 1], bitmap.pixels[index + 2],
          bitmap.pixels[index + 3]};
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

  const std::array<int, 4> center = ThumbnailPixelAt(*thumbnail, 12, 12);
  EXPECT_NEAR(center[0], 220, 6) << "thumbnail center red channel";
  EXPECT_NEAR(center[1], 0, 6) << "thumbnail center green channel";
  EXPECT_NEAR(center[2], 0, 6) << "thumbnail center blue channel";
  EXPECT_NEAR(center[3], 255, 6) << "thumbnail center alpha";
}

TEST(LayersPanelTest, GroupThumbnailComposesChildren) {
  EditorApp app;
  ASSERT_TRUE(
      app.loadFromString(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
    <g id="halves">
      <rect x="0" y="0" width="50" height="100" fill="rgb(0,0,210)"/>
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

  const std::array<int, 4> leftPixel = ThumbnailPixelAt(*thumbnail, 6, 12);
  const std::array<int, 4> rightPixel = ThumbnailPixelAt(*thumbnail, 17, 12);
  EXPECT_NEAR(leftPixel[2], 210, 8) << "left half is blue";
  EXPECT_NEAR(rightPixel[0], 210, 8) << "right half is yellow (red channel)";
  EXPECT_NEAR(rightPixel[1], 210, 8) << "right half is yellow (green channel)";
}

// ---------------------------------------------------------------------------
// Render-path: the right-aligned eye/lock buttons must actually receive clicks.
// Regression for a QA-found "lock/hide buttons don't respond" — the full-width
// SpanAllColumns row Selectable needs AllowOverlap or it eats the buttons' clicks.
// ---------------------------------------------------------------------------

class LayersPanelImGuiTest : public ::testing::Test {
protected:
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

  ImGuiContext* ctx_ = nullptr;
};

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
