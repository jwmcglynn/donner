#include <optional>
#include <string>
#include <string_view>

#include "donner/base/Transform.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TextPatch.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kCircleSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <circle id="c" cx="10" cy="10" r="5"/>
       </svg>)";

void RemoveSubstringOrAssert(std::string* source, std::string_view needle) {
  const std::size_t pos = source->find(needle);
  ASSERT_NE(pos, std::string::npos) << "missing substring: " << needle;
  source->erase(pos, needle.size());
}

bool ApplyCompletedDragWriteback(std::string* source,
                                 const SelectTool::CompletedDragWriteback& writeback) {
  const RcString serialized = toSVGTransformString(writeback.transform);
  auto patch =
      buildAttributeWriteback(*source, writeback.target, "transform", std::string_view(serialized));
  if (!patch.has_value()) {
    return false;
  }

  const auto result = applyPatches(*source, {{*patch}});
  return result.applied == 1u;
}

std::optional<SelectTool::CompletedDragWriteback> DragCircleBy(EditorApp& app, SelectTool& tool,
                                                               double deltaX) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(10.0 + deltaX, 10.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(10.0 + deltaX, 10.0));

  EXPECT_TRUE(app.flushFrame());
  return tool.consumeCompletedDragWriteback();
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

}  // namespace
}  // namespace donner::editor
