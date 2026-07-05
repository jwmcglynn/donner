#include "donner/editor/LayerTreeModel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/EditorApp.h"
#include "donner/svg/DocumentState.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {
namespace {

using ::testing::AllOf;
using ::testing::HasSubstr;
using ::testing::MatchesRegex;

// Synthetic document exercising every row kind plus a non-rendered resource:
//   <svg>
//     <defs><linearGradient id="grad"/></defs>   <- excluded
//     <g id="groupA">
//       <g>                                       <- no id, indexed fallback name
//         <rect id="rectTop"/>
//         <rect id="rectBottom"/>
//       </g>
//     </g>
//     <path id="comp" d="M.. Z M.. Z"/>           <- compound path (two subpaths)
//     <circle id="leaf"/>                          <- shape
constexpr std::string_view kSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <defs>
    <linearGradient id="grad"><stop offset="0" stop-color="red"/></linearGradient>
  </defs>
  <g id="groupA">
    <g>
      <rect id="rectTop" x="0" y="0" width="10" height="10" fill="red"/>
      <rect id="rectBottom" x="0" y="20" width="10" height="10" fill="blue"/>
    </g>
  </g>
  <path id="comp" d="M0 0 L10 0 L10 10 Z M20 20 L30 20 L30 30 Z" fill="green"/>
  <circle id="leaf" cx="50" cy="50" r="5" fill="purple"/>
</svg>)";

constexpr std::string_view kRenderableLeavesSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <rect id="rect" x="0" y="0" width="10" height="10"/>
  <circle id="circle" cx="20" cy="20" r="5"/>
  <ellipse id="ellipse" cx="40" cy="20" rx="5" ry="3"/>
  <line id="line" x1="0" y1="40" x2="20" y2="40"/>
  <polyline id="polyline" points="0,60 10,70"/>
  <polygon id="polygon" points="30,60 40,70 20,70"/>
  <path id="path" d="M0 80 L10 80"/>
  <text id="text" x="0" y="100">Label</text>
  <image id="image" href="image.png" width="10" height="10"/>
  <use id="use" href="#rect"/>
</svg>)";

constexpr std::string_view kExcludedResourcesSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <defs id="defs"/>
  <linearGradient id="linearGradient"/>
  <radialGradient id="radialGradient"/>
  <pattern id="pattern"/>
  <filter id="filter"/>
  <clipPath id="clipPath"/>
  <mask id="mask"/>
  <marker id="marker"/>
  <symbol id="symbol"/>
  <style id="style">rect { fill: red; }</style>
  <stop id="stop"/>
  <rect id="visible" x="0" y="0" width="10" height="10"/>
</svg>)";

constexpr std::string_view kVisibilitySvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <rect id="displayNone" display="none" x="0" y="0" width="10" height="10"/>
  <rect id="visibilityHidden" visibility="hidden" x="20" y="0" width="10" height="10"/>
  <rect id="visible" x="40" y="0" width="10" height="10"/>
</svg>)";

constexpr std::string_view kNonRenderableFallbackSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <a id="link"><rect id="linkedRect" x="0" y="0" width="10" height="10"/></a>
  <switch id="switch"><rect id="switchRect" x="20" y="0" width="10" height="10"/></switch>
  <feGaussianBlur id="blur" stdDeviation="2"/>
  <unknown id="custom"/>
  <rect id="visible" x="40" y="0" width="10" height="10"/>
</svg>)";

constexpr std::string_view kUnnamedSiblingsSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <rect x="0" y="0" width="10" height="10"/>
  <circle cx="20" cy="5" r="5"/>
  <rect x="30" y="0" width="10" height="10"/>
  <rect x="50" y="0" width="10" height="10"/>
</svg>)";

constexpr std::string_view kLockedRowsSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <g id="lockedGroup" data-donner-locked="true">
    <rect id="lockedChild" x="0" y="0" width="10" height="10"/>
  </g>
  <rect id="free" x="20" y="0" width="10" height="10"/>
</svg>)";

// Find the row whose display name equals `name`, or null.
const LayerTreeRow* FindRow(const LayerTreeModel& model, std::string_view name) {
  for (const LayerTreeRow& row : model.rows()) {
    if (row.displayName == name) {
      return &row;
    }
  }
  return nullptr;
}

std::vector<std::string> RowNames(const LayerTreeModel& model) {
  std::vector<std::string> names;
  for (const LayerTreeRow& row : model.rows()) {
    names.push_back(row.displayName);
  }
  return names;
}

// Index of the first row with display name `name`, or -1.
int RowIndex(const LayerTreeModel& model, std::string_view name) {
  const std::vector<LayerTreeRow>& rows = model.rows();
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    if (rows[i].displayName == name) {
      return i;
    }
  }
  return -1;
}

// Resolve an element by display name through a fully-expanded model snapshot.
// This avoids `SVGDocument::querySelector` overload-resolution quirks in the
// test TU and keeps element lookups going through the same row data the model
// produces.
std::optional<svg::SVGElement> FindElement(EditorApp& app, std::string_view name) {
  LayerTreeModel scratch;
  scratch.refresh(app);
  // Expand every group so descendant rows are present, then re-walk.
  for (const LayerTreeRow& row : scratch.rows()) {
    if (row.hasChildren) {
      scratch.setExpanded(row.stableId, true);
    }
  }
  scratch.refresh(app);
  for (const LayerTreeRow& row : scratch.rows()) {
    if (row.hasChildren) {
      scratch.setExpanded(row.stableId, true);
    }
  }
  scratch.refresh(app);
  for (const LayerTreeRow& row : scratch.rows()) {
    if (row.displayName == name) {
      return row.element;
    }
  }
  return std::nullopt;
}

std::uint64_t StableIdOf(EditorApp& app, std::string_view name) {
  return LayerTreeModel::StableIdFor(*FindElement(app, name));
}

TEST(LayerTreeModelTest, RefreshWithoutDocumentClearsRowsAndExpansionCanToggle) {
  EditorApp app;
  LayerTreeModel model;

  model.toggleExpanded(42);
  EXPECT_TRUE(model.isExpanded(42));
  model.toggleExpanded(42);
  EXPECT_FALSE(model.isExpanded(42));

  model.setExpanded(7, true);
  EXPECT_TRUE(model.isExpanded(7));
  model.refresh(app);
  EXPECT_TRUE(model.rows().empty());
  EXPECT_TRUE(model.isExpanded(7)) << "refresh without a document should not clear expansion state";
}

TEST(LayerTreeModelTest, ExcludesNonRenderedResourcesAndKeepsRenderableRows) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  LayerTreeModel model;
  model.refresh(app);

  // Root <svg> and the top-level groups/leaves are present; the
  // <defs>/<linearGradient> resources are excluded entirely.
  EXPECT_THAT(RowNames(model), testing::Contains("groupA"));
  EXPECT_THAT(RowNames(model), testing::Contains("comp"));
  EXPECT_THAT(RowNames(model), testing::Contains("leaf"));
  EXPECT_THAT(RowNames(model), testing::Not(testing::Contains("grad")));
  EXPECT_THAT(RowNames(model), testing::Not(testing::Contains(HasSubstr("linearGradient"))));
  EXPECT_THAT(RowNames(model), testing::Not(testing::Contains(HasSubstr("defs"))));
}

TEST(LayerTreeModelTest, ExcludesAllResourceRowTypes) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kExcludedResourcesSvg));

  LayerTreeModel model;
  model.refresh(app);

  EXPECT_THAT(RowNames(model), testing::Contains("visible"));
  for (std::string_view name : {"defs", "linearGradient", "radialGradient", "pattern", "filter",
                                "clipPath", "mask", "marker", "symbol", "style", "stop"}) {
    EXPECT_THAT(RowNames(model), testing::Not(testing::Contains(std::string(name))));
  }
}

TEST(LayerTreeModelTest, ExcludesNonRenderableFallbackTypes) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kNonRenderableFallbackSvg));

  LayerTreeModel model;
  model.refresh(app);

  EXPECT_THAT(RowNames(model), testing::ElementsAre("visible"));
}

TEST(LayerTreeModelTest, IncludesAllRenderableLeafTypesAsShapes) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRenderableLeavesSvg));

  LayerTreeModel model;
  model.refresh(app);

  for (std::string_view name : {"rect", "circle", "ellipse", "line", "polyline", "polygon", "path",
                                "text", "image", "use"}) {
    const LayerTreeRow* row = FindRow(model, name);
    ASSERT_NE(row, nullptr) << name;
    EXPECT_EQ(row->kind, LayerRowKind::Shape) << name;
  }
}

TEST(LayerTreeModelTest, ClassifiesRowKinds) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  LayerTreeModel model;
  model.refresh(app);

  // Expand groupA so the subgroup and its descendant rows appear.
  model.setExpanded(StableIdOf(app, "groupA"), true);
  model.refresh(app);

  const LayerTreeRow* groupA = FindRow(model, "groupA");
  ASSERT_NE(groupA, nullptr);
  EXPECT_EQ(groupA->kind, LayerRowKind::Group);

  // The document root <svg> is omitted from the panel: the top-level
  // groups/shapes are the tree roots at depth 0, and no row classifies as Root.
  ASSERT_FALSE(model.rows().empty());
  for (const LayerTreeRow& row : model.rows()) {
    EXPECT_NE(row.kind, LayerRowKind::Root) << "the <svg> root row is no longer shown";
  }
  EXPECT_EQ(groupA->depth, 0) << "top-level groups are tree roots at depth 0";

  const LayerTreeRow* comp = FindRow(model, "comp");
  ASSERT_NE(comp, nullptr);
  EXPECT_EQ(comp->kind, LayerRowKind::CompoundPath)
      << "multi-subpath <path> should classify as CompoundPath";

  const LayerTreeRow* leaf = FindRow(model, "leaf");
  ASSERT_NE(leaf, nullptr);
  EXPECT_EQ(leaf->kind, LayerRowKind::Shape);
}

TEST(LayerTreeModelTest, IdFirstNamingWithIndexedFallback) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  LayerTreeModel model;
  model.setExpanded(StableIdOf(app, "groupA"), true);
  model.refresh(app);

  // id-first naming.
  EXPECT_NE(FindRow(model, "groupA"), nullptr);
  EXPECT_NE(FindRow(model, "leaf"), nullptr);

  // The no-id subgroup falls back to an indexed "<g>[n]" style name.
  bool foundIndexedG = false;
  for (const LayerTreeRow& row : model.rows()) {
    if (row.kind == LayerRowKind::Group && row.displayName != "groupA") {
      EXPECT_THAT(row.displayName, AllOf(HasSubstr("g"), MatchesRegex(R"(<g>\[[0-9]+\])")));
      foundIndexedG = true;
    }
  }
  EXPECT_TRUE(foundIndexedG) << "expected the unnamed subgroup to use an indexed fallback name";
}

TEST(LayerTreeModelTest, IndexedFallbackCountsPreviousSameTagSiblings) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kUnnamedSiblingsSvg));

  LayerTreeModel model;
  model.refresh(app);

  EXPECT_THAT(RowNames(model),
              testing::ElementsAre("<rect>[0]", "<circle>[0]", "<rect>[1]", "<rect>[2]"));
}

TEST(LayerTreeModelTest, StackOrderListsBackToFrontInDocumentOrder) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  LayerTreeModel model;
  model.refresh(app);

  // groupA, comp, leaf are top-level siblings in document order
  // (groupA -> comp -> leaf). The panel lists layers back-to-front: the
  // first-painted (bottom/back) element at the top of the list and the
  // last-painted (top/front) element at the bottom, matching document order.
  const int groupAIdx = RowIndex(model, "groupA");
  const int compIdx = RowIndex(model, "comp");
  const int leafIdx = RowIndex(model, "leaf");
  ASSERT_GE(groupAIdx, 0);
  ASSERT_GE(compIdx, 0);
  ASSERT_GE(leafIdx, 0);
  EXPECT_LT(groupAIdx, compIdx) << "groupA (document-first/back) should list above comp";
  EXPECT_LT(compIdx, leafIdx) << "comp should list above leaf (document-last/front)";
}

TEST(LayerTreeModelTest, VisibilityFlagsReflectDisplayAndVisibilityProperties) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kVisibilitySvg));

  LayerTreeModel model;
  model.refresh(app);

  const LayerTreeRow* displayNone = FindRow(model, "displayNone");
  ASSERT_NE(displayNone, nullptr);
  EXPECT_FALSE(displayNone->isVisible);

  const LayerTreeRow* visibilityHidden = FindRow(model, "visibilityHidden");
  ASSERT_NE(visibilityHidden, nullptr);
  EXPECT_FALSE(visibilityHidden->isVisible);

  const LayerTreeRow* visible = FindRow(model, "visible");
  ASSERT_NE(visible, nullptr);
  EXPECT_TRUE(visible->isVisible);
}

TEST(LayerTreeModelTest, LockedRowsReflectAncestorLockState) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kLockedRowsSvg));

  LayerTreeModel model;
  model.refresh(app);

  const LayerTreeRow* lockedGroup = FindRow(model, "lockedGroup");
  ASSERT_NE(lockedGroup, nullptr);
  EXPECT_TRUE(lockedGroup->isLocked);

  const LayerTreeRow* lockedChild = FindRow(model, "lockedChild");
  ASSERT_NE(lockedChild, nullptr);
  EXPECT_TRUE(lockedChild->isLocked);

  const LayerTreeRow* free = FindRow(model, "free");
  ASSERT_NE(free, nullptr);
  EXPECT_FALSE(free->isLocked);
}

TEST(LayerTreeModelTest, ExpansionAndSelectionPersistAcrossRefresh) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  const std::uint64_t groupAId = StableIdOf(app, "groupA");
  const std::uint64_t leafId = StableIdOf(app, "leaf");

  std::optional<svg::SVGElement> leaf = FindElement(app, "leaf");
  ASSERT_TRUE(leaf.has_value());
  app.setSelection(*leaf);

  LayerTreeModel model;
  model.setExpanded(groupAId, true);
  model.refresh(app);
  EXPECT_TRUE(model.isExpanded(groupAId));

  const LayerTreeRow* leafRow = FindRow(model, "leaf");
  ASSERT_NE(leafRow, nullptr);
  EXPECT_TRUE(leafRow->isSelected);

  // A second refresh (mirroring an idle snapshot rebuild) preserves both the
  // expansion set keyed by stableId and the selection mirror.
  model.refresh(app);
  EXPECT_TRUE(model.isExpanded(groupAId)) << "expansion must survive refresh, keyed by stableId";
  const LayerTreeRow* leafRow2 = FindRow(model, "leaf");
  ASSERT_NE(leafRow2, nullptr);
  EXPECT_EQ(leafRow2->stableId, leafId);
  EXPECT_TRUE(leafRow2->isSelected) << "selection mirror must survive refresh";
}

TEST(LayerTreeModelTest, PartialSelectionReflectsDescendantSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  // Select a shape nested inside groupA -> groupA is partially (not fully)
  // selected.
  std::optional<svg::SVGElement> rect = FindElement(app, "rectTop");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  LayerTreeModel model;
  model.setExpanded(StableIdOf(app, "groupA"), true);
  model.refresh(app);

  const LayerTreeRow* groupA = FindRow(model, "groupA");
  ASSERT_NE(groupA, nullptr);
  EXPECT_FALSE(groupA->isSelected);
  EXPECT_TRUE(groupA->isPartiallySelected)
      << "a group with a selected descendant but not itself selected is partial";

  // Selecting groupA itself makes it fully selected, not partial.
  std::optional<svg::SVGElement> groupAElement = FindElement(app, "groupA");
  ASSERT_TRUE(groupAElement.has_value());
  app.setSelection(*groupAElement);
  model.refresh(app);

  const LayerTreeRow* groupA2 = FindRow(model, "groupA");
  ASSERT_NE(groupA2, nullptr);
  EXPECT_TRUE(groupA2->isSelected);
  EXPECT_FALSE(groupA2->isPartiallySelected);
}

TEST(LayerTreeModelTest, CollapsedGroupHidesDescendantsButKeepsChevron) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  LayerTreeModel model;
  // The first refresh default-expands top-level groups; explicitly collapse
  // groupA afterward to exercise the collapse path.
  model.refresh(app);
  model.setExpanded(StableIdOf(app, "groupA"), false);
  model.refresh(app);

  const LayerTreeRow* groupA = FindRow(model, "groupA");
  ASSERT_NE(groupA, nullptr);
  EXPECT_TRUE(groupA->hasChildren) << "collapsed group still advertises children for the chevron";
  EXPECT_FALSE(groupA->isExpanded);

  // Descendant rows are omitted while groupA is collapsed.
  EXPECT_EQ(FindRow(model, "rectTop"), nullptr);
}

}  // namespace
}  // namespace donner::editor
