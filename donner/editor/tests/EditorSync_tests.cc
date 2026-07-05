#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/SourceEditIntent.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/TextPatch.h"
#include "donner/editor/UndoTimeline.h"
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

constexpr std::string_view kAdjacentUnidentifiedLettersSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\">"
    "<g id=\"letters\"><polygon points=\"10 10 30 10 30 30 10 30\"/>"
    "<polygon points=\"50 10 70 10 70 30 50 30\"/></g></svg>";

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
  if (!ApplyCompletedDragWriteback(source, writeback)) {
    return false;
  }

  QueueSourceWritebackReparse(app, *source, previousSourceText, lastWritebackSourceText);
  return true;
}

bool QueueTransformWritebackReparse(EditorApp& app, std::string* source,
                                    std::string* previousSourceText,
                                    std::optional<std::string>* lastWritebackSourceText,
                                    const EditorApp::CompletedTransformWriteback& writeback) {
  if (!ApplyCompletedDragWriteback(source, writeback)) {
    return false;
  }

  QueueSourceWritebackReparse(app, *source, previousSourceText, lastWritebackSourceText);
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
  if (!ApplyElementRemoveWriteback(source, target)) {
    return false;
  }

  QueueSourceWritebackReparse(app, *source, previousSourceText, lastWritebackSourceText);
  return true;
}

svg::SVGElement GetElementByIdOrDie(svg::SVGDocument& document, std::string_view selector) {
  auto element = document.querySelector(selector);
  EXPECT_TRUE(element.has_value()) << "missing element: " << selector;
  return *element;
}

std::optional<svg::SVGElement> ElementChildAt(svg::SVGElement parent, std::size_t targetIndex) {
  std::size_t currentIndex = 0;
  for (auto child = parent.firstChild(); child.has_value(); child = child->nextSibling()) {
    if (currentIndex == targetIndex) {
      return child;
    }
    ++currentIndex;
  }

  return std::nullopt;
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

TEST(EditorSyncTest, StructuredSelfWritebackDoesNotRetargetRepeatedUnidentifiedSiblingDrag) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kAdjacentUnidentifiedLettersSvg));

  std::string source(kAdjacentUnidentifiedLettersSvg);
  std::string previousSourceText = source;
  std::optional<std::string> lastWritebackSourceText;

  const svg::SVGElement letters = GetElementByIdOrDie(app.document().document(), "#letters");
  auto firstLetter = ElementChildAt(letters, 0);
  auto secondLetter = ElementChildAt(letters, 1);
  ASSERT_TRUE(firstLetter.has_value());
  ASSERT_TRUE(secondLetter.has_value());

  const auto firstTarget = captureAttributeWritebackTarget(*firstLetter);
  const auto secondTarget = captureAttributeWritebackTarget(*secondLetter);
  ASSERT_TRUE(firstTarget.has_value());
  ASSERT_TRUE(secondTarget.has_value());
  ASSERT_FALSE(firstTarget->elementId.has_value());
  ASSERT_FALSE(secondTarget->elementId.has_value());

  auto writeFirstLetterTransform = [&](double translateX) {
    auto liveFirstLetter = resolveAttributeWritebackTarget(app.document().document(), *firstTarget);
    ASSERT_TRUE(liveFirstLetter.has_value());

    const Transform2d letterFromParent = Transform2d::Translate(translateX, 0.0);
    app.applyMutation(EditorCommand::SetTransformCommand(*liveFirstLetter, letterFromParent));
    ASSERT_TRUE(app.flushFrame());

    ASSERT_TRUE(QueueTransformWritebackReparse(app, &source, &previousSourceText,
                                               &lastWritebackSourceText,
                                               EditorApp::CompletedTransformWriteback{
                                                   .target = *firstTarget,
                                                   .transform = letterFromParent,
                                               }));
    ASSERT_TRUE(
        FlushQueuedWritebackReparse(app, source, &previousSourceText, &lastWritebackSourceText));
    EXPECT_TRUE(app.document().lastFlushResult().preserveUndoOnReparse);
  };

  writeFirstLetterTransform(5.0);
  writeFirstLetterTransform(10.0);

  auto finalFirstLetter = resolveAttributeWritebackTarget(app.document().document(), *firstTarget);
  auto finalSecondLetter =
      resolveAttributeWritebackTarget(app.document().document(), *secondTarget);
  ASSERT_TRUE(finalFirstLetter.has_value());
  ASSERT_TRUE(finalSecondLetter.has_value());

  const Transform2d firstLetterFromParent =
      finalFirstLetter->cast<svg::SVGGraphicsElement>().transform();
  const Transform2d secondLetterFromParent =
      finalSecondLetter->cast<svg::SVGGraphicsElement>().transform();
  EXPECT_DOUBLE_EQ(firstLetterFromParent.data[4], 10.0);
  EXPECT_DOUBLE_EQ(firstLetterFromParent.data[5], 0.0);
  EXPECT_TRUE(secondLetterFromParent.isIdentity());
  EXPECT_FALSE(finalSecondLetter->getAttribute("transform").has_value());
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

TEST(EditorSyncTest, StructuredSourceEditAppliesThroughXMLDocumentWithoutCommand) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red"/></svg>)";
  constexpr std::string_view kEditedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="blue"/></svg>)";

  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kSvg));

  std::string previousSourceText(kSvg);
  std::optional<std::string> lastWritebackSourceText;
  const std::uint64_t previousFrameVersion = app.document().currentFrameVersion();

  const auto dispatch =
      DispatchSourceTextChange(app, kEditedSvg, &previousSourceText, &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());
  EXPECT_GT(app.document().currentFrameVersion(), previousFrameVersion);
  EXPECT_EQ(app.document().document().source(), kEditedSvg);
  EXPECT_EQ(previousSourceText, kEditedSvg);

  auto rect = app.document().document().querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  std::optional<RcString> fill = rect->getAttribute("fill");
  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(*fill, RcString("blue"));
}

TEST(EditorSyncTest, StructuredSourceEditInsertsChildElementIncrementally) {
  // Inserting a brand-new child element by typing in the source pane is a
  // *structural* edit. Per #634 ("DOM-aware typing"), it must apply through the
  // incremental XMLDocument path - NOT a full-document ReplaceDocument - and must
  // preserve the entity identity of the untouched sibling, so selection,
  // compositor caches, and references survive the keystroke.
  EditorApp app;
  ASSERT_TRUE(app.structuredEditingEnabled());
  ASSERT_TRUE(app.loadFromString(kTwoRectsSvg));

  const svg::SVGElement firstRect = GetElementByIdOrDie(app.document().document(), "#a");
  const auto firstRectEntity = firstRect.entityHandle().entity();
  const auto secondRectEntity =
      GetElementByIdOrDie(app.document().document(), "#b").entityHandle().entity();

  std::string previousSourceText(kTwoRectsSvg);
  std::optional<std::string> lastWritebackSourceText;
  std::string editedSource(kTwoRectsSvg);
  const std::size_t pos = editedSource.find(R"(<rect id="b")");
  ASSERT_NE(pos, std::string::npos);
  constexpr std::string_view kInserted = R"(<rect id="c" x="70" y="10" width="10" height="10"/>
         )";
  editedSource.insert(pos, kInserted);

  // Drive the precise-offset intent path the live editor uses on a cursor insert.
  std::vector<SourceEditIntent> intents;
  intents.push_back(
      SourceEditIntent{.offset = pos, .removedLength = 0, .replacement = std::string(kInserted)});
  const auto dispatch = DispatchSourceEditIntents(app, intents, editedSource, &previousSourceText,
                                                  &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  // Incremental: a fallback would queue a ReplaceDocumentCommand, which would make
  // flushFrame() return true. An incremental apply queues nothing.
  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());

  EXPECT_EQ(app.document().document().source(), editedSource);
  EXPECT_TRUE(app.document().document().querySelector("#c").has_value());
  EXPECT_TRUE(app.document().document().querySelector("#b").has_value());

  // Untouched siblings kept their entities (incremental child-reuse, not regen).
  EXPECT_EQ(GetElementByIdOrDie(app.document().document(), "#a").entityHandle().entity(),
            firstRectEntity);
  EXPECT_EQ(GetElementByIdOrDie(app.document().document(), "#b").entityHandle().entity(),
            secondRectEntity);
}

TEST(EditorSyncTest, StructuredSourceEditDeletesChildElementIncrementally) {
  // Deleting a whole element by selecting its source span and pressing delete is a
  // structural edit; same incremental + identity-preserving contract as the insert.
  EditorApp app;
  ASSERT_TRUE(app.structuredEditingEnabled());
  ASSERT_TRUE(app.loadFromString(kTwoRectsSvg));

  const svg::SVGElement secondRect = GetElementByIdOrDie(app.document().document(), "#b");
  const auto secondRectEntity = secondRect.entityHandle().entity();

  std::string previousSourceText(kTwoRectsSvg);
  std::optional<std::string> lastWritebackSourceText;
  std::string editedSource(kTwoRectsSvg);
  constexpr std::string_view kRectA = R"(<rect id="a" x="10" y="10" width="10" height="10"/>)";
  const std::size_t pos = editedSource.find(kRectA);
  ASSERT_NE(pos, std::string::npos);
  editedSource.erase(pos, kRectA.size());

  // Drive the precise-offset intent path the live editor uses on a cursor delete.
  std::vector<SourceEditIntent> intents;
  intents.push_back(SourceEditIntent{
      .offset = pos, .removedLength = kRectA.size(), .replacement = std::string()});
  const auto dispatch = DispatchSourceEditIntents(app, intents, editedSource, &previousSourceText,
                                                  &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());

  EXPECT_EQ(app.document().document().source(), editedSource);
  EXPECT_FALSE(app.document().document().querySelector("#a").has_value());

  // The surviving sibling kept its entity.
  const svg::SVGElement reloadedSecondRect = GetElementByIdOrDie(app.document().document(), "#b");
  EXPECT_EQ(reloadedSecondRect.entityHandle().entity(), secondRectEntity);
}

TEST(EditorSyncTest, WholeTextDiffFallbackDoesNotDesyncDomFromSourceOnSimilarSiblingInsert) {
  // When a source change arrives WITHOUT precise SourceEditIntents (e.g. a
  // programmatic setText), DispatchSourceTextChange diffs the whole buffer with
  // BuildSingleSourceTextEdit. Inserting an element textually similar to an
  // adjacent sibling collapses under minimal prefix/suffix diffing into what
  // looks like a single-character `id="b"`->`id="c"` attribute edit. The
  // structured apply must NOT silently rename the existing sibling and drop the
  // new element: the DOM must stay consistent with the (correct) source bytes,
  // falling back to a full reparse if the incremental apply would desync.
  EditorApp app;
  ASSERT_TRUE(app.structuredEditingEnabled());
  ASSERT_TRUE(app.loadFromString(kTwoRectsSvg));

  std::string previousSourceText(kTwoRectsSvg);
  std::optional<std::string> lastWritebackSourceText;
  std::string editedSource(kTwoRectsSvg);
  const std::size_t pos = editedSource.find(R"(<rect id="b")");
  ASSERT_NE(pos, std::string::npos);
  editedSource.insert(pos, R"(<rect id="c" x="70" y="10" width="10" height="10"/>
         )");

  const auto dispatch =
      DispatchSourceTextChange(app, editedSource, &previousSourceText, &lastWritebackSourceText);
  EXPECT_TRUE(dispatch.dispatchedMutation);
  // A desync-detecting fallback may queue a ReplaceDocument; flush it so the DOM
  // reflects the final source either way.
  app.flushFrame();

  EXPECT_EQ(app.document().document().source(), editedSource);
  // All three elements must exist in the live DOM, matching the source bytes.
  EXPECT_TRUE(app.document().document().querySelector("#a").has_value());
  EXPECT_TRUE(app.document().document().querySelector("#b").has_value())
      << "the trailing sibling was dropped from the DOM (renamed to #c by the ambiguous diff)";
  EXPECT_TRUE(app.document().document().querySelector("#c").has_value());
}

TEST(EditorSyncTest, StructuredSourceEditingIsDefaultForNewEditorApps) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red"/></svg>)";
  constexpr std::string_view kEditedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="blue"/></svg>)";

  EditorApp app;
  ASSERT_TRUE(app.structuredEditingEnabled());
  ASSERT_TRUE(app.loadFromString(kSvg));

  std::string previousSourceText(kSvg);
  std::optional<std::string> lastWritebackSourceText;

  const auto dispatch =
      DispatchSourceTextChange(app, kEditedSvg, &previousSourceText, &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());
  EXPECT_EQ(app.document().document().source(), kEditedSvg);
}

TEST(EditorSyncTest, StructuredSourceEditingRuntimeOptOutUsesDocumentReplace) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red"/></svg>)";
  constexpr std::string_view kEditedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="blue"/></svg>)";

  EditorApp app;
  app.setStructuredEditingEnabled(false);
  ASSERT_TRUE(app.loadFromString(kSvg));

  std::string previousSourceText(kSvg);
  std::optional<std::string> lastWritebackSourceText;

  const auto dispatch =
      DispatchSourceTextChange(app, kEditedSvg, &previousSourceText, &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_FALSE(app.document().queue().empty());
  ASSERT_TRUE(app.flushFrame());
  EXPECT_TRUE(app.document().lastFlushResult().replacedDocument);
  EXPECT_EQ(app.document().document().source(), kEditedSvg);
}

TEST(EditorSyncTest, StructuredMalformedOpeningTagEditDoesNotQueueReplaceDocument) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red"/></svg>)";
  constexpr std::string_view kEditedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red/></svg>)";

  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kSvg));

  std::string previousSourceText(kSvg);
  std::optional<std::string> lastWritebackSourceText;

  const auto dispatch =
      DispatchSourceTextChange(app, kEditedSvg, &previousSourceText, &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());
  EXPECT_EQ(app.document().document().source(), kEditedSvg);
  ASSERT_TRUE(app.document().lastParseError().has_value());

  auto rect = app.document().document().querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  std::optional<RcString> fill = rect->getAttribute("fill");
  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(*fill, RcString("red"));
}

TEST(EditorSyncTest, StructuredOpeningTagRecoveryClearsDiagnosticWithoutCommand) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red"/></svg>)";
  constexpr std::string_view kDirtySvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red/></svg>)";

  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kSvg));

  std::string previousSourceText(kSvg);
  std::optional<std::string> lastWritebackSourceText;
  ASSERT_TRUE(
      DispatchSourceTextChange(app, kDirtySvg, &previousSourceText, &lastWritebackSourceText)
          .dispatchedMutation);
  ASSERT_TRUE(app.document().lastParseError().has_value());

  const auto dispatch =
      DispatchSourceTextChange(app, kSvg, &previousSourceText, &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());
  EXPECT_EQ(app.document().document().source(), kSvg);
  EXPECT_EQ(app.document().lastParseError(), std::nullopt);

  auto rect = app.document().document().querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  std::optional<RcString> fill = rect->getAttribute("fill");
  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(*fill, RcString("red"));
}

TEST(EditorSyncTest, DispatchSourceTextChangeNoOpDoesNotDispatch) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCircleSvg));

  std::string previousSourceText(kCircleSvg);
  std::optional<std::string> lastWritebackSourceText;

  const auto dispatch =
      DispatchSourceTextChange(app, kCircleSvg, &previousSourceText, &lastWritebackSourceText);

  EXPECT_FALSE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_EQ(previousSourceText, kCircleSvg);
  EXPECT_FALSE(lastWritebackSourceText.has_value());
  EXPECT_TRUE(app.document().queue().empty());
}

TEST(EditorSyncTest, DispatchSourceEditIntentsSkipsSelfWritebackEcho) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCircleSvg));

  std::string previousSourceText(kCircleSvg);
  std::optional<std::string> lastWritebackSourceText{std::string(kCircleAt40Svg)};
  const std::vector<SourceEditIntent> ignoredIntents = {
      SourceEditIntent{.offset = kCircleSvg.size() + 1, .removedLength = 1, .replacement = "x"},
  };

  const auto dispatch = DispatchSourceEditIntents(app, ignoredIntents, kCircleAt40Svg,
                                                  &previousSourceText, &lastWritebackSourceText);

  EXPECT_FALSE(dispatch.dispatchedMutation);
  EXPECT_TRUE(dispatch.skippedSelfWriteback);
  EXPECT_EQ(previousSourceText, kCircleAt40Svg);
  EXPECT_FALSE(lastWritebackSourceText.has_value());
  EXPECT_TRUE(app.document().queue().empty());
}

TEST(EditorSyncTest, DispatchSourceEditIntentsNoOpDoesNotApplyStaleIntent) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCircleSvg));

  std::string previousSourceText(kCircleSvg);
  std::optional<std::string> lastWritebackSourceText;
  const std::vector<SourceEditIntent> staleIntents = {
      SourceEditIntent{.offset = kCircleSvg.size() + 1, .removedLength = 1, .replacement = "x"},
  };

  const auto dispatch = DispatchSourceEditIntents(app, staleIntents, kCircleSvg,
                                                  &previousSourceText, &lastWritebackSourceText);

  EXPECT_FALSE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_EQ(previousSourceText, kCircleSvg);
  EXPECT_TRUE(app.document().queue().empty());
}

TEST(EditorSyncTest, DispatchSourceEditIntentsEmptyListDelegatesToTextChange) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red"/></svg>)";
  constexpr std::string_view kEditedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="blue"/></svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  std::string previousSourceText(kSvg);
  std::optional<std::string> lastWritebackSourceText;

  const auto dispatch =
      DispatchSourceEditIntents(app, {}, kEditedSvg, &previousSourceText, &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());
  EXPECT_EQ(app.document().document().source(), kEditedSvg);
  EXPECT_EQ(previousSourceText, kEditedSvg);
}

TEST(EditorSyncTest, DispatchSourceEditIntentsInvalidIntentQueuesDocumentReplace) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCircleSvg));

  std::string previousSourceText(kCircleSvg);
  std::optional<std::string> lastWritebackSourceText;
  const std::vector<SourceEditIntent> invalidIntents = {
      SourceEditIntent{.offset = kCircleSvg.size() + 1, .removedLength = 1, .replacement = "x"},
  };

  const auto dispatch = DispatchSourceEditIntents(app, invalidIntents, kCircleAt40Svg,
                                                  &previousSourceText, &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_EQ(previousSourceText, kCircleAt40Svg);
  EXPECT_FALSE(lastWritebackSourceText.has_value());
  EXPECT_FALSE(app.document().queue().empty());
  ASSERT_TRUE(app.flushFrame());
  EXPECT_TRUE(app.document().lastFlushResult().replacedDocument);
  EXPECT_EQ(app.document().document().source(), kCircleAt40Svg);
}

TEST(EditorSyncTest, DispatchSourceEditIntentsStructuredOptOutQueuesDocumentReplace) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red"/></svg>)";
  constexpr std::string_view kEditedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="blue"/></svg>)";

  EditorApp app;
  app.setStructuredEditingEnabled(false);
  ASSERT_TRUE(app.loadFromString(kSvg));

  const std::size_t redOffset = kSvg.find("red");
  ASSERT_NE(redOffset, std::string_view::npos);
  const std::vector<SourceEditIntent> intents = {
      SourceEditIntent{.offset = redOffset, .removedLength = 3, .replacement = "blue"},
  };
  std::string previousSourceText(kSvg);
  std::optional<std::string> lastWritebackSourceText;

  const auto dispatch = DispatchSourceEditIntents(app, intents, kEditedSvg, &previousSourceText,
                                                  &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_FALSE(app.document().queue().empty());
  ASSERT_TRUE(app.flushFrame());
  EXPECT_TRUE(app.document().lastFlushResult().replacedDocument);
  EXPECT_EQ(app.document().document().source(), kEditedSvg);
}

TEST(EditorSyncTest, DispatchSourceEditIntentsFinalMismatchQueuesDocumentReplace) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red"/></svg>)";
  constexpr std::string_view kIntentSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="blue"/></svg>)";
  constexpr std::string_view kNewSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="green"/></svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  const std::size_t redOffset = kSvg.find("red");
  ASSERT_NE(redOffset, std::string_view::npos);
  const std::vector<SourceEditIntent> intents = {
      SourceEditIntent{.offset = redOffset, .removedLength = 3, .replacement = "blue"},
  };
  std::string previousSourceText(kSvg);
  std::optional<std::string> lastWritebackSourceText;

  const auto dispatch = DispatchSourceEditIntents(app, intents, kNewSvg, &previousSourceText,
                                                  &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_EQ(app.document().document().source(), kIntentSvg);
  EXPECT_FALSE(app.document().queue().empty());
  ASSERT_TRUE(app.flushFrame());
  EXPECT_TRUE(app.document().lastFlushResult().replacedDocument);
  EXPECT_EQ(app.document().document().source(), kNewSvg);
  EXPECT_EQ(previousSourceText, kNewSvg);
}

TEST(EditorSyncTest, DispatchSourceEditIntentsAppliesMultipleStructuredEdits) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="red" stroke="black"/></svg>)";
  constexpr std::string_view kEditedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r" fill="tan" stroke="green"/></svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  const std::size_t redOffset = kSvg.find("red");
  const std::size_t blackOffset = kSvg.find("black");
  ASSERT_NE(redOffset, std::string_view::npos);
  ASSERT_NE(blackOffset, std::string_view::npos);
  const std::vector<SourceEditIntent> intents = {
      SourceEditIntent{.offset = redOffset, .removedLength = 3, .replacement = "tan"},
      SourceEditIntent{.offset = blackOffset, .removedLength = 5, .replacement = "green"},
  };
  std::string previousSourceText(kSvg);
  std::optional<std::string> lastWritebackSourceText;

  const auto dispatch = DispatchSourceEditIntents(app, intents, kEditedSvg, &previousSourceText,
                                                  &lastWritebackSourceText);

  EXPECT_TRUE(dispatch.dispatchedMutation);
  EXPECT_FALSE(dispatch.skippedSelfWriteback);
  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());
  EXPECT_EQ(app.document().document().source(), kEditedSvg);
  EXPECT_EQ(previousSourceText, kEditedSvg);

  auto rect = app.document().document().querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  EXPECT_EQ(rect->getAttribute("fill"), std::optional<RcString>(RcString("tan")));
  EXPECT_EQ(rect->getAttribute("stroke"), std::optional<RcString>(RcString("green")));
}

TEST(EditorSyncTest, SelfInitiatedWritebackDoesNotDispatchDuplicateReplaceDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCircleSvg));

  std::string source(kCircleSvg);
  auto circle = app.document().document().querySelector("#c");
  ASSERT_TRUE(circle.has_value());

  auto patch = buildAttributeWriteback(source, *circle, "transform", "translate(5)");
  ASSERT_TRUE(patch.has_value());
  const auto patchResult = applyPatches(source, {{*patch}});
  ASSERT_EQ(patchResult.applied, 1u);

  std::string previousSourceText(kCircleSvg);
  std::optional<std::string> lastWritebackSourceText;
  QueueSourceWritebackReparse(app, source, &previousSourceText, &lastWritebackSourceText);

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
//   3. The user types a character in the source pane - triggers a
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
// gap) OR - ideally - the undo replays against a freshly-queried
// element by id/path.
TEST(EditorSyncTest, DragThenSourceEditThenUndoReplaysAgainstFreshlyParsedElement) {
  // Known gap (documented before we even start the test body, because
  // the failing path triggers a hard EnTT assertion in `fast_mod` -
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
  UndoSnapshot beforeSnapshot{.element = *rect,
                              .transform = before,
                              .writebackTarget = captureAttributeWritebackTarget(*rect)};
  UndoSnapshot afterSnapshot{.element = *rect,
                             .transform = after,
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
  const Transform2d finalTransform = rectAfterUndo->cast<svg::SVGGraphicsElement>().transform();
  EXPECT_TRUE(finalTransform.isIdentity())
      << "undo after source-pane reparse failed to roll back the drag - snapshot dangled";
}

}  // namespace
}  // namespace donner::editor
