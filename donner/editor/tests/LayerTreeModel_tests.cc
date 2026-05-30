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

TEST(LayerTreeModelTest, VisualStackOrderListsLaterSiblingsFirst) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  LayerTreeModel model;
  model.refresh(app);

  // groupA, comp, leaf are top-level siblings in document order
  // (groupA -> comp -> leaf). Visual stack order paints later siblings on top,
  // so the row list lists them in reverse: leaf, then comp, then groupA.
  const int leafIdx = RowIndex(model, "leaf");
  const int compIdx = RowIndex(model, "comp");
  const int groupAIdx = RowIndex(model, "groupA");
  ASSERT_GE(leafIdx, 0);
  ASSERT_GE(compIdx, 0);
  ASSERT_GE(groupAIdx, 0);
  EXPECT_LT(leafIdx, compIdx) << "leaf (document-last) should sort above comp";
  EXPECT_LT(compIdx, groupAIdx) << "comp should sort above groupA (document-first)";
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
