#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>

#include "donner/base/tests/RunfileGate.h"
#include "donner/editor/EditorShell.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/gui/EditorWindow.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "imgui_internal.h"
#include "nlohmann/json.hpp"

namespace donner::editor {
class EditorCollaborationUiTestAccess {
public:
  static void OpenContextMenu(EditorShell& shell, Vector2d point) {
    shell.openRenderPaneContextMenu(point);
  }
  static bool PresentedCurrentDocument(EditorShell& shell) {
    // Background refinement may still be running after the current document is presented.
    return shell.renderCoordinator_.compositedPresentation().hasCachedTextures() &&
           shell.renderCoordinator_.displayedDocVersionForDiagnostics() >=
               shell.app_.document().currentFrameVersion();
  }
  static nlohmann::json Readiness(EditorShell& shell) {
    return {{"busy", shell.renderCoordinator_.asyncRenderer().isBusy()},
            {"dragging", shell.selectTool_.isDragging()},
            {"drafting", shell.penTool_.isDrafting()},
            {"writebacks", shell.documentSyncController_.hasPendingWritebacks()},
            {"text_changed", shell.textEditor_.isTextChanged()},
            {"ready", shell.collaborationFrameReady()},
            {"preview_scale", shell.renderCoordinator_.previewRasterScale()},
            {"preview_blocked", shell.renderCoordinator_.previewRenderingBlocked()},
            {"cached", shell.renderCoordinator_.compositedPresentation().hasCachedTextures()},
            {"displayed", shell.renderCoordinator_.displayedDocVersionForDiagnostics()},
            {"current", shell.app_.document().currentFrameVersion()}};
  }
  static void LimitSurfaceBytes(EditorShell& shell, std::uint64_t bytes) {
    ASSERT_THAT(shell.renderCoordinator_.asyncRenderer().isBusy(), ::testing::Eq(false));
    shell.renderCoordinator_.renderer().setSurfaceBudgetForTesting(256, bytes);
  }
  static double PreviewScale(EditorShell& shell) {
    return shell.renderCoordinator_.previewRasterScale();
  }
  static bool PreviewBlocked(EditorShell& shell) {
    return shell.renderCoordinator_.previewRenderingBlocked();
  }
  static bool PinCaptures(EditorShell& shell, Vector2d point) {
    return shell.commentsPresenter_.capturesInput(point);
  }
};

namespace {
using Json = nlohmann::json;
using testing::Eq;
using testing::HasSubstr;

class EditorCollaborationUiTest : public testing::Test {
protected:
  std::filesystem::path directory;
  std::string endpoint;
  std::unique_ptr<gui::EditorWindow> window;
  std::unique_ptr<EditorShell> shell;
  std::uint64_t nextId = 1;

  virtual gui::EditorWindowOptions windowOptions() {
    return {.title = "Collaboration UI test",
            .initialWidth = 900,
            .initialHeight = 650,
            .visible = false,
            .offscreen = true,
            .forceOffscreenRenderTarget = true,
            .enableFramebufferReadback = true};
  }
  virtual std::string initialSource() {
    return R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="160"><rect id="face" x="10" y="10" width="150" height="120" fill="red"/></svg>)";
  }
  void SetUp() override {
    char pattern[] = "/tmp/donner-collaboration-ui-XXXXXX";
    char* created = mkdtemp(pattern);
    ASSERT_THAT(created != nullptr, Eq(true));
    directory = created;
    endpoint = (directory / "editor.sock").string();
    window = std::make_unique<gui::EditorWindow>(windowOptions());
    ASSERT_THAT(window->valid(), Eq(true));
    shell = std::make_unique<EditorShell>(
        *window, EditorShellOptions{.initialSource = initialSource(),
                                    .initialPath = (directory / "art.svg").string(),
                                    .allowFileSystemActions = false,
                                    .allowHostClipboardAccess = false,
                                    .controlSocketPath = endpoint});
    ASSERT_THAT(shell->valid(), Eq(true));
  }
  void TearDown() override {
    shell.reset();
    window.reset();
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  void frame() {
    window->pollEvents();
    shell->prepareFrame();
    window->beginFrame();
    shell->runFrame();
    window->endFrame();
  }
  Json exchange(Json request) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return Json::object();
    timeval timeout{.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      close(fd);
      return Json::object();
    }
    const std::string data = request.dump() + "\n";
    if (send(fd, data.data(), data.size(), 0) != static_cast<ssize_t>(data.size())) {
      close(fd);
      return Json::object();
    }
    std::string bytes;
    char buffer[8192];
    while (bytes.size() < 1024 * 1024 && bytes.find('\n') == std::string::npos) {
      const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
      if (count <= 0) break;
      bytes.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    Json parsed = Json::parse(bytes, nullptr, false);
    return parsed.is_object() ? parsed : Json::object();
  }
  Json call(std::string_view name, Json args = Json::object()) {
    const Json request{{"jsonrpc", "2.0"},
                       {"id", nextId++},
                       {"method", "tools/call"},
                       {"params", {{"name", name}, {"arguments", args}}}};
    auto reply = std::async(std::launch::async, [this, request]() { return exchange(request); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (reply.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
      window->waitEventsTimeout(0.01);
      frame();
    }
    EXPECT_THAT(reply.wait_for(std::chrono::seconds(0)), Eq(std::future_status::ready));
    const Json result = reply.get();
    if (!result.contains("result")) {
      ADD_FAILURE() << result.dump()
                    << " readiness=" << EditorCollaborationUiTestAccess::Readiness(*shell).dump();
      return Json::object();
    }
    EXPECT_THAT(result["result"]["isError"], Eq(false)) << result.dump();
    return Json::parse(result["result"]["content"][0]["text"].get<std::string>());
  }
  svg::RendererBitmap captureDocumentPixel(Vector2d point) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!EditorCollaborationUiTestAccess::PresentedCurrentDocument(*shell) &&
           std::chrono::steady_clock::now() < deadline) {
      window->waitEventsTimeout(0.01);
      frame();
    }
    EXPECT_THAT(EditorCollaborationUiTestAccess::PresentedCurrentDocument(*shell), Eq(true))
        << EditorCollaborationUiTestAccess::Readiness(*shell).dump();
    window->beginFrame();
    shell->runFrame();
    const auto bitmap = window->endFrameAndReadPixels();
    const auto logical = window->windowSize();
    if (bitmap.empty() || logical.x <= 0 || logical.y <= 0) return {};
    const auto screen = shell->viewportForReadback().documentToScreen(point);
    const int x = static_cast<int>(std::lround(screen.x * bitmap.dimensions.x / logical.x));
    const int y = static_cast<int>(std::lround(screen.y * bitmap.dimensions.y / logical.y));
    if (x < 0 || y < 0 || x >= bitmap.dimensions.x || y >= bitmap.dimensions.y) return {};
    const std::size_t offset =
        static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4;
    svg::RendererBitmap pixel;
    pixel.dimensions = Vector2i(1, 1);
    pixel.rowBytes = 4;
    pixel.pixels.assign(bitmap.pixels.begin() + offset, bitmap.pixels.begin() + offset + 4);
    return pixel;
  }
  void addComment(Vector2d point, const char* text) {
    EditorCollaborationUiTestAccess::OpenContextMenu(*shell, point);
    frame();
    ASSERT_THAT(GImGui->OpenPopupStack.empty(), Eq(false));
    ImGuiWindow* menu = GImGui->OpenPopupStack.back().Window;
    ASSERT_THAT(menu != nullptr, Eq(true));
    ImGui::ActivateItemByID(menu->GetID("Add Comment Here"));
    frame();
    frame();
    ImGuiWindow* comments = ImGui::FindWindowByName("Comments");
    ASSERT_THAT(comments != nullptr, Eq(true));
    ImGui::GetIO().AddInputCharactersUTF8(text);
    frame();
    ImGui::ActivateItemByID(comments->GetID("Add Comment"));
    frame();
  }
  Json revision() {
    Json result = call("get_editor_state");
    return {{"session_id", result["session_id"]},
            {"document_generation", result["document_generation"]},
            {"source_revision", result["source_revision"]}};
  }
};

TEST_F(EditorCollaborationUiTest, PendingCommentMarkerTracksViewportAndClearsOnCancel) {
  const Vector2d point(80, 60);
  const auto original = captureDocumentPixel(point);
  ASSERT_THAT(original.empty(), Eq(false));
  call("get_svg_source");
  EditorCollaborationUiTestAccess::OpenContextMenu(*shell, point);
  frame();
  ASSERT_THAT(GImGui->OpenPopupStack.empty(), Eq(false));
  ImGuiWindow* menu = GImGui->OpenPopupStack.back().Window;
  ASSERT_THAT(menu != nullptr, Eq(true));
  ImGui::ActivateItemByID(menu->GetID("Add Comment Here"));
  frame();
  frame();
  svg::RendererBitmap marker;
  marker.dimensions = Vector2i(1, 1);
  marker.rowBytes = 4;
  marker.pixels = {114, 202, 255, 255};
  tests::CompareBitmapToBitmap(captureDocumentPixel(point), marker, "pending_comment_marker",
                               tests::PixelmatchIdentityParams());
  auto viewport = shell->viewportForReadback();
  viewport.panBy(Vector2d(24, 16));
  shell->overrideViewportForReplay(viewport);
  frame();
  tests::CompareBitmapToBitmap(captureDocumentPixel(point), marker, "panned_comment_marker",
                               tests::PixelmatchIdentityParams());
  ImGuiWindow* comments = ImGui::FindWindowByName("Comments");
  ASSERT_THAT(comments != nullptr, Eq(true));
  ImGui::ActivateItemByID(comments->GetID("Cancel"));
  frame();
  tests::CompareBitmapToBitmap(captureDocumentPixel(point), original, "cancelled_comment_marker",
                               tests::PixelmatchIdentityParams());
}

class PolygonClipCollaborationUiTest : public EditorCollaborationUiTest {
protected:
  std::string initialSource() override {
    return R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="160">
      <defs id="definitions"/><rect width="200" height="160" fill="white"/>
      <rect id="face" x="10" y="10" width="150" height="120" fill="red"/>
    </svg>)";
  }
};

TEST_F(PolygonClipCollaborationUiTest, GenericPointsEditsRenderLivePolygonClips) {
  const auto red = captureDocumentPixel(Vector2d(50, 50));
  const auto white = captureDocumentPixel(Vector2d(180, 140));
  ASSERT_THAT(red.empty(), Eq(false));
  ASSERT_THAT(white.empty(), Eq(false));
  auto args = revision();
  args.update({{"parent", "#definitions"}, {"tag", "clipPath"}, {"attributes", {{"id", "clip"}}}});
  call("insert_element", args);
  args = revision();
  args.update({{"parent", "#clip"},
               {"tag", "polygon"},
               {"attributes", {{"id", "clip-shape"}, {"points", "0,0 200,0 200,160 0,160"}}}});
  call("insert_element", args);
  args = revision();
  args["edits"] = {{{"selector", "#face"}, {"attribute", "clip-path"}, {"value", "url(#clip)"}}};
  call("apply_edits", args);
  tests::CompareBitmapToBitmap(captureDocumentPixel(Vector2d(50, 50)), red,
                               "native_polygon_clip_insert", tests::PixelmatchIdentityParams());
  args = revision();
  args["edits"] = {
      {{"selector", "#clip-shape"}, {"attribute", "points"}, {"value", "0,0 20,0 0,20"}}};
  call("apply_edits", args);
  tests::CompareBitmapToBitmap(captureDocumentPixel(Vector2d(50, 50)), white,
                               "native_polygon_clip_edit", tests::PixelmatchIdentityParams());
  args = revision();
  args["edits"][0]["selector"] = "#clip-shape";
  args["edits"][0]["attribute"] = "points";
  args["edits"][0]["value"] = "0,0 200,0 200,160 0,160";
  call("apply_edits", args);
  tests::CompareBitmapToBitmap(captureDocumentPixel(Vector2d(50, 50)), red,
                               "native_polygon_clip_restore", tests::PixelmatchIdentityParams());
}

class GeodeSplashCollaborationUiTest : public EditorCollaborationUiTest {
protected:
  gui::EditorWindowOptions windowOptions() override {
    auto options = EditorCollaborationUiTest::windowOptions();
    options.initialWidth = 2400;
    options.initialHeight = 1600;
    return options;
  }
  std::string initialSource() override {
    const auto splash = donner::tests::ReadRequiredRunfile("geode_splash.svg");
    EXPECT_THAT(splash.ok(), Eq(true)) << splash.error;
    return splash.contents;
  }
};

class BudgetedGeodeCollaborationUiTest : public GeodeSplashCollaborationUiTest {
protected:
  void SetUp() override {
    GeodeSplashCollaborationUiTest::SetUp();
    ASSERT_THAT(shell != nullptr, Eq(true));
    EditorCollaborationUiTestAccess::LimitSurfaceBytes(*shell, 8u * 1024u * 1024u);
  }
};

TEST_F(BudgetedGeodeCollaborationUiTest, BudgetRecoveryPreservesArtworkAndRepeatedComments) {
  const std::string before = call("get_svg_source")["source"].get<std::string>();
  {
    SCOPED_TRACE("initial budget recovery");
    (void)captureDocumentPixel(Vector2d(544, 500));
  }
  EXPECT_THAT(EditorCollaborationUiTestAccess::PreviewScale(*shell), ::testing::Lt(1.0));
  shell->queueDocumentSpaceReplayInputForTesting(
      {.documentPoint = Vector2d(544, 500), .leftMouseDown = true, .leftMousePressed = true});
  frame();
  shell->queueDocumentSpaceReplayInputForTesting(
      {.documentPoint = Vector2d(544, 500), .leftMouseReleased = true});
  frame();
  call("get_editor_state");
  {
    SCOPED_TRACE("after shape click");
    (void)captureDocumentPixel(Vector2d(544, 500));
  }
  EXPECT_THAT(EditorCollaborationUiTestAccess::PreviewBlocked(*shell), Eq(false));
  addComment(Vector2d(544, 500), "Refine this crystal");
  addComment(Vector2d(500, 400), "Refine this edge");
  EXPECT_THAT(call("get_comments")["comments"].size(), Eq(2u));
  EXPECT_THAT(call("get_svg_source")["source"].get<std::string>(), Eq(before));
  window->beginFrame();
  shell->runFrame();
  const auto bitmap = window->endFrameAndReadPixels();
  ASSERT_THAT(bitmap.empty(), Eq(false));
  const char* output = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  ASSERT_THAT(output != nullptr, Eq(true));
  const std::string path = (std::filesystem::path(output) / "native-budget-recovery.png").string();
  EXPECT_THAT(svg::RendererImageIO::writeRgbaPixelsToPngFile(
                  path.c_str(), bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
                  bitmap.rowBytes / 4),
              Eq(true));
}

TEST_F(GeodeSplashCollaborationUiTest, PresentsTheFullArtworkAndRespondsToMcp) {
  std::cerr << "Startup window=" << window->windowSize().x << "x" << window->windowSize().y
            << " displayScale=" << window->displayScale() << "\n";
  EXPECT_THAT(call("get_editor_state")["has_document"], Eq(true));
  EXPECT_THAT(call("get_svg_source")["source"].get<std::string>(), HasSubstr("background-glow"));
  (void)captureDocumentPixel(Vector2d(100, 100));
  window->beginFrame();
  shell->runFrame();
  const auto bitmap = window->endFrameAndReadPixels();
  ASSERT_THAT(bitmap.empty(), Eq(false));
  const char* output = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  ASSERT_THAT(output != nullptr, Eq(true));
  std::cerr << "Startup framebuffer=" << bitmap.dimensions.x << "x" << bitmap.dimensions.y << "\n";
  const std::string path = (std::filesystem::path(output) / "native-geode-splash.png").string();
  EXPECT_THAT(svg::RendererImageIO::writeRgbaPixelsToPngFile(
                  path.c_str(), bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
                  bitmap.rowBytes / 4),
              Eq(true));
}

TEST_F(GeodeSplashCollaborationUiTest, ClickingMaskedCavityKeepsTheNativeEditorResponsive) {
  call("get_editor_state");
  shell->queueDocumentSpaceReplayInputForTesting(
      {.documentPoint = Vector2d(544, 500), .leftMouseDown = true, .leftMousePressed = true});
  frame();
  shell->queueDocumentSpaceReplayInputForTesting(
      {.documentPoint = Vector2d(544, 500), .leftMouseReleased = true});
  frame();
  EXPECT_THAT(call("get_editor_state")["selection_count"], Eq(1));
}

TEST_F(EditorCollaborationUiTest, McpEditsTheVisibleDocumentAndUndoRestoresIt) {
  Json edit = revision();
  edit["edits"] = {{{"selector", "#face"}, {"attribute", "fill"}, {"value", "blue"}}};
  call("apply_edits", edit);
  ASSERT_THAT(shell->documentSourceForReadback().has_value(), Eq(true));
  EXPECT_THAT(*shell->documentSourceForReadback(), HasSubstr("fill=\"blue\""));
  svg::RendererBitmap expected;
  expected.dimensions = Vector2i(1, 1);
  expected.rowBytes = 4;
  expected.pixels = {0, 0, 255, 255};
  tests::CompareBitmapToBitmap(captureDocumentPixel(Vector2d(40, 40)), expected,
                               "native_mcp_blue_face", tests::PixelmatchIdentityParams());
  call("select_by_selector", {{"selector", "#face"}});
  EXPECT_THAT(call("get_editor_state")["selection"][0]["id"], Eq("face"));
  call("undo", revision());
  EXPECT_THAT(*shell->documentSourceForReadback(), HasSubstr("fill=\"red\""));
}

TEST_F(EditorCollaborationUiTest, ContextMenuCommentReachesMcpAndDrawsAnAnchoredPin) {
  call("get_editor_state");
  addComment(Vector2d(40, 40), "Make this face more angular");
  const Json feedback = call("get_comments");
  ASSERT_THAT(feedback["comments"].size(), Eq(1u)) << feedback.dump();
  EXPECT_THAT(feedback["comments"][0]["text"], Eq("Make this face more angular"));
  EXPECT_THAT(feedback["comments"][0]["element_id"], Eq("face"));
  EXPECT_THAT(feedback["comments"][0]["x"], Eq(40.0));
  addComment(Vector2d(100, 85), "Lighten this face");
  const Json secondFeedback = call("get_comments");
  ASSERT_THAT(secondFeedback["comments"].size(), Eq(2u));
  EXPECT_THAT(secondFeedback["comments"][1]["text"], Eq("Lighten this face"));
  EXPECT_THAT(secondFeedback["comments"][1]["x"], Eq(100.0));
  EXPECT_THAT(secondFeedback["comments"][1]["y"], Eq(85.0));

  frame();
  const auto screen = shell->viewportForReadback().documentToScreen(Vector2d(40, 40));
  EXPECT_THAT(EditorCollaborationUiTestAccess::PinCaptures(*shell, screen), Eq(true));
  window->beginFrame();
  shell->runFrame();
  const auto bitmap = window->endFrameAndReadPixels();
  ASSERT_THAT(bitmap.empty(), Eq(false));
  const char* output = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (output != nullptr) {
    const std::string path = (std::filesystem::path(output) / "native-comments.png").string();
    EXPECT_THAT(svg::RendererImageIO::writeRgbaPixelsToPngFile(
                    path.c_str(), bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
                    bitmap.rowBytes / 4),
                Eq(true));
  }
}
}  // namespace
}  // namespace donner::editor
