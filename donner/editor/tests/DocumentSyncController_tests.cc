#include "donner/editor/DocumentSyncController.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <string>
#include <utility>

#include "donner/base/MathUtils.h"
#include "donner/base/Transform.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LockState.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/TextEditor.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/core/Display.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" fill="red"/>
       </svg>)";

constexpr std::string_view kNonCanonicalTransformSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" transform="translate(10, 0)" fill="red"/>
       </svg>)svg";

constexpr std::string_view kTwoRectSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="0" y="0" width="10" height="10"/>
         <rect id="r2" x="20" y="0" width="10" height="10"/>
       </svg>)";

class DocumentSyncControllerTest : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    imguiContext_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.Fonts->Build();

    ASSERT_TRUE(app_.loadFromString(kTrivialSvg));
    app_.setCurrentFilePath("test.svg");
    app_.setCleanSourceText(kTrivialSvg);
    textEditor_.setText(kTrivialSvg);
  }

  void TearDown() override {
    if (imguiContext_ != nullptr) {
      ImGui::DestroyContext(imguiContext_);
      imguiContext_ = nullptr;
    }
  }

  ImGuiContext* imguiContext_ = nullptr;
  EditorApp app_;
  TextEditor textEditor_;
  DocumentSyncController controller_{std::string(kTrivialSvg)};
};

TEST_F(DocumentSyncControllerTest, InitialTextDoesNotMarkDocumentDirty) {
  ASSERT_TRUE(textEditor_.isTextChanged());

  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

  EXPECT_FALSE(app_.isDirty());
  EXPECT_FALSE(textEditor_.isTextChanged());
}

TEST_F(DocumentSyncControllerTest, MultiCharacterUserSourceEditDoesNotQueueFlashWake) {
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  ASSERT_FALSE(textEditor_.nextFlashWakeSeconds().has_value());

  const std::size_t insertOffset = textEditor_.getText().find("<rect");
  ASSERT_NE(insertOffset, std::string::npos);
  textEditor_.setCursorPosition(textEditor_.getCoordinatesAtByteOffset(insertOffset));
  textEditor_.insertText("<!-- note -->");

  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

  EXPECT_FALSE(textEditor_.nextFlashWakeSeconds().has_value());
}

TEST_F(DocumentSyncControllerTest, SingleCharacterUserSourceEditDoesNotQueueFlashWake) {
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  ASSERT_FALSE(textEditor_.nextFlashWakeSeconds().has_value());

  const std::size_t insertOffset = textEditor_.getText().find("<rect");
  ASSERT_NE(insertOffset, std::string::npos);
  textEditor_.setCursorPosition(textEditor_.getCoordinatesAtByteOffset(insertOffset));
  textEditor_.insertText(" ");

  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

  EXPECT_FALSE(textEditor_.nextFlashWakeSeconds().has_value());
}

TEST_F(DocumentSyncControllerTest, ThrottledCompletedStyleEditReportsWakeAndAppliesOnIdle) {
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

  const std::size_t rectTagOffset = textEditor_.getText().find("<rect");
  ASSERT_NE(rectTagOffset, std::string::npos);
  const std::size_t insertOffset = rectTagOffset + std::string_view("<rect").size();
  textEditor_.setCursorPosition(textEditor_.getCoordinatesAtByteOffset(insertOffset));

  textEditor_.insertText(" ");
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  EXPECT_TRUE(controller_.nextTextSyncWakeSeconds().has_value());

  textEditor_.insertText(R"(style="display:none")");
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  EXPECT_TRUE(controller_.nextTextSyncWakeSeconds().has_value());

  auto rect = app_.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  EXPECT_FALSE(rect->getAttribute("style").has_value());

  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.2f);
  EXPECT_FALSE(controller_.nextTextSyncWakeSeconds().has_value());

  rect = app_.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  EXPECT_EQ(rect->getAttribute("style"), std::optional<RcString>(RcString("display:none")));
  EXPECT_EQ(rect->getComputedStyle().display.get().value(), svg::Display::None);
}

TEST_F(DocumentSyncControllerTest, PartialOpeningTagEditPreservesSelectionWhileInvalid) {
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

  std::optional<svg::SVGElement> rect = app_.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app_.setSelection(*rect);

  const std::size_t rectTagOffset = textEditor_.getText().find("<rect");
  ASSERT_NE(rectTagOffset, std::string::npos);
  const std::size_t insertOffset = rectTagOffset + std::string_view("<rect").size();
  textEditor_.setCursorPosition(textEditor_.getCoordinatesAtByteOffset(insertOffset));

  for (const char ch : std::string_view(" style=\"display:none\"")) {
    textEditor_.insertText(std::string(1, ch));
    controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

    ASSERT_TRUE(app_.hasSelection()) << "lost selection after typing '" << ch << "'";
    ASSERT_EQ(app_.selectedElements().size(), 1u);
    EXPECT_TRUE(app_.selectedElements().front() == *rect);

    controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.2f);
  }

  EXPECT_FALSE(app_.document().lastParseError().has_value());
  ASSERT_TRUE(app_.hasSelection());
  EXPECT_TRUE(app_.selectedElements().front() == *rect);
  EXPECT_NE(app_.document().document().source().find(R"(style="display:none")"),
            std::string_view::npos);
}

TEST_F(DocumentSyncControllerTest, TypingDisplayNoneBeforePathClassKeepsSelectionAfterFlush) {
  constexpr std::string_view kPathClassSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <style>.cls-82{fill:red}</style>
  <path class="cls-82" d="M10 10 H60 V60 H10 Z"/>
</svg>)svg";

  ASSERT_TRUE(app_.loadFromString(kPathClassSvg));
  app_.setCleanSourceText(kPathClassSvg);
  textEditor_.setText(kPathClassSvg);
  controller_.resetForLoadedDocument(std::string(kPathClassSvg));
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

  std::optional<svg::SVGElement> path = app_.document().document().querySelector(".cls-82");
  ASSERT_TRUE(path.has_value());
  app_.setSelection(*path);

  const std::size_t pathTagOffset = textEditor_.getText().find("<path");
  ASSERT_NE(pathTagOffset, std::string::npos);
  const std::size_t insertOffset = pathTagOffset + std::string_view("<path").size();
  textEditor_.setCursorPosition(textEditor_.getCoordinatesAtByteOffset(insertOffset));

  for (const char ch : std::string_view(" style=\"display:none\"")) {
    textEditor_.insertText(std::string(1, ch));
    controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
    (void)app_.flushFrame();

    controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.2f);
    (void)app_.flushFrame();

    ASSERT_TRUE(app_.hasSelection()) << "lost selection after typing '" << ch << "'";
    ASSERT_EQ(app_.selectedElements().size(), 1u);
    EXPECT_TRUE(app_.selectedElement()->isa<svg::SVGGeometryElement>());
  }

  ASSERT_TRUE(app_.selectedElement().has_value());
  EXPECT_EQ(app_.selectedElement()->getComputedStyle().display.get().value(), svg::Display::None);
}

TEST_F(DocumentSyncControllerTest, RevertingTextToCleanBaselineClearsDirtyFlag) {
  textEditor_.setText(std::string(kTrivialSvg) + "\n<!-- edit -->\n");
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  EXPECT_TRUE(app_.isDirty());

  textEditor_.setText(kTrivialSvg);
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  EXPECT_FALSE(app_.isDirty());
}

TEST_F(DocumentSyncControllerTest, UndoingDragWritebackBackToBaselineClearsDirtyFlag) {
  SelectTool tool;

  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  ASSERT_FALSE(app_.isDirty());

  tool.onMouseDown(app_, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app_, Vector2d(25.0, 15.0), /*buttonHeld=*/true);
  tool.onMouseUp(app_, Vector2d(25.0, 15.0));
  ASSERT_TRUE(app_.flushFrame());
  ASSERT_TRUE(app_.isDirty());

  controller_.applyPendingWritebacks(app_, tool, textEditor_);
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  EXPECT_FALSE(app_.flushFrame());
  ASSERT_TRUE(app_.isDirty());

  app_.undo();
  ASSERT_TRUE(app_.flushFrame());

  controller_.applyPendingWritebacks(app_, tool, textEditor_);
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  EXPECT_FALSE(app_.flushFrame());
  EXPECT_FALSE(app_.isDirty());
}

TEST_F(DocumentSyncControllerTest, SourceBackedDragWritebackMirrorsWithoutTextChangeEcho) {
  SelectTool tool;

  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  ASSERT_FALSE(textEditor_.isTextChanged());
  const std::uint64_t documentGeneration = app_.document().documentGeneration();

  tool.onMouseDown(app_, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app_, Vector2d(25.0, 15.0), /*buttonHeld=*/true);
  tool.onMouseUp(app_, Vector2d(25.0, 15.0));
  ASSERT_TRUE(app_.flushFrame());
  EXPECT_FALSE(app_.document().lastFlushResult().replacedDocument);
  EXPECT_EQ(app_.document().documentGeneration(), documentGeneration);

  controller_.applyPendingWritebacks(app_, tool, textEditor_);

  EXPECT_EQ(textEditor_.getText(), app_.document().document().source());
  EXPECT_FALSE(textEditor_.isTextChanged());
  EXPECT_TRUE(textEditor_.nextFlashWakeSeconds().has_value());
  EXPECT_TRUE(app_.document().queue().empty());
}

TEST_F(DocumentSyncControllerTest, SourceBackedDragWritebackExtendsSelectedSourceRange) {
  SelectTool tool;

  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  ASSERT_FALSE(textEditor_.isTextChanged());

  const std::string beforeText = textEditor_.getText();
  const std::size_t rectStart = beforeText.find("<rect");
  ASSERT_NE(rectStart, std::string::npos);
  const std::size_t rectEnd = beforeText.find("/>", rectStart);
  ASSERT_NE(rectEnd, std::string::npos);
  textEditor_.setSelection(textEditor_.getCoordinatesAtByteOffset(rectStart),
                           textEditor_.getCoordinatesAtByteOffset(rectEnd + 2));
  textEditor_.setCursorPosition(textEditor_.getCoordinatesAtByteOffset(rectStart));

  tool.onMouseDown(app_, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app_, Vector2d(25.0, 15.0), /*buttonHeld=*/true);
  tool.onMouseUp(app_, Vector2d(25.0, 15.0));
  ASSERT_TRUE(app_.flushFrame());

  controller_.applyPendingWritebacks(app_, tool, textEditor_);

  const std::string selectedText = textEditor_.getSelectedText();
  EXPECT_NE(selectedText.find("<rect"), std::string::npos);
  EXPECT_NE(selectedText.find("transform=\"translate(10)\""), std::string::npos);
  EXPECT_TRUE(selectedText.ends_with("/>")) << selectedText;
  EXPECT_EQ(textEditor_.getCursorPosition(), textEditor_.getCoordinatesAtByteOffset(rectStart));
}

TEST_F(DocumentSyncControllerTest, ForwardWritebackPreservesAuthorSyntaxAndUndoRestoresBytes) {
  // A forward transform edit on an authored rotate(45) must rewrite the source
  // as rotate(60) (author form), not canonicalize to matrix(); a subsequent
  // verbatim restore must return the exact original bytes.
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="0" y="0" width="10" height="10" transform="rotate(45)"/>
       </svg>)svg";

  EditorApp app;
  TextEditor textEditor;
  SelectTool tool;
  DocumentSyncController controller{std::string(kSvg)};

  ASSERT_TRUE(app.loadFromString(kSvg));
  app.setCurrentFilePath("test.svg");
  app.setCleanSourceText(kSvg);
  textEditor.setText(kSvg);
  textEditor.resetTextChanged();

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  auto r1Target = captureAttributeWritebackTarget(*r1);
  ASSERT_TRUE(r1Target.has_value());

  // Forward edit: rotate(45) -> rotate(60), carrying the author's bytes.
  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = *r1Target,
      .transform = Transform2d::Rotate(60.0 * MathConstants<double>::kDegToRad),
      .sourceTransformAttributeValue = RcString("rotate(45)"),
  });
  controller.applyPendingWritebacks(app, tool, textEditor);

  r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->getAttribute("transform"), std::optional<RcString>(RcString("rotate(60)")));
  EXPECT_EQ(textEditor.getText().find("matrix("), std::string::npos);
  EXPECT_NE(textEditor.getText().find("transform=\"rotate(60)\""), std::string::npos);

  // Verbatim restore (the undo path): exact original bytes come back.
  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = *r1Target,
      .transform = Transform2d::Rotate(45.0 * MathConstants<double>::kDegToRad),
      .sourceTransformAttributeValue = RcString("rotate(45)"),
      .restoreSourceTransformAttributeValue = true,
  });
  controller.applyPendingWritebacks(app, tool, textEditor);

  r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->getAttribute("transform"), std::optional<RcString>(RcString("rotate(45)")));
  EXPECT_NE(textEditor.getText().find("transform=\"rotate(45)\""), std::string::npos);
}

TEST_F(DocumentSyncControllerTest, AppTransformWritebacksDrainAllQueuedEntries) {
  EditorApp app;
  TextEditor textEditor;
  SelectTool tool;
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCurrentFilePath("test.svg");
  app.setCleanSourceText(kTwoRectSvg);
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  auto r1Target = captureAttributeWritebackTarget(*r1);
  auto r2Target = captureAttributeWritebackTarget(*r2);
  ASSERT_TRUE(r1Target.has_value());
  ASSERT_TRUE(r2Target.has_value());

  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = *r1Target, .transform = Transform2d::Translate(Vector2d(5.0, 0.0))});
  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = *r2Target, .transform = Transform2d::Translate(Vector2d(11.0, 0.0))});

  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_FALSE(textEditor.isTextChanged());
  r1 = app.document().document().querySelector("#r1");
  r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r1->getAttribute("transform"), std::optional<RcString>(RcString("translate(5)")));
  EXPECT_EQ(r2->getAttribute("transform"), std::optional<RcString>(RcString("translate(11)")));
}

TEST_F(DocumentSyncControllerTest, MultiSelectionDragWritebackDrainsSelectToolExtras) {
  EditorApp app;
  TextEditor textEditor;
  SelectTool tool;
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCurrentFilePath("test.svg");
  app.setCleanSourceText(kTwoRectSvg);
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  tool.onMouseDown(app, Vector2d(5.0, 5.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(13.0, 5.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(13.0, 5.0));
  ASSERT_TRUE(app.flushFrame());

  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_FALSE(textEditor.isTextChanged());
  r1 = app.document().document().querySelector("#r1");
  r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r1->getAttribute("transform"), std::optional<RcString>(RcString("translate(8)")));
  EXPECT_EQ(r2->getAttribute("transform"), std::optional<RcString>(RcString("translate(8)")));
}

TEST_F(DocumentSyncControllerTest, TransformWritebackRestoreAndEmptyTransformBranches) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="0" y="0" width="10" height="10" transform="translate(1)"/>
         <rect id="r2" x="20" y="0" width="10" height="10" transform="translate(2)"/>
         <rect id="r3" x="40" y="0" width="10" height="10" transform="translate(3)"/>
       </svg>)svg";

  EditorApp app;
  TextEditor textEditor;
  SelectTool tool;
  DocumentSyncController controller{std::string(kSvg)};

  ASSERT_TRUE(app.loadFromString(kSvg));
  app.setCurrentFilePath("test.svg");
  app.setCleanSourceText(kSvg);
  textEditor.setText(kSvg);
  textEditor.resetTextChanged();

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  auto r3 = app.document().document().querySelector("#r3");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r3.has_value());
  auto r1Target = captureAttributeWritebackTarget(*r1);
  auto r2Target = captureAttributeWritebackTarget(*r2);
  auto r3Target = captureAttributeWritebackTarget(*r3);
  ASSERT_TRUE(r1Target.has_value());
  ASSERT_TRUE(r2Target.has_value());
  ASSERT_TRUE(r3Target.has_value());

  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = AttributeWritebackTarget{},
      .transform = Transform2d::Translate(Vector2d(99.0, 0.0)),
  });
  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = *r1Target,
      .transform = Transform2d::Translate(Vector2d(7.0, 0.0)),
      .sourceTransformAttributeValue = RcString("translate(4 5)"),
      .restoreSourceTransformAttributeValue = true,
  });
  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = *r2Target,
      .transform = Transform2d::Translate(Vector2d(7.0, 0.0)),
      .restoreSourceTransformAttributeValue = true,
  });
  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = *r3Target,
      .transform = Transform2d(),
  });

  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_FALSE(textEditor.isTextChanged());
  r1 = app.document().document().querySelector("#r1");
  r2 = app.document().document().querySelector("#r2");
  r3 = app.document().document().querySelector("#r3");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r1->getAttribute("transform"), std::optional<RcString>(RcString("translate(4 5)")));
  EXPECT_FALSE(r2->getAttribute("transform").has_value());
  EXPECT_FALSE(r3->getAttribute("transform").has_value());
}

TEST_F(DocumentSyncControllerTest, SourceBackedDeleteWritebackMirrorsFlushDeltaWithoutTextEcho) {
  SelectTool tool;

  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  ASSERT_FALSE(textEditor_.isTextChanged());
  const std::uint64_t documentGeneration = app_.document().documentGeneration();

  std::optional<svg::SVGElement> rect = app_.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  std::optional<AttributeWritebackTarget> target = captureAttributeWritebackTarget(*rect);
  ASSERT_TRUE(target.has_value());

  app_.enqueueElementRemoveWriteback(EditorApp::CompletedElementRemoveWriteback{.target = *target});
  app_.applyMutation(EditorCommand::DeleteElementCommand(*rect));
  ASSERT_TRUE(app_.flushFrame());
  EXPECT_FALSE(app_.document().lastFlushResult().replacedDocument);
  EXPECT_EQ(app_.document().documentGeneration(), documentGeneration);
  ASSERT_EQ(app_.document().lastFlushResult().sourceDeltas.size(), 1u);

  controller_.applyPendingWritebacks(app_, tool, textEditor_);

  EXPECT_EQ(textEditor_.getText(), app_.document().document().source());
  EXPECT_FALSE(textEditor_.isTextChanged());
  EXPECT_TRUE(app_.document().queue().empty());
  EXPECT_FALSE(app_.document().document().querySelector("#r1").has_value());
}

TEST_F(DocumentSyncControllerTest, PathOperationMirrorsInsertAndDeletesWithoutTextEcho) {
  constexpr std::string_view kTwoOverlappingRects =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="40" height="40" fill="red"/>
         <rect id="r2" x="30" y="25" width="40" height="20" fill="blue"/>
       </svg>)";

  EditorApp app;
  TextEditor textEditor;
  SelectTool tool;
  DocumentSyncController controller{std::string(kTwoOverlappingRects)};

  ASSERT_TRUE(app.loadFromString(kTwoOverlappingRects));
  app.setCurrentFilePath("test.svg");
  app.setCleanSourceText(kTwoOverlappingRects);
  textEditor.setText(kTwoOverlappingRects);
  textEditor.resetTextChanged();

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::Intersect));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_FALSE(app.document().lastFlushResult().replacedDocument);
  EXPECT_GE(app.document().lastFlushResult().sourceDeltas.size(), 3u);

  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_FALSE(textEditor.isTextChanged());
  EXPECT_TRUE(app.document().document().querySelector("path").has_value());
  EXPECT_FALSE(app.document().document().querySelector("#r1").has_value());
  EXPECT_FALSE(app.document().document().querySelector("#r2").has_value());
}

TEST_F(DocumentSyncControllerTest, UiDeleteUndoRestoresDeletedElementAndSourceText) {
  EditorApp app;
  TextEditor textEditor;
  SelectTool tool;
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCurrentFilePath("test.svg");
  app.setCleanSourceText(kTwoRectSvg);
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  ASSERT_TRUE(app.deleteSelectionWithUndo(textEditor.getText()));
  ASSERT_TRUE(app.canUndo());
  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_FALSE(app.document().document().querySelector("#r1").has_value());
  EXPECT_EQ(textEditor.getText().find("id=\"r1\""), std::string::npos);

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_TRUE(app.document().document().querySelector("#r1").has_value());
  EXPECT_EQ(textEditor.getText(), std::string(kTwoRectSvg));
  EXPECT_FALSE(app.isDirty());
  ASSERT_TRUE(app.canRedo());

  app.redo();
  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_FALSE(app.document().document().querySelector("#r1").has_value());
  EXPECT_EQ(textEditor.getText().find("id=\"r1\""), std::string::npos);
}

TEST_F(DocumentSyncControllerTest, DeleteSelectionSkipsLockedElementsInSourceToo) {
  constexpr std::string_view kLockedRectSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="locked" data-donner-locked="true" x="0" y="0" width="10" height="10"/>
         <rect id="free" x="20" y="0" width="10" height="10"/>
       </svg>)";

  EditorApp app;
  TextEditor textEditor;
  SelectTool tool;
  DocumentSyncController controller{std::string(kLockedRectSvg)};

  ASSERT_TRUE(app.loadFromString(kLockedRectSvg));
  app.setCurrentFilePath("test.svg");
  app.setCleanSourceText(kLockedRectSvg);
  textEditor.setText(kLockedRectSvg);
  textEditor.resetTextChanged();

  auto locked = app.document().document().querySelector("#locked");
  ASSERT_TRUE(locked.has_value());

  // Deleting a selection that is entirely locked is a no-op: no undo entry,
  // no removal writeback, and the element survives in both DOM and source.
  app.setSelection(*locked);
  EXPECT_FALSE(app.deleteSelectionWithUndo(textEditor.getText()));
  EXPECT_FALSE(app.canUndo());
  app.flushFrame();
  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_TRUE(app.document().document().querySelector("#locked").has_value());
  EXPECT_NE(textEditor.getText().find("id=\"locked\""), std::string::npos)
      << "deleting a locked element must not splice it out of the source text";

  // A mixed selection deletes only the unlocked element; the locked one
  // survives in DOM and source and stays selected.
  auto free = app.document().document().querySelector("#free");
  ASSERT_TRUE(free.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*locked, *free});
  ASSERT_TRUE(app.deleteSelectionWithUndo(textEditor.getText()));
  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_FALSE(app.document().document().querySelector("#free").has_value());
  EXPECT_EQ(textEditor.getText().find("id=\"free\""), std::string::npos);
  EXPECT_TRUE(app.document().document().querySelector("#locked").has_value());
  EXPECT_NE(textEditor.getText().find("id=\"locked\""), std::string::npos);
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_TRUE(IsLocked(app.selectedElements().front()));
}

TEST_F(DocumentSyncControllerTest, SetTextContentMirrorsIntoSourceText) {
  constexpr std::string_view kTextSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <text id="t" x="10" y="20">old</text>
       </svg>)";

  EditorApp app;
  TextEditor textEditor;
  SelectTool tool;
  DocumentSyncController controller{std::string(kTextSvg)};

  ASSERT_TRUE(app.loadFromString(kTextSvg));
  app.setCurrentFilePath("test.svg");
  app.setCleanSourceText(kTextSvg);
  textEditor.setText(kTextSvg);
  textEditor.resetTextChanged();

  auto text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());

  app.applyMutation(EditorCommand::SetTextContentCommand(*text, "updated content"));
  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, tool, textEditor);

  // The live DOM, the document source, and the source pane all reflect the
  // edit - text content edits are structural DOM edits like any other, so
  // they must not be lost on save or on the next source reparse.
  EXPECT_EQ(text->cast<svg::SVGTextElement>().textContent(), "updated content");
  EXPECT_NE(std::string(app.document().document().source()).find("updated content"),
            std::string::npos)
      << "SetTextContent must emit source deltas; got source:\n"
      << std::string(app.document().document().source());
  EXPECT_NE(textEditor.getText().find("updated content"), std::string::npos);
  EXPECT_EQ(textEditor.getText(), app.document().document().source());

  // Clearing the content removes the text node from the source too.
  app.applyMutation(EditorCommand::SetTextContentCommand(*text, ""));
  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, tool, textEditor);
  EXPECT_EQ(textEditor.getText().find("updated content"), std::string::npos);
  EXPECT_NE(textEditor.getText().find("<text"), std::string::npos);
}

TEST_F(DocumentSyncControllerTest, UndoToBaselineClearsDirtyFlagWhenSourceHasTrailingNewline) {
  // Regression: donner_splash.svg ships with a trailing '\n'. `TextBuffer`
  // canonicalizes lines when text is round-tripped through it, dropping that
  // trailing newline. If the clean baseline is stored verbatim it never equals
  // the text editor's canonical form, so `syncDirtyFromSource` latches the
  // dirty flag on forever - including after an undo that restores the source.
  EditorApp app;
  TextEditor textEditor;
  SelectTool tool;

  const std::string sourceWithTrailingNewline = std::string(kTrivialSvg) + "\n";
  ASSERT_TRUE(app.loadFromString(sourceWithTrailingNewline));
  app.setCurrentFilePath("test.svg");
  textEditor.setText(sourceWithTrailingNewline);
  // Mimic the production call sequence: route the clean baseline through
  // `textEditor.getText()` so it matches the canonicalized form the editor
  // will read back during `syncDirtyFromSource`.
  app.setCleanSourceText(textEditor.getText());

  DocumentSyncController controller{textEditor.getText()};

  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);
  ASSERT_FALSE(app.isDirty());

  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(25.0, 15.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(25.0, 15.0));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(app.isDirty());

  controller.applyPendingWritebacks(app, tool, textEditor);
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);
  EXPECT_FALSE(app.flushFrame());
  ASSERT_TRUE(app.isDirty());

  app.undo();
  ASSERT_TRUE(app.flushFrame());

  controller.applyPendingWritebacks(app, tool, textEditor);
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);
  EXPECT_FALSE(app.flushFrame());
  EXPECT_FALSE(app.isDirty());
}

TEST_F(DocumentSyncControllerTest, UndoingDragOnNonCanonicalTransformRestoresCleanSourceBaseline) {
  EditorApp app;
  TextEditor textEditor;
  DocumentSyncController controller{std::string(kNonCanonicalTransformSvg)};
  SelectTool tool;

  ASSERT_TRUE(app.loadFromString(kNonCanonicalTransformSvg));
  app.setCurrentFilePath("test.svg");
  app.setCleanSourceText(kNonCanonicalTransformSvg);
  textEditor.setText(kNonCanonicalTransformSvg);

  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);
  ASSERT_FALSE(app.isDirty());

  tool.onMouseDown(app, Vector2d(25.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(35.0, 15.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(35.0, 15.0));
  ASSERT_TRUE(app.flushFrame());

  controller.applyPendingWritebacks(app, tool, textEditor);
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);
  EXPECT_FALSE(app.flushFrame());
  ASSERT_TRUE(app.isDirty());

  app.undo();
  ASSERT_TRUE(app.flushFrame());

  controller.applyPendingWritebacks(app, tool, textEditor);
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);
  EXPECT_FALSE(app.flushFrame());
  EXPECT_EQ(textEditor.getText(), std::string(kNonCanonicalTransformSvg));
  EXPECT_FALSE(app.isDirty());
}

TEST(DocumentSyncControllerStructuredTest, BatchedSiblingSourceEditsStayLocal) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="a" fill="red"/><circle id="b" fill="red"/></svg>)";

  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kSvg));

  TextEditor textEditor;
  textEditor.setText(kSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kSvg)};

  std::string edited(kSvg);
  const std::size_t firstRed = edited.find("red");
  ASSERT_NE(firstRed, std::string::npos);
  textEditor.setSelection(Coordinates(0, static_cast<int>(firstRed)),
                          Coordinates(0, static_cast<int>(firstRed + 3)));
  textEditor.insertText("blue");
  edited.replace(firstRed, 3, "blue");

  const std::size_t secondRed = edited.rfind("red");
  ASSERT_NE(secondRed, std::string::npos);
  textEditor.setSelection(Coordinates(0, static_cast<int>(secondRed)),
                          Coordinates(0, static_cast<int>(secondRed + 3)));
  textEditor.insertText("green");
  edited.replace(secondRed, 3, "green");

  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);

  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());
  EXPECT_EQ(app.document().document().source(), edited);

  auto rect = app.document().document().querySelector("#a");
  ASSERT_TRUE(rect.has_value());
  EXPECT_EQ(rect->getAttribute("fill"), RcString("blue"));

  auto circle = app.document().document().querySelector("#b");
  ASSERT_TRUE(circle.has_value());
  EXPECT_EQ(circle->getAttribute("fill"), RcString("green"));
}

TEST(DocumentSyncControllerStructuredTest, ElementSubtreeSourceInsertStaysLocal) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="layer"><rect id="a" width="10" height="10"/></g></svg>)";
  constexpr std::string_view kInserted = R"(<circle id="b" cx="5" cy="6" r="3" fill="blue"/>)";

  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kSvg));

  TextEditor textEditor;
  textEditor.setText(kSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kSvg)};

  const std::size_t insertOffset = std::string_view(kSvg).find("</g>");
  ASSERT_NE(insertOffset, std::string_view::npos);
  textEditor.setCursorPosition(Coordinates(0, static_cast<int>(insertOffset)));
  textEditor.insertText(kInserted);

  std::string edited(kSvg);
  edited.insert(insertOffset, kInserted);

  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);

  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());
  EXPECT_EQ(app.document().document().source(), edited);

  auto inserted = app.document().document().querySelector("#b");
  ASSERT_TRUE(inserted.has_value());
  EXPECT_EQ(inserted->getAttribute("fill"), RcString("blue"));
}

TEST(DocumentSyncControllerStructuredTest, SelectedElementSourceDeleteClearsSelectionBeforeBounds) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  auto selected = app.document().document().querySelector("#r1");
  ASSERT_TRUE(selected.has_value());
  app.setSelection(*selected);

  const std::string currentText = textEditor.getText();
  const std::string selectedSource = R"(<rect id="r1" x="0" y="0" width="10" height="10"/>)";
  const std::size_t deleteOffset = currentText.find(selectedSource);
  ASSERT_NE(deleteOffset, std::string::npos);

  textEditor.setSelection(
      textEditor.getCoordinatesAtByteOffset(deleteOffset),
      textEditor.getCoordinatesAtByteOffset(deleteOffset + selectedSource.size()));
  textEditor.insertText("");
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);

  EXPECT_TRUE(app.selectedElements().empty());
  SelectionBoundsCache cache;
  RefreshSelectionBoundsCache(cache, std::span<const svg::SVGElement>(app.selectedElements()),
                              app.document().currentFrameVersion(),
                              app.document().currentFrameVersion());
  EXPECT_TRUE(cache.pendingBoundsDoc.empty());
  EXPECT_TRUE(cache.pendingOccludingBoundsDoc.empty());
}

TEST(DocumentSyncControllerStructuredTest, SelectedElementSourceAttributeEditsKeepBoundsSafe) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  auto selected = app.document().document().querySelector("#r1");
  ASSERT_TRUE(selected.has_value());
  app.setSelection(*selected);

  constexpr std::string_view kOldWidth = R"(width="10")";
  constexpr std::string_view kNewWidth = R"(width="24")";
  const std::string currentText = textEditor.getText();
  const std::size_t editOffset = currentText.find(kOldWidth);
  ASSERT_NE(editOffset, std::string::npos);

  textEditor.setSelection(textEditor.getCoordinatesAtByteOffset(editOffset),
                          textEditor.getCoordinatesAtByteOffset(editOffset + kOldWidth.size()));
  textEditor.insertText(kNewWidth);
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front().id(), "r1");
  SelectionBoundsCache cache;
  RefreshSelectionBoundsCache(cache, std::span<const svg::SVGElement>(app.selectedElements()),
                              app.document().currentFrameVersion(),
                              app.document().currentFrameVersion());
  EXPECT_FALSE(cache.displayedBoundsDoc.empty());
}

TEST(DocumentSyncControllerStructuredTest, SourceInsertNearSelectionKeepsBoundsSafe) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="layer"><rect id="a" x="0" y="0" width="10" height="10"/></g></svg>)";
  constexpr std::string_view kInserted = R"(<circle id="b" cx="5" cy="6" r="3" fill="blue"/>)";

  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kSvg));

  TextEditor textEditor;
  textEditor.setText(kSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kSvg)};

  auto selected = app.document().document().querySelector("#a");
  ASSERT_TRUE(selected.has_value());
  app.setSelection(*selected);

  const std::size_t insertOffset = std::string_view(kSvg).find("</g>");
  ASSERT_NE(insertOffset, std::string_view::npos);
  textEditor.setCursorPosition(textEditor.getCoordinatesAtByteOffset(insertOffset));
  textEditor.insertText(kInserted);

  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front().id(), "a");
  SelectionBoundsCache cache;
  RefreshSelectionBoundsCache(cache, std::span<const svg::SVGElement>(app.selectedElements()),
                              app.document().currentFrameVersion(),
                              app.document().currentFrameVersion());
  EXPECT_FALSE(cache.displayedBoundsDoc.empty());
}

TEST(DocumentSyncControllerStructuredTest, SourceTypingMutationStressKeepsSelectionBoundsSafe) {
  struct MutationCase {
    std::string_view name;
    std::string_view selectedId;
    std::string_view oldText;
    std::string_view newText;
    bool expectSelection = true;
    bool expectParseError = false;
  };

  const MutationCase cases[] = {
      {
          .name = "insert attribute in selected opening tag",
          .selectedId = "r1",
          .oldText = R"(id="r1")",
          .newText = R"(id="r1" data-note="typed")",
      },
      {
          .name = "remove selected x attribute",
          .selectedId = "r1",
          .oldText = R"( x="0")",
          .newText = "",
      },
      {
          .name = "replace selected element with same id",
          .selectedId = "r1",
          .oldText = R"(<rect id="r1" x="0" y="0" width="10" height="10"/>)",
          .newText = R"(<rect id="r1" x="3" y="4" width="18" height="16" fill="blue"/>)",
      },
      {
          .name = "delete selected element",
          .selectedId = "r1",
          .oldText = R"(<rect id="r1" x="0" y="0" width="10" height="10"/>)",
          .newText = "",
          .expectSelection = false,
      },
      {
          .name = "type malformed attribute value",
          .selectedId = "r1",
          .oldText = R"(width="10")",
          .newText = R"(width="10)",
          .expectParseError = true,
      },
  };

  for (const MutationCase& mutationCase : cases) {
    SCOPED_TRACE(mutationCase.name);

    EditorApp app;
    app.setStructuredEditingEnabled(true);
    ASSERT_TRUE(app.loadFromString(kTwoRectSvg));

    TextEditor textEditor;
    textEditor.setText(kTwoRectSvg);
    textEditor.resetTextChanged();
    DocumentSyncController controller{std::string(kTwoRectSvg)};

    auto selected =
        app.document().document().querySelector("#" + std::string(mutationCase.selectedId));
    ASSERT_TRUE(selected.has_value());
    app.setSelection(*selected);

    const std::string currentText = textEditor.getText();
    const std::size_t editOffset = currentText.find(mutationCase.oldText);
    ASSERT_NE(editOffset, std::string::npos);

    textEditor.setSelection(
        textEditor.getCoordinatesAtByteOffset(editOffset),
        textEditor.getCoordinatesAtByteOffset(editOffset + mutationCase.oldText.size()));
    textEditor.insertText(mutationCase.newText);
    controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);

    SelectionBoundsCache cacheBeforeFlush;
    RefreshSelectionBoundsCache(
        cacheBeforeFlush, std::span<const svg::SVGElement>(app.selectedElements()),
        app.document().currentFrameVersion(), app.document().currentFrameVersion());

    if (!app.document().queue().empty()) {
      (void)app.flushFrame();
      SelectionBoundsCache cacheAfterFlush;
      RefreshSelectionBoundsCache(
          cacheAfterFlush, std::span<const svg::SVGElement>(app.selectedElements()),
          app.document().currentFrameVersion(), app.document().currentFrameVersion());
    }

    if (mutationCase.expectSelection) {
      EXPECT_FALSE(app.selectedElements().empty());
    } else {
      EXPECT_TRUE(app.selectedElements().empty());
    }
    EXPECT_EQ(app.document().lastParseError().has_value(), mutationCase.expectParseError);
  }
}

TEST_F(DocumentSyncControllerTest, NextTextSyncWakeIsNulloptAfterDebounceDrains) {
  // The fixture's initial setText marks the editor changed; the first
  // handleTextEdits dispatches and arms the throttle.
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

  // Idling past the debounce window with no further edits drains the throttle,
  // after which no wake is pending.
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.2f);
  EXPECT_FALSE(controller_.nextTextSyncWakeSeconds().has_value());
}

TEST_F(DocumentSyncControllerTest, NextTextSyncWakeReportsRemainingDebounceWindow) {
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

  const std::size_t insertOffset = textEditor_.getText().find("<rect");
  ASSERT_NE(insertOffset, std::string::npos);
  textEditor_.setCursorPosition(textEditor_.getCoordinatesAtByteOffset(insertOffset));
  textEditor_.insertText(" ");
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

  const std::optional<float> initialWake = controller_.nextTextSyncWakeSeconds();
  ASSERT_TRUE(initialWake.has_value());
  EXPECT_GT(*initialWake, 0.0f);

  // Advancing the idle timer (without new edits) shrinks the reported wake but
  // does not drop below zero.
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.05f);
  const std::optional<float> laterWake = controller_.nextTextSyncWakeSeconds();
  ASSERT_TRUE(laterWake.has_value());
  EXPECT_LT(*laterWake, *initialWake);
  EXPECT_GE(*laterWake, 0.0f);
}

TEST_F(DocumentSyncControllerTest, SyncParseErrorMarkersHandlesErrorAppearAndClear) {
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);

  // No parse error initially: the no-error branch is taken and must not throw.
  controller_.syncParseErrorMarkers(app_, textEditor_);

  // Introduce a malformed attribute value so the next flush yields a parse error.
  const std::size_t widthOffset = textEditor_.getText().find("width=\"20\"");
  ASSERT_NE(widthOffset, std::string::npos);
  textEditor_.setSelection(textEditor_.getCoordinatesAtByteOffset(widthOffset),
                           textEditor_.getCoordinatesAtByteOffset(
                               widthOffset + std::string_view("width=\"20\"").size()));
  textEditor_.insertText("width=\"20");
  // Dispatch then drain the debounce so the malformed source is reparsed.
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.2f);
  (void)app_.flushFrame();

  ASSERT_TRUE(app_.document().lastParseError().has_value());
  // Error-present branch: sets markers from the diagnostic.
  controller_.syncParseErrorMarkers(app_, textEditor_);
  ASSERT_EQ(controller_.sourceDiagnostics().diagnostics.size(), 1u);
  EXPECT_EQ(controller_.sourceDiagnostics().revision, app_.document().parseDiagnosticsRevision());
  EXPECT_EQ(controller_.sourceDiagnostics().diagnostics.front().severity,
            DiagnosticSeverity::Error);
  EXPECT_GT(controller_.sourceDiagnostics().diagnostics.front().range.end,
            controller_.sourceDiagnostics().diagnostics.front().range.start);
  // Repeated calls with the same error hit the change-detection short-circuit.
  controller_.syncParseErrorMarkers(app_, textEditor_);

  // Revert to valid source; the next sync takes the clear branch.
  textEditor_.setText(kTrivialSvg);
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.2f);
  (void)app_.flushFrame();
  ASSERT_FALSE(app_.document().lastParseError().has_value());
  controller_.syncParseErrorMarkers(app_, textEditor_);
  EXPECT_TRUE(controller_.sourceDiagnostics().diagnostics.empty());
}

TEST(DocumentSyncControllerStructuredTest, MirrorSourceDeltasWithEmptyDeltasReturnsFalse) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  EXPECT_FALSE(controller.mirrorSourceDeltas(app, textEditor, {}));
}

TEST(DocumentSyncControllerStructuredTest, MirrorSourceDeltasWithoutDocumentReturnsFalse) {
  EditorApp app;
  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  xml::XMLSourceDelta delta;
  delta.offset = 0;
  delta.removedLength = 0;
  delta.insertedLength = 1;

  EXPECT_FALSE(controller.mirrorSourceDeltas(app, textEditor, {delta}));
  EXPECT_EQ(textEditor.getText(), std::string(kTwoRectSvg));
}

TEST(DocumentSyncControllerStructuredTest, MirrorSourceDeltasWithSourceLessDocumentReturnsFalse) {
  EditorApp app;
  svg::SVGDocument document;
  app.document().setDocument(std::move(document));
  ASSERT_TRUE(app.hasDocument());
  ASSERT_FALSE(app.document().document().hasSourceStore());

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  xml::XMLSourceDelta delta;
  delta.offset = 0;
  delta.removedLength = 0;
  delta.insertedLength = 1;

  EXPECT_FALSE(controller.mirrorSourceDeltas(app, textEditor, {delta}));
  EXPECT_EQ(textEditor.getText(), std::string(kTwoRectSvg));
}

TEST(DocumentSyncControllerStructuredTest, MirrorSourceDeltasOutOfRangeFallsBackToFullMirror) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCleanSourceText(kTwoRectSvg);

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  // A delta whose offset/lengths point far past the current source forces the
  // out-of-range guard, which falls back to a full document mirror rather than
  // applying a corrupt splice. The mirror still succeeds (returns true) and
  // leaves the editor text equal to the document source.
  std::vector<xml::XMLSourceDelta> deltas;
  xml::XMLSourceDelta delta;
  delta.offset = 100000;
  delta.removedLength = 5;
  delta.insertedLength = 5;
  deltas.push_back(delta);

  EXPECT_TRUE(controller.mirrorSourceDeltas(app, textEditor, deltas));
  EXPECT_EQ(textEditor.getText(), app.document().document().source());
}

TEST(DocumentSyncControllerStructuredTest, ResetForLoadedDocumentClearsThrottleState) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  // Make an edit so the controller enters its throttled state.
  const std::size_t offset = textEditor.getText().find("<rect");
  ASSERT_NE(offset, std::string::npos);
  textEditor.setCursorPosition(textEditor.getCoordinatesAtByteOffset(offset));
  textEditor.insertText(" ");
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);
  ASSERT_TRUE(controller.nextTextSyncWakeSeconds().has_value());

  // Resetting for a freshly loaded document clears the throttle so no wake is
  // pending.
  controller.resetForLoadedDocument(std::string(kTwoRectSvg));
  EXPECT_FALSE(controller.nextTextSyncWakeSeconds().has_value());
}

TEST(DocumentSyncControllerStructuredTest, DebouncedTrailingEditAppliesOnIdle) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCleanSourceText(kTwoRectSvg);

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  // First edit dispatches immediately and starts the throttle.
  std::size_t fillOffset = textEditor.getText().find("width=\"10\"");
  ASSERT_NE(fillOffset, std::string::npos);
  textEditor.setSelection(
      textEditor.getCoordinatesAtByteOffset(fillOffset),
      textEditor.getCoordinatesAtByteOffset(fillOffset + std::string_view("width=\"10\"").size()));
  textEditor.insertText("width=\"12\"");
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);
  ASSERT_TRUE(controller.nextTextSyncWakeSeconds().has_value());

  // A second edit within the debounce window is held as pending (not dispatched
  // yet) because the controller is still throttled.
  fillOffset = textEditor.getText().find("width=\"12\"");
  ASSERT_NE(fillOffset, std::string::npos);
  textEditor.setSelection(
      textEditor.getCoordinatesAtByteOffset(fillOffset),
      textEditor.getCoordinatesAtByteOffset(fillOffset + std::string_view("width=\"12\"").size()));
  textEditor.insertText("width=\"24\"");
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);

  // Idle past the debounce window: the pending trailing edit flushes and the
  // throttle clears.
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.2f);
  EXPECT_FALSE(controller.nextTextSyncWakeSeconds().has_value());

  (void)app.flushFrame();
  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  EXPECT_EQ(rect->getAttribute("width"), std::optional<RcString>(RcString("24")));
}

TEST(DocumentSyncControllerStructuredTest, MultiDeltaMoveMirrorsIntoTextPane) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="moved" fill="red" /><g id="target" /></svg>)";

  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kSvg));
  app.setCleanSourceText(kSvg);
  const std::uint64_t documentGeneration = app.document().documentGeneration();

  TextEditor textEditor;
  textEditor.setText(kSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kSvg)};

  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  auto moved = app.document().document().querySelector("#moved");
  ASSERT_TRUE(moved.has_value());

  xml::ApplySourceEditResult result = app.document().document().insertElement(*target, *moved);
  ASSERT_TRUE(result.applied);
  ASSERT_EQ(result.sourceDeltas.size(), 2u);
  EXPECT_TRUE(std::ranges::any_of(result.sourceDeltas, [](const xml::XMLSourceDelta& delta) {
    return delta.insertedLength > 0;
  }));

  EXPECT_TRUE(controller.mirrorSourceDeltas(app, textEditor, result.sourceDeltas));

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_FALSE(textEditor.isTextChanged());
  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());
  EXPECT_EQ(app.document().documentGeneration(), documentGeneration);
  EXPECT_NE(textEditor.getText().find(R"(<g id="target"><rect id="moved")"), std::string::npos);
  EXPECT_TRUE(app.isDirty());
}

TEST(DocumentSyncControllerStructuredTest, MirrorSourceDeltasPreservesLocalEditAndQueuesReparse) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCleanSourceText(kTwoRectSvg);

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  const std::size_t rectOffset = textEditor.getText().find("<rect");
  ASSERT_NE(rectOffset, std::string::npos);
  textEditor.setCursorPosition(textEditor.getCoordinatesAtByteOffset(rectOffset));
  textEditor.insertText("<!-- local -->");
  textEditor.resetTextChanged();

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  xml::ApplySourceEditResult result =
      app.document().document().setElementAttribute(*rect, "fill", "green");
  ASSERT_TRUE(result.applied);
  ASSERT_FALSE(result.sourceDeltas.empty());

  EXPECT_TRUE(controller.mirrorSourceDeltas(app, textEditor, result.sourceDeltas));

  EXPECT_NE(textEditor.getText().find("<!-- local -->"), std::string::npos);
  EXPECT_NE(textEditor.getText().find(R"(fill="green")"), std::string::npos);
  EXPECT_FALSE(textEditor.isTextChanged());
  EXPECT_FALSE(app.document().queue().empty());
}

TEST(DocumentSyncControllerStructuredTest, MirrorSourceDeltasMapsBeforeLocalEdit) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCleanSourceText(kTwoRectSvg);

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  const std::size_t closeOffset = textEditor.getText().find("</svg>");
  ASSERT_NE(closeOffset, std::string::npos);
  textEditor.setCursorPosition(textEditor.getCoordinatesAtByteOffset(closeOffset));
  textEditor.insertText("<!-- local tail -->");
  textEditor.resetTextChanged();

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  xml::ApplySourceEditResult result =
      app.document().document().setElementAttribute(*rect, "fill", "green");
  ASSERT_TRUE(result.applied);
  ASSERT_FALSE(result.sourceDeltas.empty());

  EXPECT_TRUE(controller.mirrorSourceDeltas(app, textEditor, result.sourceDeltas));

  EXPECT_NE(textEditor.getText().find("<!-- local tail -->"), std::string::npos);
  EXPECT_NE(textEditor.getText().find(R"(fill="green")"), std::string::npos);
  EXPECT_FALSE(textEditor.isTextChanged());
  EXPECT_FALSE(app.document().queue().empty());
}

TEST(DocumentSyncControllerStructuredTest, MirrorSourceDeltasMapsAfterLocalEdit) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCleanSourceText(kTwoRectSvg);

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  const std::size_t firstRectOffset = textEditor.getText().find("<rect");
  ASSERT_NE(firstRectOffset, std::string::npos);
  textEditor.setCursorPosition(textEditor.getCoordinatesAtByteOffset(firstRectOffset));
  textEditor.insertText("<!-- local head -->");
  textEditor.resetTextChanged();

  auto rect = app.document().document().querySelector("#r2");
  ASSERT_TRUE(rect.has_value());
  xml::ApplySourceEditResult result =
      app.document().document().setElementAttribute(*rect, "fill", "blue");
  ASSERT_TRUE(result.applied);
  ASSERT_FALSE(result.sourceDeltas.empty());

  EXPECT_TRUE(controller.mirrorSourceDeltas(app, textEditor, result.sourceDeltas));

  EXPECT_NE(textEditor.getText().find("<!-- local head -->"), std::string::npos);
  const std::size_t r2Offset = textEditor.getText().find(R"(id="r2")");
  ASSERT_NE(r2Offset, std::string::npos);
  EXPECT_NE(textEditor.getText().find(R"(fill="blue")", r2Offset), std::string::npos);
  EXPECT_FALSE(textEditor.isTextChanged());
  EXPECT_FALSE(app.document().queue().empty());
}

TEST(DocumentSyncControllerStructuredTest, MirrorSourceDeltasFallsBackWhenDeleteOverlapsLocalEdit) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCleanSourceText(kTwoRectSvg);

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  const std::size_t widthOffset = textEditor.getText().find(R"(width="10")");
  ASSERT_NE(widthOffset, std::string::npos);
  textEditor.setSelection(textEditor.getCoordinatesAtByteOffset(widthOffset),
                          textEditor.getCoordinatesAtByteOffset(
                              widthOffset + std::string_view(R"(width="10")").size()));
  textEditor.insertText(R"(width="11")");
  textEditor.resetTextChanged();

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  xml::ApplySourceEditResult result = app.document().document().removeElement(*rect);
  ASSERT_TRUE(result.applied);
  ASSERT_EQ(result.sourceDeltas.size(), 1u);
  ASSERT_GT(result.sourceDeltas.front().removedLength, 0u);

  EXPECT_TRUE(controller.mirrorSourceDeltas(app, textEditor, result.sourceDeltas));

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_EQ(textEditor.getText().find("id=\"r1\""), std::string::npos);
  EXPECT_FALSE(textEditor.isTextChanged());
}

TEST(DocumentSyncControllerStructuredTest, MirrorSourceDeltasFallsBackWhenInsertionContextIsGone) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCleanSourceText(kTwoRectSvg);

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  // The append lands on its own line right after the last child, so delete that whole tail
  // locally (the last <rect> through </svg>). The insertion context is now gone, and the
  // mirror must fall back to a full resync rather than patch into a stale offset.
  const std::size_t lastRectOffset = textEditor.getText().rfind("<rect");
  ASSERT_NE(lastRectOffset, std::string::npos);
  textEditor.setSelection(textEditor.getCoordinatesAtByteOffset(lastRectOffset),
                          textEditor.getCoordinatesAtByteOffset(textEditor.getText().size()));
  textEditor.insertText("");
  textEditor.resetTextChanged();

  svg::SVGRectElement inserted = svg::SVGRectElement::Create(app.document().document());
  inserted.setAttribute("id", "inserted");
  inserted.setAttribute("width", "5");
  inserted.setAttribute("height", "6");
  xml::ApplySourceEditResult result =
      app.document().document().insertElement(app.document().document().svgElement(), inserted);
  ASSERT_TRUE(result.applied);
  ASSERT_EQ(result.sourceDeltas.size(), 1u);
  ASSERT_EQ(result.sourceDeltas.front().removedLength, 0u);

  EXPECT_TRUE(controller.mirrorSourceDeltas(app, textEditor, result.sourceDeltas));

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_NE(textEditor.getText().find("</svg>"), std::string::npos);
  EXPECT_NE(textEditor.getText().find(R"(id="inserted")"), std::string::npos);
}

TEST(DocumentSyncControllerStructuredTest, MirrorSourceDeltasInsertsNewRootChildIntoLocalText) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCleanSourceText(kTwoRectSvg);

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  const std::size_t closeOffset = textEditor.getText().find("</svg>");
  ASSERT_NE(closeOffset, std::string::npos);
  textEditor.setCursorPosition(textEditor.getCoordinatesAtByteOffset(closeOffset));
  textEditor.insertText("<!-- local -->\n");
  textEditor.resetTextChanged();

  svg::SVGRectElement inserted = svg::SVGRectElement::Create(app.document().document());
  inserted.setAttribute("id", "inserted");
  inserted.setAttribute("width", "5");
  inserted.setAttribute("height", "6");
  xml::ApplySourceEditResult result =
      app.document().document().insertElement(app.document().document().svgElement(), inserted);
  ASSERT_TRUE(result.applied);
  ASSERT_EQ(result.sourceDeltas.size(), 1u);
  ASSERT_EQ(result.sourceDeltas.front().removedLength, 0u);
  ASSERT_GT(result.sourceDeltas.front().insertedLength, 0u);

  EXPECT_TRUE(controller.mirrorSourceDeltas(app, textEditor, result.sourceDeltas));

  EXPECT_NE(textEditor.getText().find("<!-- local -->"), std::string::npos);
  EXPECT_NE(textEditor.getText().find(R"(id="inserted")"), std::string::npos);
  EXPECT_LT(textEditor.getText().find("<!-- local -->"), textEditor.getText().find("</svg>"));
  EXPECT_FALSE(textEditor.isTextChanged());
}

TEST(DocumentSyncControllerStructuredTest, InvalidSourceBackedTransformWritebackMirrorsSource) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setCleanSourceText(kTwoRectSvg);

  TextEditor textEditor;
  textEditor.setText("stale text");
  textEditor.resetTextChanged();
  SelectTool tool;
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = AttributeWritebackTarget{},
      .transform = Transform2d::Translate(Vector2d(5.0, 0.0)),
  });

  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_FALSE(textEditor.isTextChanged());
}

TEST(DocumentSyncControllerStructuredTest, SourceLessElementRemoveWritebackPatchesText) {
  const auto makeRectTarget = [](std::size_t rectIndex, RcString id) {
    return AttributeWritebackTarget{
        .elementPath =
            {
                AttributeWritebackPathSegment{0, xml::XMLQualifiedName(RcString("svg"))},
                AttributeWritebackPathSegment{rectIndex, xml::XMLQualifiedName(RcString("rect"))},
            },
        .elementId = std::move(id),
    };
  };

  EditorApp app;
  svg::SVGDocument document;
  app.document().setDocument(std::move(document));
  ASSERT_TRUE(app.hasDocument());
  ASSERT_FALSE(app.document().document().hasSourceStore());

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  SelectTool tool;
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  app.enqueueElementRemoveWriteback(EditorApp::CompletedElementRemoveWriteback{
      .target = AttributeWritebackTarget{},
  });
  app.enqueueElementRemoveWriteback(EditorApp::CompletedElementRemoveWriteback{
      .target = makeRectTarget(0, RcString("r1")),
  });

  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_EQ(textEditor.getText().find(R"(id="r1")"), std::string::npos);
  EXPECT_NE(textEditor.getText().find(R"(id="r2")"), std::string::npos);
  EXPECT_TRUE(textEditor.isTextChanged());
  EXPECT_FALSE(app.document().queue().empty());
}

TEST(DocumentSyncControllerStructuredTest, SourceLessInvalidTransformWritebackDrainsWithoutPatch) {
  EditorApp app;
  svg::SVGDocument document;
  app.document().setDocument(std::move(document));
  ASSERT_TRUE(app.hasDocument());
  ASSERT_FALSE(app.document().document().hasSourceStore());

  TextEditor textEditor;
  textEditor.setText(kTwoRectSvg);
  textEditor.resetTextChanged();
  SelectTool tool;
  DocumentSyncController controller{std::string(kTwoRectSvg)};

  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = AttributeWritebackTarget{},
      .transform = Transform2d::Translate(Vector2d(8.0, 0.0)),
  });

  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_EQ(textEditor.getText(), std::string(kTwoRectSvg));
  EXPECT_FALSE(textEditor.isTextChanged());
  EXPECT_TRUE(app.document().queue().empty());
}

TEST(DocumentSyncControllerStructuredTest, ProgrammaticDocumentTransformWritebacksPatchText) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect id="r1" width="10" height="10"/><rect id="r2" width="10" height="10" transform="translate(2)"/><rect id="r3" width="10" height="10" transform="translate(3)"/><rect id="r4" width="10" height="10"/></svg>)svg";

  svg::SVGDocument document;
  const auto makeRectTarget = [](std::size_t rectIndex, RcString id) {
    return AttributeWritebackTarget{
        .elementPath =
            {
                AttributeWritebackPathSegment{0, xml::XMLQualifiedName(RcString("svg"))},
                AttributeWritebackPathSegment{rectIndex, xml::XMLQualifiedName(RcString("rect"))},
            },
        .elementId = std::move(id),
    };
  };
  AttributeWritebackTarget r1Target = makeRectTarget(0, RcString("r1"));
  AttributeWritebackTarget r2Target = makeRectTarget(1, RcString("r2"));
  AttributeWritebackTarget r3Target = makeRectTarget(2, RcString("r3"));
  AttributeWritebackTarget r4Target = makeRectTarget(3, RcString("r4"));

  EditorApp app;
  app.document().setDocument(std::move(document));
  ASSERT_TRUE(app.hasDocument());
  ASSERT_FALSE(app.document().document().hasSourceStore());
  app.setCleanSourceText(kSvg);

  TextEditor textEditor;
  textEditor.setText(kSvg);
  textEditor.resetTextChanged();
  SelectTool tool;
  DocumentSyncController controller{std::string(kSvg)};

  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = r1Target,
      .transform = Transform2d::Translate(Vector2d(6.0, 0.0)),
      .sourceTransformAttributeValue = RcString("translate(9)"),
      .restoreSourceTransformAttributeValue = true,
  });
  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = r2Target,
      .transform = Transform2d::Translate(Vector2d(6.0, 0.0)),
      .restoreSourceTransformAttributeValue = true,
  });
  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = r3Target,
      .transform = Transform2d(),
  });
  app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
      .target = r4Target,
      .transform = Transform2d::Translate(Vector2d(8.0, 0.0)),
  });

  controller.applyPendingWritebacks(app, tool, textEditor);

  EXPECT_NE(textEditor.getText().find("id=\"r1\""), std::string::npos);
  EXPECT_NE(textEditor.getText().find("transform=\"translate(9)\""), std::string::npos);
  EXPECT_NE(textEditor.getText().find(R"(<rect id="r2" width="10" height="10"/>)"),
            std::string::npos);
  EXPECT_NE(textEditor.getText().find(R"(<rect id="r3" width="10" height="10"/>)"),
            std::string::npos);
  const std::size_t r4Offset = textEditor.getText().find(R"(id="r4")");
  ASSERT_NE(r4Offset, std::string::npos);
  EXPECT_NE(textEditor.getText().find("transform=\"translate(8)\"", r4Offset), std::string::npos);
  EXPECT_FALSE(app.document().queue().empty());

  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(app.document().document().hasSourceStore());
  auto parsedR1 = app.document().document().querySelector("#r1");
  auto parsedR2 = app.document().document().querySelector("#r2");
  auto parsedR3 = app.document().document().querySelector("#r3");
  auto parsedR4 = app.document().document().querySelector("#r4");
  ASSERT_TRUE(parsedR1.has_value());
  ASSERT_TRUE(parsedR2.has_value());
  ASSERT_TRUE(parsedR3.has_value());
  ASSERT_TRUE(parsedR4.has_value());
  EXPECT_EQ(parsedR1->getAttribute("transform"), std::optional<RcString>(RcString("translate(9)")));
  EXPECT_FALSE(parsedR2->getAttribute("transform").has_value());
  EXPECT_FALSE(parsedR3->getAttribute("transform").has_value());
  EXPECT_EQ(parsedR4->getAttribute("transform"), std::optional<RcString>(RcString("translate(8)")));
}

}  // namespace
}  // namespace donner::editor
