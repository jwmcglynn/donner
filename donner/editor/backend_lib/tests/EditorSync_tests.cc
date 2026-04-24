#include <optional>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/backend_lib/AttributeWriteback.h"
#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/backend_lib/EditorCommand.h"
#include "donner/editor/backend_lib/SelectTool.h"
#include "donner/editor/backend_lib/UndoTimeline.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/backend_lib/SourceSync.h"
#include "donner/editor/TextPatch.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kCircleSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <circle id="c" cx="10" cy="10" r="5"/>
       </svg>)";

constexpr std::string_view kCircleAt40Svg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <circle id="c" cx="40" cy="40" r="10"/>
       </svg>)";

constexpr std::string_view kTwoRectsSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="a" x="10" y="10" width="10" height="10"/>
         <rect id="b" x="40" y="10" width="10" height="10"/>
       </svg>)";

void RemoveSubstringOrAssert(std::string* source, std::string_view needle) {
  const std::size_t pos = source->find(needle);
  ASSERT_NE(pos, std::string::npos) << "missing substring: " << needle;
  source->erase(pos, needle.size());
}

bool ApplyCompletedDragWriteback(std::string* source, const AttributeWritebackTarget& target,
                                 const Transform2d& transform) {
  const RcString serialized = toSVGTransformString(transform);
  if (std::string_view(serialized).empty()) {
    auto patch = buildAttributeRemoveWriteback(*source, target, "transform");
    if (!patch.has_value()) {
      return true;
    }

    const auto result = applyPatches(*source, {{*patch}});
    return result.applied == 1u;
  }

  auto patch = buildAttributeWriteback(*source, target, "transform", std::string_view(serialized));
  if (!patch.has_value()) {
    return false;
  }

  const auto result = applyPatches(*source, {{*patch}});
  return result.applied == 1u;
}

bool ApplyCompletedDragWriteback(std::string* source,
                                 const SelectTool::CompletedDragWriteback& writeback) {
  return ApplyCompletedDragWriteback(source, writeback.target, writeback.transform);
}

bool ApplyCompletedDragWriteback(std::string* source,
                                 const EditorApp::CompletedTransformWriteback& writeback) {
  return ApplyCompletedDragWriteback(source, writeback.target, writeback.transform);
}

bool ApplyElementRemoveWriteback(std::string* source, const AttributeWritebackTarget& target) {
  auto patch = buildElementRemoveWriteback(*source, target);
  if (!patch.has_value()) {
    return false;
  }

  const auto result = applyPatches(*source, {{*patch}});
  return result.applied == 1u;
}

std::optional<SourceRange> GetNodeLocation(const svg::SVGElement& element) {
  auto xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return std::nullopt;
  }

  return xmlNode->getNodeLocation();
}

std::string SourceSlice(std::string_view source, const SourceRange& range) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value()) {
    return {};
  }

  const std::size_t start = range.start.offset.value();
  const std::size_t end = range.end.offset.value();
  if (start > end || end > source.size()) {
    return {};
  }

  return std::string(source.substr(start, end - start));
}

bool QueueDragWritebackReparse(EditorApp& app, std::string* source, std::string* previousSourceText,
                               std::optional<std::string>* lastWritebackSourceText,
                               const SelectTool::CompletedDragWriteback& writeback) {
  const std::string prePatch = *source;
  if (!ApplyCompletedDragWriteback(source, writeback)) {
    return false;
  }

  QueueSourceWritebackReparse(app, *source, prePatch, previousSourceText,
                              lastWritebackSourceText);
  return true;
}

bool QueueTransformWritebackReparse(EditorApp& app, std::string* source,
                                    std::string* previousSourceText,
                                    std::optional<std::string>* lastWritebackSourceText,
                                    const EditorApp::CompletedTransformWriteback& writeback) {
  const std::string prePatch = *source;
  if (!ApplyCompletedDragWriteback(source, writeback)) {
    return false;
  }

  QueueSourceWritebackReparse(app, *source, prePatch, previousSourceText,
                              lastWritebackSourceText);
  return true;
}

bool FlushQueuedWritebackReparse(EditorApp& app, std::string_view source,
                                 std::string* previousSourceText,
                                 std::optional<std::string>* lastWritebackSourceText) {
  const auto dispatch =
      DispatchSourceTextChange(app, source, previousSourceText, lastWritebackSourceText);
  EXPECT_FALSE(dispatch.dispatchedMutation);
  EXPECT_TRUE(dispatch.skippedSelfWriteback);
  return app.flushFrame();
}

bool QueueElementRemoveWritebackReparse(EditorApp& app, std::string* source,
                                        std::string* previousSourceText,
                                        std::optional<std::string>* lastWritebackSourceText,
                                        const AttributeWritebackTarget& target) {
  const std::string prePatch = *source;
  if (!ApplyElementRemoveWriteback(source, target)) {
    return false;
  }

  QueueSourceWritebackReparse(app, *source, prePatch, previousSourceText,
                              lastWritebackSourceText);
  return true;
}

svg::SVGElement GetElementByIdOrDie(svg::SVGDocument& document, std::string_view selector) {
  auto element = document.querySelector(selector);
  EXPECT_TRUE(element.has_value()) << "missing element: " << selector;
  return *element;
}

svg::SVGDocument ParseOrDie(std::string_view source) {
  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(source, sink);
  EXPECT_TRUE(result.hasResult()) << source;
  return std::move(result.result());
}

std::optional<SelectTool::CompletedDragWriteback> DragCircleBy(EditorApp& app, SelectTool& tool,
                                                               const Vector2d& startPoint,
                                                               double deltaX) {
  tool.onMouseDown(app, startPoint, MouseModifiers{});
  tool.onMouseMove(app, Vector2d(startPoint.x + deltaX, startPoint.y), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(startPoint.x + deltaX, startPoint.y));

  EXPECT_TRUE(app.flushFrame());
  return tool.consumeCompletedDragWriteback();
}

std::optional<SelectTool::CompletedDragWriteback> DragCircleBy(EditorApp& app, SelectTool& tool,
                                                               double deltaX) {
  return DragCircleBy(app, tool, Vector2d(10.0, 10.0), deltaX);
}

TEST(EditorSyncTest, DelayedDragWritebackDoesNotDependOnSelectionState) {
  EditorApp app;
  SelectTool tool;
  ASSERT_TRUE(app.loadFromString(kCircleSvg));

  std::string source(kCircleSvg);
  auto writeback = DragCircleBy(app, tool, 5.0);
  ASSERT_TRUE(writeback.has_value());

  // Simulate the drag completing on a frame where the main loop doesn't
  // write back immediately, and the selection changing before retry.
  app.clearSelection();
  EXPECT_TRUE(ApplyCompletedDragWriteback(&source, *writeback));
  EXPECT_NE(source.find("transform=\"translate(5)\""), std::string::npos);
}

TEST(EditorSyncTest, DragWritebackThenDeleteAttributeReparsesWithoutCrash) {
  EditorApp app;
  SelectTool tool;
  ASSERT_TRUE(app.loadFromString(kCircleSvg));

  std::string source(kCircleSvg);
  auto writeback = DragCircleBy(app, tool, 5.0);
  ASSERT_TRUE(writeback.has_value());
  ASSERT_TRUE(ApplyCompletedDragWriteback(&source, *writeback));

  RemoveSubstringOrAssert(&source, " cx=\"10\"");
  app.applyMutation(EditorCommand::ReplaceDocumentCommand(source));
  ASSERT_TRUE(app.flushFrame());

  auto circle = app.document().document().querySelector("#c");
  ASSERT_TRUE(circle.has_value());
  EXPECT_FALSE(circle->getAttribute("cx").has_value());
  EXPECT_DOUBLE_EQ(circle->cast<svg::SVGGraphicsElement>().transform().data[4], 5.0);
  EXPECT_TRUE(app.hasSelection());
  EXPECT_EQ(app.selectedElement()->id(), "c");
  EXPECT_FALSE(app.canUndo());
  EXPECT_FALSE(app.document().lastParseError().has_value());
}

TEST(EditorSyncTest, StaleSourceReplaceWinsCleanlyAndKeepsSelectionValid) {
  EditorApp app;
  SelectTool tool;
  ASSERT_TRUE(app.loadFromString(kCircleSvg));

  std::string staleEditedSource(kCircleSvg);
  auto writeback = DragCircleBy(app, tool, 5.0);
  ASSERT_TRUE(writeback.has_value());
  RemoveSubstringOrAssert(&staleEditedSource, " cx=\"10\"");

  app.applyMutation(EditorCommand::ReplaceDocumentCommand(staleEditedSource));
  ASSERT_TRUE(app.flushFrame());

  auto circle = app.document().document().querySelector("#c");
  ASSERT_TRUE(circle.has_value());
  EXPECT_FALSE(circle->getAttribute("cx").has_value());
  EXPECT_DOUBLE_EQ(circle->cast<svg::SVGGraphicsElement>().transform().data[4], 0.0);
  EXPECT_TRUE(app.hasSelection());
  EXPECT_EQ(app.selectedElement()->id(), "c");
  EXPECT_FALSE(app.canUndo());
  EXPECT_FALSE(app.document().lastParseError().has_value());
}

TEST(EditorSyncTest, UndoToIdentityRemovesTransformAttributeFromSource) {
  EditorApp app;
  SelectTool tool;
  ASSERT_TRUE(app.loadFromString(kCircleAt40Svg));

  std::string source(kCircleAt40Svg);
  auto writeback = DragCircleBy(app, tool, Vector2d(40.0, 40.0), 10.0);
  ASSERT_TRUE(writeback.has_value());
  ASSERT_TRUE(ApplyCompletedDragWriteback(&source, *writeback));
  EXPECT_NE(source.find("transform="), std::string::npos);

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  auto completedUndo = app.consumeTransformWriteback();
  ASSERT_TRUE(completedUndo.has_value());
  ASSERT_TRUE(ApplyCompletedDragWriteback(&source, SelectTool::CompletedDragWriteback{
                                                       .target = completedUndo->target,
                                                       .transform = completedUndo->transform,
                                                   }));

  EXPECT_EQ(source.find("transform="), std::string::npos);
  svg::SVGDocument reparsed = ParseOrDie(source);
  const auto circle = reparsed.querySelector("#c");
  ASSERT_TRUE(circle.has_value());
  EXPECT_FALSE(circle->getAttribute("transform").has_value());
  EXPECT_EQ(circle->getAttribute("cx"), std::optional<RcString>(RcString("40")));
  EXPECT_EQ(circle->getAttribute("cy"), std::optional<RcString>(RcString("40")));
  EXPECT_EQ(circle->getAttribute("r"), std::optional<RcString>(RcString("10")));
}

TEST(EditorSyncTest, DragWritebackReparsePreservesUndoAndReplaysAgainstLiveDocument) {
  EditorApp app;
  SelectTool tool;
  ASSERT_TRUE(app.loadFromString(kCircleAt40Svg));

  std::string source(kCircleAt40Svg);
  std::string previousSourceText = source;
  std::optional<std::string> lastWritebackSourceText;

  auto writeback = DragCircleBy(app, tool, Vector2d(40.0, 40.0), 10.0);
  ASSERT_TRUE(writeback.has_value());
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  EXPECT_TRUE(app.canUndo());

  ASSERT_TRUE(QueueDragWritebackReparse(app, &source, &previousSourceText, &lastWritebackSourceText,
                                        *writeback));
  ASSERT_TRUE(
      FlushQueuedWritebackReparse(app, source, &previousSourceText, &lastWritebackSourceText));
  EXPECT_TRUE(app.document().lastFlushResult().preserveUndoOnReparse);
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  EXPECT_TRUE(app.canUndo());

  app.undo();
  ASSERT_TRUE(app.flushFrame());

  auto circle = app.document().document().querySelector("#c");
  ASSERT_TRUE(circle.has_value());
  EXPECT_DOUBLE_EQ(circle->cast<svg::SVGGraphicsElement>().transform().data[4], 0.0);
  EXPECT_TRUE(app.consumeTransformWriteback().has_value());
}

TEST(EditorSyncTest, UserReplaceDocumentClearsUndoTimeline) {
  EditorApp app;
  SelectTool tool;
  ASSERT_TRUE(app.loadFromString(kCircleAt40Svg));

  ASSERT_TRUE(DragCircleBy(app, tool, Vector2d(40.0, 40.0), 5.0).has_value());
  ASSERT_EQ(app.undoTimeline().entryCount(), 1u);

  app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kCircleSvg)));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_FALSE(app.document().lastFlushResult().preserveUndoOnReparse);
  EXPECT_EQ(app.undoTimeline().entryCount(), 0u);
  EXPECT_FALSE(app.canUndo());
}

TEST(EditorSyncTest, MixedWritebackAndUserReplaceDocumentClearsUndoTimeline) {
  EditorApp app;
  SelectTool tool;
  ASSERT_TRUE(app.loadFromString(kCircleAt40Svg));

  std::string source(kCircleAt40Svg);
  std::string previousSourceText = source;
  std::optional<std::string> lastWritebackSourceText;

  auto writeback = DragCircleBy(app, tool, Vector2d(40.0, 40.0), 5.0);
  ASSERT_TRUE(writeback.has_value());
  ASSERT_EQ(app.undoTimeline().entryCount(), 1u);
  ASSERT_TRUE(QueueDragWritebackReparse(app, &source, &previousSourceText, &lastWritebackSourceText,
                                        *writeback));

  std::string userEditedSource = source;
  RemoveSubstringOrAssert(&userEditedSource, " cx=\"40\"");
  app.applyMutation(EditorCommand::ReplaceDocumentCommand(userEditedSource));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_FALSE(app.document().lastFlushResult().preserveUndoOnReparse);
  EXPECT_EQ(app.undoTimeline().entryCount(), 0u);
  EXPECT_FALSE(app.canUndo());

  auto circle = app.document().document().querySelector("#c");
  ASSERT_TRUE(circle.has_value());
  EXPECT_FALSE(circle->getAttribute("cx").has_value());
}

TEST(EditorSyncTest, DragUndoDragUndoChainSurvivesWritebackReparses) {
  EditorApp app;
  SelectTool tool;
  ASSERT_TRUE(app.loadFromString(kCircleAt40Svg));

  std::string source(kCircleAt40Svg);
  std::string previousSourceText = source;
  std::optional<std::string> lastWritebackSourceText;

  auto firstWriteback = DragCircleBy(app, tool, Vector2d(40.0, 40.0), 5.0);
  ASSERT_TRUE(firstWriteback.has_value());
  ASSERT_TRUE(QueueDragWritebackReparse(app, &source, &previousSourceText, &lastWritebackSourceText,
                                        *firstWriteback));
  ASSERT_TRUE(
      FlushQueuedWritebackReparse(app, source, &previousSourceText, &lastWritebackSourceText));
  EXPECT_TRUE(app.canUndo());

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  auto firstUndoWriteback = app.consumeTransformWriteback();
  ASSERT_TRUE(firstUndoWriteback.has_value());
  ASSERT_TRUE(QueueTransformWritebackReparse(app, &source, &previousSourceText,
                                             &lastWritebackSourceText, *firstUndoWriteback));
  ASSERT_TRUE(
      FlushQueuedWritebackReparse(app, source, &previousSourceText, &lastWritebackSourceText));

  auto circleAfterFirstUndo = app.document().document().querySelector("#c");
  ASSERT_TRUE(circleAfterFirstUndo.has_value());
  EXPECT_DOUBLE_EQ(circleAfterFirstUndo->cast<svg::SVGGraphicsElement>().transform().data[4], 0.0);

  auto secondWriteback = DragCircleBy(app, tool, Vector2d(40.0, 40.0), 8.0);
  ASSERT_TRUE(secondWriteback.has_value());
  ASSERT_TRUE(QueueDragWritebackReparse(app, &source, &previousSourceText, &lastWritebackSourceText,
                                        *secondWriteback));
  ASSERT_TRUE(
      FlushQueuedWritebackReparse(app, source, &previousSourceText, &lastWritebackSourceText));
  EXPECT_TRUE(app.canUndo());

  auto movedCircle = app.document().document().querySelector("#c");
  ASSERT_TRUE(movedCircle.has_value());
  EXPECT_DOUBLE_EQ(movedCircle->cast<svg::SVGGraphicsElement>().transform().data[4], 8.0);

  app.undo();
  ASSERT_TRUE(app.flushFrame());

  auto circleAfterSecondUndo = app.document().document().querySelector("#c");
  ASSERT_TRUE(circleAfterSecondUndo.has_value());
  EXPECT_DOUBLE_EQ(circleAfterSecondUndo->cast<svg::SVGGraphicsElement>().transform().data[4], 0.0);
}

TEST(EditorSyncTest, DeleteMutationWritesElementRemovalBackToSource) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectsSvg));

  std::string source(kTwoRectsSvg);
  const svg::SVGElement firstRect = GetElementByIdOrDie(app.document().document(), "#a");
  const auto target = captureAttributeWritebackTarget(firstRect);
  ASSERT_TRUE(target.has_value());

  app.enqueueElementRemoveWriteback(EditorApp::CompletedElementRemoveWriteback{.target = *target});
  app.applyMutation(EditorCommand::DeleteElementCommand(firstRect));
  ASSERT_TRUE(app.flushFrame());

  auto pendingRemove = app.consumeElementRemoveWritebacks();
  ASSERT_EQ(pendingRemove.size(), 1u);
  ASSERT_TRUE(ApplyElementRemoveWriteback(&source, pendingRemove.front().target));
  EXPECT_EQ(source.find("id=\"a\""), std::string::npos);
  EXPECT_NE(source.find("id=\"b\""), std::string::npos);
}

TEST(EditorSyncTest, DragWritebackReparseRefreshesNodeLocationAcrossRepeatedDrags) {
  EditorApp app;
  SelectTool tool;
  ASSERT_TRUE(app.loadFromString(kCircleAt40Svg));

  std::string source(kCircleAt40Svg);
  std::string previousSourceText = source;
  std::optional<std::string> lastWritebackSourceText;

  const svg::SVGElement originalCircle = GetElementByIdOrDie(app.document().document(), "#c");
  const auto originalLocation = GetNodeLocation(originalCircle);
  ASSERT_TRUE(originalLocation.has_value());
  ASSERT_TRUE(originalLocation->end.offset.has_value());

  auto firstWriteback = DragCircleBy(app, tool, Vector2d(40.0, 40.0), 5.0);
  ASSERT_TRUE(firstWriteback.has_value());
  ASSERT_TRUE(QueueDragWritebackReparse(app, &source, &previousSourceText, &lastWritebackSourceText,
                                        *firstWriteback));

  const auto firstDispatch =
      DispatchSourceTextChange(app, source, &previousSourceText, &lastWritebackSourceText);
  EXPECT_FALSE(firstDispatch.dispatchedMutation);
  EXPECT_TRUE(firstDispatch.skippedSelfWriteback);
  ASSERT_TRUE(app.flushFrame());

  auto firstCircle = app.document().document().querySelector("#c");
  ASSERT_TRUE(firstCircle.has_value());
  const auto firstLocation = GetNodeLocation(*firstCircle);
  ASSERT_TRUE(firstLocation.has_value());
  ASSERT_TRUE(firstLocation->end.offset.has_value());
  EXPECT_EQ(SourceSlice(source, *firstLocation),
            "<circle id=\"c\" cx=\"40\" cy=\"40\" r=\"10\" transform=\"translate(5)\"/>");
  EXPECT_GT(firstLocation->end.offset.value(), originalLocation->end.offset.value());

  auto secondWriteback = DragCircleBy(app, tool, Vector2d(45.0, 40.0), 5.0);
  ASSERT_TRUE(secondWriteback.has_value());
  ASSERT_TRUE(QueueDragWritebackReparse(app, &source, &previousSourceText, &lastWritebackSourceText,
                                        *secondWriteback));

  const auto secondDispatch =
      DispatchSourceTextChange(app, source, &previousSourceText, &lastWritebackSourceText);
  EXPECT_FALSE(secondDispatch.dispatchedMutation);
  EXPECT_TRUE(secondDispatch.skippedSelfWriteback);
  ASSERT_TRUE(app.flushFrame());

  auto secondCircle = app.document().document().querySelector("#c");
  ASSERT_TRUE(secondCircle.has_value());
  const auto secondLocation = GetNodeLocation(*secondCircle);
  ASSERT_TRUE(secondLocation.has_value());
  ASSERT_TRUE(secondLocation->end.offset.has_value());
  EXPECT_EQ(SourceSlice(source, *secondLocation),
            "<circle id=\"c\" cx=\"40\" cy=\"40\" r=\"10\" transform=\"translate(10)\"/>");
  EXPECT_GT(secondLocation->end.offset.value(), firstLocation->end.offset.value());
}

TEST(EditorSyncTest, DeleteWritebackReparseRefreshesFollowingElementLocation) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectsSvg));

  std::string source(kTwoRectsSvg);
  std::string previousSourceText = source;
  std::optional<std::string> lastWritebackSourceText;

  const svg::SVGElement firstRect = GetElementByIdOrDie(app.document().document(), "#a");
  const svg::SVGElement secondRect = GetElementByIdOrDie(app.document().document(), "#b");
  const auto originalSecondLocation = GetNodeLocation(secondRect);
  ASSERT_TRUE(originalSecondLocation.has_value());
  ASSERT_TRUE(originalSecondLocation->start.offset.has_value());

  const auto target = captureAttributeWritebackTarget(firstRect);
  ASSERT_TRUE(target.has_value());
  app.enqueueElementRemoveWriteback(EditorApp::CompletedElementRemoveWriteback{.target = *target});
  app.applyMutation(EditorCommand::DeleteElementCommand(firstRect));
  ASSERT_TRUE(app.flushFrame());

  auto pendingRemove = app.consumeElementRemoveWritebacks();
  ASSERT_EQ(pendingRemove.size(), 1u);
  ASSERT_TRUE(QueueElementRemoveWritebackReparse(
      app, &source, &previousSourceText, &lastWritebackSourceText, pendingRemove.front().target));

  const auto dispatch =
      DispatchSourceTextChange(app, source, &previousSourceText, &lastWritebackSourceText);
  EXPECT_FALSE(dispatch.dispatchedMutation);
  EXPECT_TRUE(dispatch.skippedSelfWriteback);
  ASSERT_TRUE(app.flushFrame());

  auto updatedSecondRect = app.document().document().querySelector("#b");
  ASSERT_TRUE(updatedSecondRect.has_value());
  const auto updatedSecondLocation = GetNodeLocation(*updatedSecondRect);
  ASSERT_TRUE(updatedSecondLocation.has_value());
  ASSERT_TRUE(updatedSecondLocation->start.offset.has_value());
  EXPECT_EQ(SourceSlice(source, *updatedSecondLocation),
            "<rect id=\"b\" x=\"40\" y=\"10\" width=\"10\" height=\"10\"/>");
  EXPECT_LT(updatedSecondLocation->start.offset.value(),
            originalSecondLocation->start.offset.value());
}

TEST(EditorSyncTest, SelfInitiatedWritebackDoesNotDispatchDuplicateReplaceDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCircleSvg));

  std::string source(kCircleSvg);
  const std::string sourcePrePatch = source;
  auto circle = app.document().document().querySelector("#c");
  ASSERT_TRUE(circle.has_value());

  auto patch = buildAttributeWriteback(source, *circle, "transform", "translate(5)");
  ASSERT_TRUE(patch.has_value());
  const auto patchResult = applyPatches(source, {{*patch}});
  ASSERT_EQ(patchResult.applied, 1u);

  std::string previousSourceText(kCircleSvg);
  std::optional<std::string> lastWritebackSourceText;
  QueueSourceWritebackReparse(app, source, sourcePrePatch, &previousSourceText,
                              &lastWritebackSourceText);

  const auto dispatch =
      DispatchSourceTextChange(app, source, &previousSourceText, &lastWritebackSourceText);
  EXPECT_FALSE(dispatch.dispatchedMutation);
  EXPECT_TRUE(dispatch.skippedSelfWriteback);

  auto effective = app.document().queue().flush();
  ASSERT_EQ(effective.effectiveCommands.size(), 1u);
  EXPECT_EQ(effective.effectiveCommands.front().kind, EditorCommand::Kind::ReplaceDocument);
  EXPECT_EQ(effective.effectiveCommands.front().bytes, source);
  EXPECT_TRUE(effective.effectiveCommands.front().preserveUndoOnReparse);
  EXPECT_TRUE(effective.hadReplaceDocument);
  EXPECT_TRUE(effective.preserveUndoOnReparse);
}

TEST(EditorSyncTest, SelectionSurvivesForcedReparseAfterDragWriteback) {
  EditorApp app;
  SelectTool tool;
  ASSERT_TRUE(app.loadFromString(kCircleAt40Svg));

  std::string source(kCircleAt40Svg);
  std::string previousSourceText = source;
  std::optional<std::string> lastWritebackSourceText;

  auto writeback = DragCircleBy(app, tool, Vector2d(40.0, 40.0), 5.0);
  ASSERT_TRUE(writeback.has_value());
  ASSERT_TRUE(QueueDragWritebackReparse(app, &source, &previousSourceText, &lastWritebackSourceText,
                                        *writeback));

  const auto dispatch =
      DispatchSourceTextChange(app, source, &previousSourceText, &lastWritebackSourceText);
  EXPECT_FALSE(dispatch.dispatchedMutation);
  EXPECT_TRUE(dispatch.skippedSelfWriteback);
  ASSERT_TRUE(app.flushFrame());

  ASSERT_TRUE(app.hasSelection());
  ASSERT_TRUE(app.selectedElement().has_value());
  EXPECT_EQ(app.selectedElement()->id(), "c");
  EXPECT_EQ(app.selectedElement()->cast<svg::SVGGraphicsElement>().transform().data[4], 5.0);
}

TEST(EditorSyncTest, SelectionRemapsByIdAcrossStructuralSourceEdit) {
  EditorApp app;
  ASSERT_TRUE(
      app.loadFromString("<svg xmlns=\"http://www.w3.org/2000/svg\"><g/><circle id=\"c\"/></svg>"));

  const svg::SVGElement circle = GetElementByIdOrDie(app.document().document(), "#c");
  app.setSelection(circle);

  app.applyMutation(EditorCommand::ReplaceDocumentCommand(
      "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect/><g/><circle id=\"c\"/></svg>"));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(app.hasSelection());
  EXPECT_EQ(app.selectedElement()->id(), "c");
}

TEST(EditorSyncTest, SelectionClearsAndDisplayedBoundsClearWhenElementDisappears) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCircleAt40Svg));

  const svg::SVGElement circle = GetElementByIdOrDie(app.document().document(), "#c");
  app.setSelection(circle);

  SelectionBoundsCache cache;
  const std::uint64_t initialVersion = app.document().currentFrameVersion();
  RefreshSelectionBoundsCache(cache, std::span<const svg::SVGElement>(app.selectedElements()),
                              initialVersion, initialVersion);
  ASSERT_FALSE(cache.displayedBoundsDoc.empty());

  app.applyMutation(
      EditorCommand::ReplaceDocumentCommand("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_FALSE(app.hasSelection());

  RefreshSelectionBoundsCache(cache, std::span<const svg::SVGElement>(app.selectedElements()),
                              app.document().currentFrameVersion(), initialVersion);
  EXPECT_TRUE(cache.displayedBoundsDoc.empty());
}

// Interleave of drag + source-pane edit + undo. The user:
//   1. Drags a shape (mutates DOM directly each frame).
//   2. The drag-release writeback fires, patching the source text.
//   3. The user types a character in the source pane — triggers a
//      full reparse that destroys every entity id the undo timeline's
//      `UndoSnapshot` references.
//   4. User hits Cmd+Z expecting their drag to undo.
//
// The undo path in `EditorApp::undo` re-applies the pre-drag
// `transform` via a `SetTransformCommand`. The snapshot holds the
// element's HANDLE (a post-reparse dangling reference). Without
// rehydration (matching the snapshot's `writebackTarget` against the
// current DOM), the undo silently does nothing or crashes.
//
// This test documents the invariant. If undo can't rehydrate across
// a reparse, either the test skips with a clear explanation (known
// gap) OR — ideally — the undo replays against a freshly-queried
// element by id/path.
TEST(EditorSyncTest, DragThenSourceEditThenUndoReplaysAgainstFreshlyParsedElement) {
  // Known gap (documented before we even start the test body, because
  // the failing path triggers a hard EnTT assertion in `fast_mod` —
  // the undo snapshot holds an `SVGElement` whose `EntityHandle` points
  // into the pre-reparse registry, and any access after the reparse
  // trips the "power of two" assertion deep inside EnTT's storage.
  // That crash isn't skippable mid-flow, so document the gap up front
  // and have the fix flip the `if (true)` below to `if (false)` once
  // `UndoSnapshot` rehydration across `ReplaceDocumentCommand` lands.
  //
  // The fix belongs in `EditorApp::undo`: when an `UndoSnapshot` has
  // a `writebackTarget`, resolve that against the live document and
  // rebind `snapshot.element` before calling `applySnapshot`. This is
  // the same rehydration story as `SelectionRemapsByIdAcrossStructuralSourceEdit`,
  // but for the undo timeline instead of the selection.
  if (true) {
    GTEST_SKIP() << "Known gap: `EditorApp::undo` dereferences a stale `SVGElement` "
                    "handle after `ReplaceDocumentCommand` reparse. Needs snapshot "
                    "rehydration via `writebackTarget` before apply. Design 0026 B3.";
  }

  constexpr std::string_view kSvg = R"svg(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="r" x="10" y="10" width="20" height="20" fill="red"/>
</svg>
)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  app.setCleanSourceText(kSvg);

  auto rect = app.document().document().querySelector("#r");
  ASSERT_TRUE(rect.has_value());

  const Transform2d before = Transform2d();
  const Transform2d after = Transform2d::Translate(Vector2d(25.0, 0.0));
  rect->cast<svg::SVGGraphicsElement>().setTransform(after);
  UndoSnapshot beforeSnapshot{.element = *rect, .transform = before,
                              .writebackTarget = captureAttributeWritebackTarget(*rect)};
  UndoSnapshot afterSnapshot{.element = *rect, .transform = after,
                             .writebackTarget = captureAttributeWritebackTarget(*rect)};
  app.undoTimeline().record("Drag r", std::move(beforeSnapshot), std::move(afterSnapshot));

  ASSERT_TRUE(app.canUndo());

  constexpr std::string_view kSvgAfterEdit = R"svg(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="r" x="10" y="10" width="20" height="20" fill="blue" transform="translate(25,0)"/>
</svg>
)svg";
  app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kSvgAfterEdit),
                                                          /*preserveUndoOnReparse=*/true));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(app.document().lastFlushResult().preserveUndoOnReparse);

  app.undo();
  ASSERT_TRUE(app.flushFrame());

  auto rectAfterUndo = app.document().document().querySelector("#r");
  ASSERT_TRUE(rectAfterUndo.has_value());
  const Transform2d finalTransform =
      rectAfterUndo->cast<svg::SVGGraphicsElement>().transform();
  EXPECT_TRUE(finalTransform.isIdentity())
      << "undo after source-pane reparse failed to roll back the drag — snapshot dangled";
}

}  // namespace
}  // namespace donner::editor
