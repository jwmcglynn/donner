#include "donner/editor/DocumentSyncController.h"

#include <gtest/gtest.h>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/backend_lib/SelectTool.h"

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
  ASSERT_TRUE(app_.flushFrame());
  ASSERT_TRUE(app_.isDirty());

  app_.undo();
  ASSERT_TRUE(app_.flushFrame());

  controller_.applyPendingWritebacks(app_, tool, textEditor_);
  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  EXPECT_FALSE(app_.isDirty());
}

TEST_F(DocumentSyncControllerTest, UndoToBaselineClearsDirtyFlagWhenSourceHasTrailingNewline) {
  // Regression: donner_splash.svg ships with a trailing '\n'. `TextBuffer`
  // canonicalizes lines when text is round-tripped through it, dropping that
  // trailing newline. If the clean baseline is stored verbatim it never equals
  // the text editor's canonical form, so `syncDirtyFromSource` latches the
  // dirty flag on forever — including after an undo that restores the source.
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
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(app.isDirty());

  app.undo();
  ASSERT_TRUE(app.flushFrame());

  controller.applyPendingWritebacks(app, tool, textEditor);
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);
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
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(app.isDirty());

  app.undo();
  ASSERT_TRUE(app.flushFrame());

  controller.applyPendingWritebacks(app, tool, textEditor);
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);
  EXPECT_EQ(textEditor.getText(), std::string(kNonCanonicalTransformSvg));
  EXPECT_FALSE(app.isDirty());
}

}  // namespace
}  // namespace donner::editor
