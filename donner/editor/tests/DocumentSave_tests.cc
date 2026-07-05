#include "donner/editor/DocumentSave.h"

#include <fcntl.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/tests/TestTempDir.h"
#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor {
namespace {

constexpr std::string_view kRoundTripSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
  <defs>
    <filter id="soft"><feGaussianBlur stdDeviation="1.5"/></filter>
  </defs>
  <rect id="drag" x="10" y="12" width="24" height="20" fill="#d62828"/>
  <circle id="accent" cx="64" cy="58" r="15" fill="#457b9d" filter="url(#soft)"/>
</svg>)svg";

std::filesystem::path TestOutputDir() {
  if (const char* outputDir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    return std::filesystem::path(outputDir);
  }
  return TestTempDir();
}

std::filesystem::path UniqueTestPath(std::string_view suffix) {
  const testing::TestInfo* testInfo = testing::UnitTest::GetInstance()->current_test_info();
  std::string filename = testInfo->test_suite_name();
  filename += "_";
  filename += testInfo->name();
  filename += "_";
  filename += suffix;
  return TestOutputDir() / filename;
}

Coordinates CoordinatesForOffset(std::string_view source, std::size_t offset) {
  int line = 0;
  std::size_t lineStart = 0;
  for (std::size_t i = 0; i < offset && i < source.size(); ++i) {
    if (source[i] == '\n') {
      ++line;
      lineStart = i + 1;
    }
  }
  return Coordinates(line, static_cast<int>(offset - lineStart));
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::string result;
  result.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return result;
}

svg::RendererBitmap Render(EditorApp& app) {
  svg::Renderer renderer;
  renderer.draw(app.document().document());
  return renderer.takeSnapshot();
}

void SaveCurrentDocument(EditorApp& app, const std::filesystem::path& path) {
  ASSERT_TRUE(app.hasDocument());
  ASSERT_TRUE(app.document().document().hasSourceStore());

  const DocumentSaveResult result = SaveSourceToPath(path, app.document().document().source());
  ASSERT_TRUE(result.ok()) << result.message;
}

void ExpectSavedRoundTripMatchesLive(EditorApp& app, const std::filesystem::path& path,
                                     std::string_view label) {
  SaveCurrentDocument(app, path);

  EditorApp reloadedApp;
  ASSERT_TRUE(reloadedApp.loadFromString(ReadFile(path)));

  tests::CompareBitmapToBitmap(Render(reloadedApp), Render(app), label);
}

class DocumentSaveRoundTripTest : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    imguiContext_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.Fonts->Build();
  }

  void TearDown() override {
    if (imguiContext_ != nullptr) {
      ImGui::DestroyContext(imguiContext_);
      imguiContext_ = nullptr;
    }
  }

private:
  ImGuiContext* imguiContext_ = nullptr;
};

TEST_F(DocumentSaveRoundTripTest, CanvasDragSaveReloadPreservesRenderedDocument) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kRoundTripSvg));
  app.setCleanSourceText(kRoundTripSvg);

  TextEditor textEditor;
  textEditor.setText(kRoundTripSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kRoundTripSvg)};
  SelectTool selectTool;

  selectTool.onMouseDown(app, Vector2d(18.0, 18.0), MouseModifiers{});
  selectTool.onMouseMove(app, Vector2d(38.0, 30.0), /*buttonHeld=*/true);
  selectTool.onMouseUp(app, Vector2d(38.0, 30.0));
  ASSERT_TRUE(app.flushFrame());

  controller.applyPendingWritebacks(app, selectTool, textEditor);
  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/1.0f);
  EXPECT_FALSE(app.flushFrame());
  ASSERT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_NE(app.document().document().source().find("translate("), std::string_view::npos);

  ExpectSavedRoundTripMatchesLive(app, UniqueTestPath("canvas_drag.svg"), "canvas_drag_roundtrip");
}

TEST_F(DocumentSaveRoundTripTest, TextEditSaveReloadPreservesRenderedDocument) {
  EditorApp app;
  app.setStructuredEditingEnabled(true);
  ASSERT_TRUE(app.loadFromString(kRoundTripSvg));
  app.setCleanSourceText(kRoundTripSvg);

  TextEditor textEditor;
  textEditor.setText(kRoundTripSvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kRoundTripSvg)};

  const std::size_t fillOffset = std::string_view(kRoundTripSvg).find("#d62828");
  ASSERT_NE(fillOffset, std::string_view::npos);
  textEditor.setSelection(CoordinatesForOffset(kRoundTripSvg, fillOffset),
                          CoordinatesForOffset(kRoundTripSvg, fillOffset + 7));
  textEditor.insertText("#2a9d8f");

  controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/1.0f);
  EXPECT_TRUE(app.document().queue().empty());
  EXPECT_FALSE(app.flushFrame());
  ASSERT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_NE(app.document().document().source().find("#2a9d8f"), std::string_view::npos);

  ExpectSavedRoundTripMatchesLive(app, UniqueTestPath("text_edit.svg"), "text_edit_roundtrip");
}

TEST(DocumentSaveTest, RejectsSymlinkDestinationWithoutTouchingTarget) {
#ifndef O_NOFOLLOW
  GTEST_SKIP() << "O_NOFOLLOW is unavailable on this platform";
#else
  const std::filesystem::path target = UniqueTestPath("target.svg");
  const std::filesystem::path symlink = UniqueTestPath("link.svg");
  std::filesystem::remove(target);
  std::filesystem::remove(symlink);

  constexpr std::string_view kOriginal = "<svg id=\"original\"/>";
  {
    std::ofstream file(target, std::ios::binary);
    ASSERT_TRUE(file.good());
    file << kOriginal;
  }

  std::error_code ec;
  std::filesystem::create_symlink(target, symlink, ec);
  if (ec) {
    GTEST_SKIP() << "Could not create symlink in test output dir: " << ec.message();
  }

  const DocumentSaveResult result = SaveSourceToPath(symlink, "<svg id=\"replaced\"/>");
  EXPECT_EQ(result.status, DocumentSaveStatus::OpenFailed);
  EXPECT_NE(result.errorNumber, 0);
  EXPECT_EQ(ReadFile(target), kOriginal);

  std::filesystem::remove(symlink);
  std::filesystem::remove(target);
#endif
}

TEST(DocumentSaveTest, StreamsStatusesAndResultOkFlag) {
  std::ostringstream statuses;
  statuses << DocumentSaveStatus::Ok << " " << DocumentSaveStatus::OpenFailed << " "
           << DocumentSaveStatus::WriteFailed << " " << DocumentSaveStatus::CloseFailed;

  EXPECT_EQ(statuses.str(), "Ok OpenFailed WriteFailed CloseFailed");
  EXPECT_TRUE(DocumentSaveResult{.status = DocumentSaveStatus::Ok}.ok());
  EXPECT_FALSE(DocumentSaveResult{.status = DocumentSaveStatus::OpenFailed}.ok());
}

TEST(DocumentSaveTest, CreatesAndTruncatesDestinationWithoutFollowingSymlinks) {
  const std::filesystem::path path = UniqueTestPath("created.svg");
  std::filesystem::remove(path);

  DocumentSaveResult result = SaveSourceToPath(path, "<svg id=\"first\"/>");
  ASSERT_TRUE(result.ok()) << result.message;
  EXPECT_EQ(result.bytesWritten, 17u);
  EXPECT_EQ(ReadFile(path), "<svg id=\"first\"/>");

  result = SaveSourceToPath(path, "");
  ASSERT_TRUE(result.ok()) << result.message;
  EXPECT_EQ(result.bytesWritten, 0u);
  EXPECT_EQ(ReadFile(path), "");

  std::filesystem::remove(path);
}

TEST(DocumentSaveTest, ReportsOpenFailureForDirectoryDestination) {
  const std::filesystem::path directory = TestOutputDir() / "document_save_directory_destination";
  std::filesystem::create_directories(directory);

  const DocumentSaveResult result = SaveSourceToPath(directory, "<svg/>");

  EXPECT_EQ(result.status, DocumentSaveStatus::OpenFailed);
  EXPECT_NE(result.errorNumber, 0);
  EXPECT_EQ(result.bytesWritten, 0u);
  EXPECT_NE(result.message.find("Could not open "), std::string::npos);
  EXPECT_NE(result.message.find(directory.string()), std::string::npos);

  std::filesystem::remove(directory);
}

}  // namespace
}  // namespace donner::editor
