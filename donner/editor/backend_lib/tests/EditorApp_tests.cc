#include "donner/editor/backend_lib/EditorApp.h"

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" fill="red"/>
         <rect id="r2" x="50" y="50" width="20" height="20" fill="blue"/>
       </svg>)";

TEST(EditorAppTest, EmptyByDefault) {
  EditorApp app;
  EXPECT_FALSE(app.hasDocument());
  EXPECT_FALSE(app.hasSelection());
  EXPECT_FALSE(app.selectedElement().has_value());
}

TEST(EditorAppTest, LoadFromStringPopulatesDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  EXPECT_TRUE(app.hasDocument());
  EXPECT_FALSE(app.hasSelection());

  EXPECT_TRUE(app.document().document().querySelector("#r1").has_value());
  EXPECT_TRUE(app.document().document().querySelector("#r2").has_value());
}

TEST(EditorAppTest, LoadFromStringClearsExistingSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);
  EXPECT_TRUE(app.hasSelection());

  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  EXPECT_FALSE(app.hasSelection());
}

TEST(EditorAppTest, HitTestReturnsTopElementAtPoint) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  // Inside r1 (10,10 → 30,30).
  auto inR1 = app.hitTest(Vector2d(15.0, 15.0));
  ASSERT_TRUE(inR1.has_value());
  EXPECT_EQ(inR1->id(), "r1");

  // Inside r2 (50,50 → 70,70).
  auto inR2 = app.hitTest(Vector2d(60.0, 60.0));
  ASSERT_TRUE(inR2.has_value());
  EXPECT_EQ(inR2->id(), "r2");

  // Empty space outside both rects.
  auto miss = app.hitTest(Vector2d(80.0, 80.0));
  EXPECT_FALSE(miss.has_value());
}

TEST(EditorAppTest, HitTestReturnsNulloptWhenNoDocument) {
  EditorApp app;
  EXPECT_FALSE(app.hitTest(Vector2d(0.0, 0.0)).has_value());
}

TEST(EditorAppTest, ApplyMutationFlowsThroughDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  app.applyMutation(
      EditorCommand::SetTransformCommand(*rect, Transform2d::Translate(Vector2d(99.0, 0.0))));

  EXPECT_EQ(app.document().queue().size(), 1u);
  EXPECT_TRUE(app.flushFrame());
  EXPECT_EQ(app.document().queue().size(), 0u);
}

TEST(EditorAppTest, SelectionSetAndClear) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());

  app.setSelection(*r1);
  EXPECT_TRUE(app.hasSelection());
  ASSERT_TRUE(app.selectedElement().has_value());
  EXPECT_TRUE(*app.selectedElement() == *r1);
  EXPECT_EQ(app.selectedElements().size(), 1u);

  app.setSelection(std::nullopt);
  EXPECT_FALSE(app.hasSelection());
  EXPECT_TRUE(app.selectedElements().empty());
}

// Multi-select API (Milestone 4 of the editor UX design doc): a
// vector-shaped backing store for shift+click and marquee
// selections, with a single-element compatibility shim for back-compat
// callers like the source-pane highlight and the inspector readout.
TEST(EditorAppTest, MultiSelectionStoresEveryElement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});
  EXPECT_TRUE(app.hasSelection());
  ASSERT_EQ(app.selectedElements().size(), 2u);
  EXPECT_TRUE(app.selectedElements()[0] == *r1);
  EXPECT_TRUE(app.selectedElements()[1] == *r2);
  // Single-element compat: returns the *first* element.
  ASSERT_TRUE(app.selectedElement().has_value());
  EXPECT_TRUE(*app.selectedElement() == *r1);

  // clearSelection reads as the natural opposite of "set N elements".
  app.clearSelection();
  EXPECT_FALSE(app.hasSelection());
  EXPECT_TRUE(app.selectedElements().empty());
  EXPECT_FALSE(app.selectedElement().has_value());
}

TEST(EditorAppTest, ToggleInSelectionAddsThenRemoves) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  app.toggleInSelection(*r1);
  EXPECT_EQ(app.selectedElements().size(), 1u);
  EXPECT_TRUE(app.selectedElements()[0] == *r1);

  app.toggleInSelection(*r2);
  EXPECT_EQ(app.selectedElements().size(), 2u);

  // Toggling an already-selected element removes it without
  // disturbing the other entries.
  app.toggleInSelection(*r1);
  EXPECT_EQ(app.selectedElements().size(), 1u);
  EXPECT_TRUE(app.selectedElements()[0] == *r2);

  // Re-toggling brings it back.
  app.toggleInSelection(*r1);
  EXPECT_EQ(app.selectedElements().size(), 2u);
}

TEST(EditorAppTest, AddToSelectionIsIdempotent) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());

  app.addToSelection(*r1);
  app.addToSelection(*r1);
  app.addToSelection(*r1);
  EXPECT_EQ(app.selectedElements().size(), 1u);
}

TEST(EditorAppTest, HitTestRectFindsAllIntersectingElements) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  // r1 lives at (10,10..30,30), r2 at (50,50..70,70). A marquee
  // covering the full document grabs both.
  auto bothHits = app.hitTestRect(Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));
  EXPECT_EQ(bothHits.size(), 2u);

  // A marquee that only overlaps r1 returns just r1.
  auto r1Only = app.hitTestRect(Box2d::FromXYWH(5.0, 5.0, 20.0, 20.0));
  ASSERT_EQ(r1Only.size(), 1u);
  EXPECT_EQ(r1Only[0].id(), "r1");

  // A marquee that misses both returns empty.
  auto noHits = app.hitTestRect(Box2d::FromXYWH(80.0, 80.0, 5.0, 5.0));
  EXPECT_TRUE(noHits.empty());

  // Edge contact (marquee touches r1's edge) counts as intersection.
  auto edgeHits = app.hitTestRect(Box2d::FromXYWH(30.0, 30.0, 5.0, 5.0));
  ASSERT_EQ(edgeHits.size(), 1u);
  EXPECT_EQ(edgeHits[0].id(), "r1");
}

TEST(EditorAppTest, HitTestRectReturnsEmptyWithoutDocument) {
  EditorApp app;
  EXPECT_TRUE(app.hitTestRect(Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0)).empty());
}

TEST(EditorAppTest, SyncDirtyFromSourceClearsWhenTextReturnsToCleanBaseline) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  app.setCleanSourceText(kTrivialSvg);
  EXPECT_FALSE(app.isDirty());

  const std::string edited = std::string(kTrivialSvg) + "\n<!-- edit -->\n";
  app.syncDirtyFromSource(edited);
  EXPECT_TRUE(app.isDirty());

  app.syncDirtyFromSource(kTrivialSvg);
  EXPECT_FALSE(app.isDirty());
}

}  // namespace
}  // namespace donner::editor
