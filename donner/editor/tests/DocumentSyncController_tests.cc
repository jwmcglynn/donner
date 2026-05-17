#include "donner/editor/DocumentSyncController.h"

#include <gtest/gtest.h>

#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TextEditor.h"

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

  tool.onMouseDown(app_, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app_, Vector2d(25.0, 15.0), /*buttonHeld=*/true);
  tool.onMouseUp(app_, Vector2d(25.0, 15.0));
  ASSERT_TRUE(app_.flushFrame());

  controller_.applyPendingWritebacks(app_, tool, textEditor_);

  EXPECT_EQ(textEditor_.getText(), app_.document().document().source());
  EXPECT_FALSE(textEditor_.isTextChanged());
  EXPECT_TRUE(app_.document().queue().empty());
}

TEST_F(DocumentSyncControllerTest, SourceBackedDeleteWritebackMirrorsFlushDeltaWithoutTextEcho) {
  SelectTool tool;

  controller_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/0.0f);
  ASSERT_FALSE(textEditor_.isTextChanged());

  std::optional<svg::SVGElement> rect = app_.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  std::optional<AttributeWritebackTarget> target = captureAttributeWritebackTarget(*rect);
  ASSERT_TRUE(target.has_value());

  app_.enqueueElementRemoveWriteback(EditorApp::CompletedElementRemoveWriteback{.target = *target});
  app_.applyMutation(EditorCommand::DeleteElementCommand(*rect));
  ASSERT_TRUE(app_.flushFrame());
  ASSERT_EQ(app_.document().lastFlushResult().sourceDeltas.size(), 1u);

  controller_.applyPendingWritebacks(app_, tool, textEditor_);

  EXPECT_EQ(textEditor_.getText(), app_.document().document().source());
  EXPECT_FALSE(textEditor_.isTextChanged());
  EXPECT_TRUE(app_.document().queue().empty());
  EXPECT_FALSE(app_.document().document().querySelector("#r1").has_value());
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

}  // namespace
}  // namespace donner::editor
