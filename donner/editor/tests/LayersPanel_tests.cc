#include "donner/editor/LayersPanel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <string_view>
#include <type_traits>

#include "donner/editor/CompositorDebugPanel.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/LayerTreeModel.h"
#include "donner/svg/SVGElement.h"

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

}  // namespace
}  // namespace donner::editor
