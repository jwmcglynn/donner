#include <optional>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/SelectionAabb.h"
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

bool ApplyCompletedDragWriteback(std::string* source,
                                 const SelectTool::CompletedDragWriteback& writeback) {
  const RcString serialized = toSVGTransformString(writeback.transform);
  if (std::string_view(serialized).empty()) {
    auto patch = buildAttributeRemoveWriteback(*source, writeback.target, "transform");
    if (!patch.has_value()) {
      return true;
    }

    const auto result = applyPatches(*source, {{*patch}});
    return result.applied == 1u;
  }

  auto patch =
      buildAttributeWriteback(*source, writeback.target, "transform", std::string_view(serialized));
  if (!patch.has_value()) {
    return false;
  }

  const auto result = applyPatches(*source, {{*patch}});
  return result.applied == 1u;
}

bool ApplyElementRemoveWriteback(std::string* source, const AttributeWritebackTarget& target) {
  auto patch = buildElementRemoveWriteback(*source, target);
  if (!patch.has_value()) {
    return false;
  }

  const auto result = applyPatches(*source, {{*patch}});
  return result.applied == 1u;
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

}  // namespace
}  // namespace donner::editor
