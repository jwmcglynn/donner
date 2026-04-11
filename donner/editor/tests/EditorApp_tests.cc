#include "donner/editor/EditorApp.h"

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
  EXPECT_TRUE(app.selectedEntity() == entt::null);
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

  // Use a real entity from the document so that selection comparisons
  // don't have to fabricate entt::entity values.
  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(rect->entityHandle().entity());
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
  const Entity entity = rect->entityHandle().entity();

  app.applyMutation(EditorCommand::SetTransformCommand(
      entity, Transform2d::Translate(Vector2d(99.0, 0.0))));

  EXPECT_EQ(app.document().queue().size(), 1u);
  EXPECT_TRUE(app.flushFrame());
  EXPECT_EQ(app.document().queue().size(), 0u);
}

TEST(EditorAppTest, SelectionSetAndClear) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  const Entity entity = r1->entityHandle().entity();

  app.setSelection(entity);
  EXPECT_TRUE(app.hasSelection());
  EXPECT_TRUE(app.selectedEntity() == entity);

  app.setSelection(entt::null);
  EXPECT_FALSE(app.hasSelection());
}

}  // namespace
}  // namespace donner::editor
