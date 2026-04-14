#include "donner/editor/CommandQueue.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

// CommandQueue carries `svg::SVGElement` payloads in SetTransform /
// DeleteElement commands. Elements can only be produced via public donner
// APIs (`querySelector`, tree traversal) so each test parses a trivial
// SVG and references its `<rect>` children by id. This reflects the way
// real callers build commands in the editor — via hit-test results and
// source-pane queries, never via raw entity ids.

constexpr std::string_view kThreeRectsSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="300" height="100">
         <rect id="a" x="0"   y="0" width="50" height="50"/>
         <rect id="b" x="100" y="0" width="50" height="50"/>
         <rect id="c" x="200" y="0" width="50" height="50"/>
       </svg>)";

class CommandQueueTest : public ::testing::Test {
protected:
  void SetUp() override {
    ParseWarningSink sink = ParseWarningSink::Disabled();
    auto result = svg::parser::SVGParser::ParseSVG(kThreeRectsSvg, sink);
    ASSERT_FALSE(result.hasError());
    doc.emplace(std::move(result.result()));
    a = doc->querySelector("#a");
    b = doc->querySelector("#b");
    c = doc->querySelector("#c");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());
  }

  static Transform2d MakeTranslation(double dx, double dy) {
    return Transform2d::Translate(Vector2d(dx, dy));
  }

  std::optional<svg::SVGDocument> doc;
  std::optional<svg::SVGElement> a;
  std::optional<svg::SVGElement> b;
  std::optional<svg::SVGElement> c;
};

TEST_F(CommandQueueTest, EmptyFlushReturnsNothing) {
  CommandQueue queue;
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  EXPECT_TRUE(effective.empty());
  EXPECT_FALSE(flushResult.hadReplaceDocument);
  EXPECT_FALSE(flushResult.preserveUndoOnReparse);
  EXPECT_TRUE(queue.empty());
}

TEST_F(CommandQueueTest, SetTransformsForDifferentEntitiesPreserveOrder) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(*b, MakeTranslation(0.0, 2.0)));
  queue.push(EditorCommand::SetTransformCommand(*c, MakeTranslation(3.0, 4.0)));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  ASSERT_EQ(effective.size(), 3u);
  EXPECT_TRUE(effective[0].element == a);
  EXPECT_TRUE(effective[1].element == b);
  EXPECT_TRUE(effective[2].element == c);
  EXPECT_TRUE(queue.empty());
}

TEST_F(CommandQueueTest, MultipleSetTransformsForSameEntityCollapse) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(2.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(3.0, 0.0)));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  ASSERT_EQ(effective.size(), 1u);
  EXPECT_TRUE(effective[0].element == a);
  // The most recent transform (translation by 3.0) wins.
  EXPECT_DOUBLE_EQ(effective[0].transform.data[4], 3.0);
}

TEST_F(CommandQueueTest, CoalescingPreservesPerEntityOrderForDistinctEntities) {
  CommandQueue queue;
  // A, B, A, B — should collapse to two effective entries, A then B in
  // their first-seen order.
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(*b, MakeTranslation(2.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(3.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(*b, MakeTranslation(4.0, 0.0)));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  ASSERT_EQ(effective.size(), 2u);
  EXPECT_TRUE(effective[0].element == a);
  EXPECT_DOUBLE_EQ(effective[0].transform.data[4], 3.0);
  EXPECT_TRUE(effective[1].element == b);
  EXPECT_DOUBLE_EQ(effective[1].transform.data[4], 4.0);
}

TEST_F(CommandQueueTest, ReplaceDocumentDropsAllPriorCommands) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(*b, MakeTranslation(2.0, 0.0)));
  queue.push(EditorCommand::ReplaceDocumentCommand("<svg/>"));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  ASSERT_EQ(effective.size(), 1u);
  EXPECT_EQ(effective[0].kind, EditorCommand::Kind::ReplaceDocument);
  EXPECT_EQ(effective[0].bytes, "<svg/>");
  EXPECT_TRUE(flushResult.hadReplaceDocument);
  EXPECT_FALSE(flushResult.preserveUndoOnReparse);
}

TEST_F(CommandQueueTest, CommandsAfterReplaceDocumentSurvive) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::ReplaceDocumentCommand("<svg/>"));
  queue.push(EditorCommand::SetTransformCommand(*b, MakeTranslation(5.0, 0.0)));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  ASSERT_EQ(effective.size(), 2u);
  EXPECT_EQ(effective[0].kind, EditorCommand::Kind::ReplaceDocument);
  EXPECT_EQ(effective[1].kind, EditorCommand::Kind::SetTransform);
  EXPECT_TRUE(effective[1].element == b);
}

TEST_F(CommandQueueTest, MultipleReplaceDocumentKeepsOnlyLatestAndCommandsAfter) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::ReplaceDocumentCommand("first"));
  queue.push(EditorCommand::SetTransformCommand(*b, MakeTranslation(2.0, 0.0)));
  queue.push(EditorCommand::ReplaceDocumentCommand("second"));
  queue.push(EditorCommand::SetTransformCommand(*c, MakeTranslation(3.0, 0.0)));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  ASSERT_EQ(effective.size(), 2u);
  EXPECT_EQ(effective[0].kind, EditorCommand::Kind::ReplaceDocument);
  EXPECT_EQ(effective[0].bytes, "second");
  EXPECT_EQ(effective[1].kind, EditorCommand::Kind::SetTransform);
  EXPECT_TRUE(effective[1].element == c);
  EXPECT_TRUE(flushResult.hadReplaceDocument);
  EXPECT_FALSE(flushResult.preserveUndoOnReparse);
}

TEST_F(CommandQueueTest, PreserveUndoReparseSurvivesSingleReplaceDocumentBatch) {
  CommandQueue queue;
  queue.push(EditorCommand::ReplaceDocumentCommand("writeback", /*preserveUndoOnReparse=*/true));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  ASSERT_EQ(effective.size(), 1u);
  EXPECT_TRUE(flushResult.hadReplaceDocument);
  EXPECT_TRUE(flushResult.preserveUndoOnReparse);
  EXPECT_TRUE(effective.front().preserveUndoOnReparse);
}

TEST_F(CommandQueueTest, UserReplaceInMixedBatchClearsPreserveUndoMetadata) {
  CommandQueue queue;
  queue.push(EditorCommand::ReplaceDocumentCommand("writeback", /*preserveUndoOnReparse=*/true));
  queue.push(EditorCommand::ReplaceDocumentCommand("user-edit"));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  ASSERT_EQ(effective.size(), 1u);
  EXPECT_EQ(effective.front().bytes, "user-edit");
  EXPECT_TRUE(flushResult.hadReplaceDocument);
  EXPECT_FALSE(flushResult.preserveUndoOnReparse);
}

TEST_F(CommandQueueTest, ClearDropsPendingWithoutFlushing) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(*b, MakeTranslation(2.0, 0.0)));
  EXPECT_EQ(queue.size(), 2u);

  queue.clear();
  EXPECT_TRUE(queue.empty());

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  EXPECT_TRUE(effective.empty());
}

TEST_F(CommandQueueTest, DeleteElementCommandNotCoalesced) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::DeleteElementCommand(*b));
  queue.push(EditorCommand::DeleteElementCommand(*c));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  ASSERT_EQ(effective.size(), 3u);
  EXPECT_EQ(effective[0].kind, EditorCommand::Kind::SetTransform);
  EXPECT_EQ(effective[1].kind, EditorCommand::Kind::DeleteElement);
  EXPECT_EQ(effective[2].kind, EditorCommand::Kind::DeleteElement);
}

}  // namespace
}  // namespace donner::editor
