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
    return !shell.renderCoordinator_.asyncRenderer().isBusy() &&
           shell.renderCoordinator_.displayedDocVersionForDiagnostics() >=
               shell.app_.document().currentFrameVersion();
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
    for (int i = 0;
         i < 100 && reply.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
         ++i) {
      window->waitEventsTimeout(0.01);
      frame();
    }
    EXPECT_THAT(reply.wait_for(std::chrono::seconds(0)), Eq(std::future_status::ready));
    const Json result = reply.get();
    if (!result.contains("result")) {
      ADD_FAILURE() << result.dump();
      return Json::object();
    }
    EXPECT_THAT(result["result"]["isError"], Eq(false)) << result.dump();
    return Json::parse(result["result"]["content"][0]["text"].get<std::string>());
  }
  svg::RendererBitmap captureDocumentPixel(Vector2d point) {
    for (int i = 0; i < 100 && !EditorCollaborationUiTestAccess::PresentedCurrentDocument(*shell);
         ++i) {
      window->waitEventsTimeout(0.01);
      frame();
    }
    EXPECT_THAT(EditorCollaborationUiTestAccess::PresentedCurrentDocument(*shell), Eq(true));
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
  Json revision() {
    Json result = call("get_editor_state");
    return {{"session_id", result["session_id"]},
            {"document_generation", result["document_generation"]},
            {"source_revision", result["source_revision"]}};
  }
};

class GeodeSplashCollaborationUiTest : public EditorCollaborationUiTest {
protected:
  gui::EditorWindowOptions windowOptions() override {
    auto options = EditorCollaborationUiTest::windowOptions();
    options.initialWidth = 3200;
    options.initialHeight = 1800;
    options.offscreenContentScale = 2.0;
    return options;
  }
  std::string initialSource() override {
    const auto splash = donner::tests::ReadRequiredRunfile("geode_splash.svg");
    EXPECT_THAT(splash.ok(), Eq(true)) << splash.error;
    return splash.contents;
  }
};

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
  EditorCollaborationUiTestAccess::OpenContextMenu(*shell, Vector2d(40, 40));
  frame();
  ASSERT_THAT(GImGui->OpenPopupStack.empty(), Eq(false));
  ImGuiWindow* menu = GImGui->OpenPopupStack.back().Window;
  ASSERT_THAT(menu != nullptr, Eq(true));
  ImGui::ActivateItemByID(menu->GetID("Add Comment Here"));
  frame();
  frame();
  ImGuiWindow* comments = ImGui::FindWindowByName("Comments");
  ASSERT_THAT(comments != nullptr, Eq(true));
  ImGui::GetIO().AddInputCharactersUTF8("Make this face more angular");
  frame();
  ImGui::ActivateItemByID(comments->GetID("Add Comment"));
  frame();
  const Json feedback = call("get_comments");
  ASSERT_THAT(feedback["comments"].size(), Eq(1u)) << feedback.dump();
  EXPECT_THAT(feedback["comments"][0]["text"], Eq("Make this face more angular"));
  EXPECT_THAT(feedback["comments"][0]["element_id"], Eq("face"));
  EXPECT_THAT(feedback["comments"][0]["x"], Eq(40.0));
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
