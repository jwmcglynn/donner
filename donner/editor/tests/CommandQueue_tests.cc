#include "donner/editor/CommandQueue.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <ostream>
#include <string>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {

void PrintTo(EditorCommand::Kind kind, std::ostream* os) {
  switch (kind) {
    case EditorCommand::Kind::SetTransform: *os << "SetTransform"; return;
    case EditorCommand::Kind::ReplaceDocument: *os << "ReplaceDocument"; return;
    case EditorCommand::Kind::SetAttribute: *os << "SetAttribute"; return;
    case EditorCommand::Kind::RemoveAttribute: *os << "RemoveAttribute"; return;
    case EditorCommand::Kind::InsertElement: *os << "InsertElement"; return;
    case EditorCommand::Kind::DeleteElement: *os << "DeleteElement"; return;
    case EditorCommand::Kind::CutShapes: *os << "CutShapes"; return;
    case EditorCommand::Kind::PasteShapes: *os << "PasteShapes"; return;
    case EditorCommand::Kind::InsertText: *os << "InsertText"; return;
    case EditorCommand::Kind::SetTextContent: *os << "SetTextContent"; return;
  }

  *os << "EditorCommand::Kind(" << static_cast<int>(kind) << ")";
}

namespace {

using ::testing::AllOf;
using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::IsEmpty;

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

MATCHER_P(OptionalElementIdIs, expectedId, "has element id " + testing::PrintToString(expectedId)) {
  if (!arg.has_value()) {
    *result_listener << "is nullopt";
    return false;
  }

  const std::string actualId(arg->id());
  if (actualId == expectedId) {
    return true;
  }

  *result_listener << "id is " << testing::PrintToString(actualId);
  return false;
}

auto CommandKindIs(EditorCommand::Kind kind) {
  return Field("kind", &EditorCommand::kind, kind);
}

auto CommandElementIdIs(std::string_view id) {
  return Field("element", &EditorCommand::element, OptionalElementIdIs(std::string(id)));
}

auto CommandParentElementIdIs(std::string_view id) {
  return Field("parentElement", &EditorCommand::parentElement,
               OptionalElementIdIs(std::string(id)));
}

MATCHER_P(TranslateXIs, matcher, "has translate x " + testing::DescribeMatcher<double>(matcher)) {
  return testing::ExplainMatchResult(matcher, arg.data[4], result_listener);
}

auto CommandTransformXIs(auto matcher) {
  return Field("transform", &EditorCommand::transform, TranslateXIs(matcher));
}

auto CommandBytesIs(std::string_view bytes) {
  return Field("bytes", &EditorCommand::bytes, std::string(bytes));
}

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
  EXPECT_THAT(effective, IsEmpty());
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
  EXPECT_THAT(effective, ElementsAre(CommandElementIdIs("a"), CommandElementIdIs("b"),
                                     CommandElementIdIs("c")));
  EXPECT_TRUE(queue.empty());
}

TEST_F(CommandQueueTest, MultipleSetTransformsForSameEntityCollapse) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(2.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(3.0, 0.0)));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  // The most recent transform (translation by 3.0) wins.
  EXPECT_THAT(effective,
              ElementsAre(AllOf(CommandElementIdIs("a"), CommandTransformXIs(DoubleEq(3.0)))));
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
  EXPECT_THAT(effective,
              ElementsAre(AllOf(CommandElementIdIs("a"), CommandTransformXIs(DoubleEq(3.0))),
                          AllOf(CommandElementIdIs("b"), CommandTransformXIs(DoubleEq(4.0)))));
}

TEST_F(CommandQueueTest, ReplaceDocumentDropsAllPriorCommands) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::SetTransformCommand(*b, MakeTranslation(2.0, 0.0)));
  queue.push(EditorCommand::ReplaceDocumentCommand("<svg/>"));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  EXPECT_THAT(effective, ElementsAre(AllOf(CommandKindIs(EditorCommand::Kind::ReplaceDocument),
                                           CommandBytesIs("<svg/>"))));
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
  EXPECT_THAT(effective, ElementsAre(CommandKindIs(EditorCommand::Kind::ReplaceDocument),
                                     AllOf(CommandKindIs(EditorCommand::Kind::SetTransform),
                                           CommandElementIdIs("b"))));
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
  EXPECT_THAT(
      effective,
      ElementsAre(
          AllOf(CommandKindIs(EditorCommand::Kind::ReplaceDocument), CommandBytesIs("second")),
          AllOf(CommandKindIs(EditorCommand::Kind::SetTransform), CommandElementIdIs("c"))));
  EXPECT_TRUE(flushResult.hadReplaceDocument);
  EXPECT_FALSE(flushResult.preserveUndoOnReparse);
}

TEST_F(CommandQueueTest, PreserveUndoReparseSurvivesSingleReplaceDocumentBatch) {
  CommandQueue queue;
  queue.push(EditorCommand::ReplaceDocumentCommand("writeback", /*preserveUndoOnReparse=*/true));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  EXPECT_THAT(effective,
              ElementsAre(AllOf(
                  CommandKindIs(EditorCommand::Kind::ReplaceDocument), CommandBytesIs("writeback"),
                  Field("preserveUndoOnReparse", &EditorCommand::preserveUndoOnReparse, true))));
  EXPECT_TRUE(flushResult.hadReplaceDocument);
  EXPECT_TRUE(flushResult.preserveUndoOnReparse);
}

TEST_F(CommandQueueTest, UserReplaceInMixedBatchClearsPreserveUndoMetadata) {
  CommandQueue queue;
  queue.push(EditorCommand::ReplaceDocumentCommand("writeback", /*preserveUndoOnReparse=*/true));
  queue.push(EditorCommand::ReplaceDocumentCommand("user-edit"));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  EXPECT_THAT(effective, ElementsAre(AllOf(CommandKindIs(EditorCommand::Kind::ReplaceDocument),
                                           CommandBytesIs("user-edit"))));
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
  EXPECT_THAT(effective, IsEmpty());
}

TEST_F(CommandQueueTest, DeleteElementCommandNotCoalesced) {
  CommandQueue queue;
  queue.push(EditorCommand::SetTransformCommand(*a, MakeTranslation(1.0, 0.0)));
  queue.push(EditorCommand::DeleteElementCommand(*b));
  queue.push(EditorCommand::DeleteElementCommand(*c));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  EXPECT_THAT(effective, ElementsAre(CommandKindIs(EditorCommand::Kind::SetTransform),
                                     CommandKindIs(EditorCommand::Kind::DeleteElement),
                                     CommandKindIs(EditorCommand::Kind::DeleteElement)));
}

TEST_F(CommandQueueTest, InsertElementCommandNotCoalesced) {
  CommandQueue queue;
  queue.push(EditorCommand::InsertElementCommand(*a, *b));
  queue.push(EditorCommand::InsertElementCommand(*a, *c));

  const auto flushResult = queue.flush();
  const auto& effective = flushResult.effectiveCommands;
  EXPECT_THAT(effective,
              ElementsAre(AllOf(CommandKindIs(EditorCommand::Kind::InsertElement),
                                CommandParentElementIdIs("a"), CommandElementIdIs("b")),
                          AllOf(CommandKindIs(EditorCommand::Kind::InsertElement),
                                CommandParentElementIdIs("a"), CommandElementIdIs("c"))));
}

}  // namespace
}  // namespace donner::editor
