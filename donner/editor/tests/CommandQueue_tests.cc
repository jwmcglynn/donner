#include "donner/editor/CommandQueue.h"

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

// Helpers — none of these tests need a real Registry, so we fabricate
// `Entity` values directly via `entt::entity` casts.
constexpr Entity kEntityA = static_cast<Entity>(1);
constexpr Entity kEntityB = static_cast<Entity>(2);
constexpr Entity kEntityC = static_cast<Entity>(3);

Transform2d MakeTranslation(double dx, double dy) {
  return Transform2d::Translate(Vector2d(dx, dy));
}

TEST(CommandQueueTest, EmptyFlushReturnsNothing) {
  CommandQueue queue;
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);

  const auto effective = queue.flush();
  EXPECT_TRUE(effective.empty());
  EXPECT_TRUE(queue.empty());
}

TEST(CommandQueueTest, SetTransformsForDifferentEntitiesPreserveOrder) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(kEntityA, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(kEntityB, MakeTranslation(0.0, 2.0)));
  queue.push(EditorCommand::SetTransformCommand(kEntityC, MakeTranslation(3.0, 4.0)));

  const auto effective = queue.flush();
  ASSERT_EQ(effective.size(), 3u);
  EXPECT_EQ(effective[0].entity, kEntityA);
  EXPECT_EQ(effective[1].entity, kEntityB);
  EXPECT_EQ(effective[2].entity, kEntityC);
  EXPECT_TRUE(queue.empty());
}

TEST(CommandQueueTest, MultipleSetTransformsForSameEntityCollapse) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(kEntityA, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(kEntityA, MakeTranslation(2.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(kEntityA, MakeTranslation(3.0, 0.0)));

  const auto effective = queue.flush();
  ASSERT_EQ(effective.size(), 1u);
  EXPECT_EQ(effective[0].entity, kEntityA);
  // The most recent transform (translation by 3.0) wins.
  EXPECT_DOUBLE_EQ(effective[0].transform.data[4], 3.0);
}

TEST(CommandQueueTest, CoalescingPreservesPerEntityOrderForDistinctEntities) {
  CommandQueue queue;
  // A, B, A, B — should collapse to two effective entries, A then B in
  // their first-seen order.
  queue.push(EditorCommand::SetTransformCommand(kEntityA, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(kEntityB, MakeTranslation(2.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(kEntityA, MakeTranslation(3.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(kEntityB, MakeTranslation(4.0, 0.0)));

  const auto effective = queue.flush();
  ASSERT_EQ(effective.size(), 2u);
  EXPECT_EQ(effective[0].entity, kEntityA);
  EXPECT_DOUBLE_EQ(effective[0].transform.data[4], 3.0);
  EXPECT_EQ(effective[1].entity, kEntityB);
  EXPECT_DOUBLE_EQ(effective[1].transform.data[4], 4.0);
}

TEST(CommandQueueTest, ReplaceDocumentDropsAllPriorCommands) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(kEntityA, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(kEntityB, MakeTranslation(2.0, 0.0)));
  queue.push(EditorCommand::ReplaceDocumentCommand("<svg/>"));

  const auto effective = queue.flush();
  ASSERT_EQ(effective.size(), 1u);
  EXPECT_EQ(effective[0].kind, EditorCommand::Kind::ReplaceDocument);
  EXPECT_EQ(effective[0].bytes, "<svg/>");
}

TEST(CommandQueueTest, CommandsAfterReplaceDocumentSurvive) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(kEntityA, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::ReplaceDocumentCommand("<svg/>"));
  queue.push(EditorCommand::SetTransformCommand(kEntityB, MakeTranslation(5.0, 0.0)));

  const auto effective = queue.flush();
  ASSERT_EQ(effective.size(), 2u);
  EXPECT_EQ(effective[0].kind, EditorCommand::Kind::ReplaceDocument);
  EXPECT_EQ(effective[1].kind, EditorCommand::Kind::SetTransform);
  EXPECT_EQ(effective[1].entity, kEntityB);
}

TEST(CommandQueueTest, MultipleReplaceDocumentKeepsOnlyLatestAndCommandsAfter) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(kEntityA, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::ReplaceDocumentCommand("first"));
  queue.push(EditorCommand::SetTransformCommand(kEntityB, MakeTranslation(2.0, 0.0)));
  queue.push(EditorCommand::ReplaceDocumentCommand("second"));
  queue.push(EditorCommand::SetTransformCommand(kEntityC, MakeTranslation(3.0, 0.0)));

  const auto effective = queue.flush();
  ASSERT_EQ(effective.size(), 2u);
  EXPECT_EQ(effective[0].kind, EditorCommand::Kind::ReplaceDocument);
  EXPECT_EQ(effective[0].bytes, "second");
  EXPECT_EQ(effective[1].kind, EditorCommand::Kind::SetTransform);
  EXPECT_EQ(effective[1].entity, kEntityC);
}

TEST(CommandQueueTest, ClearDropsPendingWithoutFlushing) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(kEntityA, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(kEntityB, MakeTranslation(2.0, 0.0)));
  EXPECT_EQ(queue.size(), 2u);

  queue.clear();
  EXPECT_TRUE(queue.empty());

  const auto effective = queue.flush();
  EXPECT_TRUE(effective.empty());
}

}  // namespace
}  // namespace donner::editor
