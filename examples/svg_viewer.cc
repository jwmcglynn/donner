/**
 * @example svg_viewer.cc Interactive SVG viewer with the donner editor's
 * text-pane TextEditor wired to typing → full re-parse rendering.
 *
 * Always-on smoke test for `//donner/editor:text_editor` +
 * `//donner/editor:async_svg_document` + `donner::svg::Renderer` integration.
 * Loads an SVG from a CLI arg, displays it, and offers a `TextEditor` pane
 * that re-parses the document on every text change via the new
 * `EditorCommand::ReplaceDocument` command-queue path.
 *
 * To run:
 *
 * ```sh
 * bazel run //examples:svg_viewer -- donner_splash.svg
 * ```
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TextEditor.h"
#include "donner/svg/renderer/Renderer.h"

#include "glad/glad.h"

extern "C" {
#include "GLFW/glfw3.h"
}

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace {

constexpr int kInitialWindowWidth = 1280;
constexpr int kInitialWindowHeight = 800;

void GlfwErrorCallback(int error, const char* description) {
  std::cerr << "GLFW error " << error << ": " << description << "\n";
}

std::string LoadFile(const std::string& filename) {
  std::ifstream file(filename, std::ios::binary);
  if (!file) {
    std::cerr << "Could not open file " << filename << "\n";
    return {};
  }
  std::ostringstream out;
  out << file.rdbuf();
  return std::move(out).str();
}

}  // namespace

int main(int argc, char** argv) {
  // When launched via `bazel run`, restore the user's working directory so
  // relative paths resolve naturally.
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  if (argc != 2) {
    std::cerr << "Usage: svg_viewer <filename>\n";
    return 1;
  }

  const std::string svgPath = argv[1];
  const std::string initialSource = LoadFile(svgPath);
  if (initialSource.empty()) {
    return 1;
  }

  // ---------------------------------------------------------------------------
  // GLFW + OpenGL context
  // ---------------------------------------------------------------------------
  glfwSetErrorCallback(GlfwErrorCallback);
  if (!glfwInit()) {
    std::cerr << "Failed to initialize GLFW\n";
    return 1;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

  GLFWwindow* window = glfwCreateWindow(kInitialWindowWidth, kInitialWindowHeight,
                                        "Donner SVG Viewer", nullptr, nullptr);
  if (!window) {
    std::cerr << "Failed to create GLFW window\n";
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);  // vsync

  if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
    std::cerr << "Failed to initialize OpenGL loader\n";
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  // ---------------------------------------------------------------------------
  // ImGui
  // ---------------------------------------------------------------------------
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // Editor preference persistence is disabled per docs/design_docs/editor.md
  // (Security: ImGui ini file privacy/trust surface).
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 330");

  // ---------------------------------------------------------------------------
  // Editor state
  // ---------------------------------------------------------------------------
  donner::editor::EditorApp app;
  if (!app.loadFromString(initialSource)) {
    std::cerr << "Failed to parse SVG file: " << svgPath << "\n";
    // Continue running with an empty document so the user can edit the source.
  }

  donner::editor::SelectTool selectTool;

  donner::editor::TextEditor textEditor;
  textEditor.setLanguageDefinition(donner::editor::TextEditor::LanguageDefinition::SVG());
  textEditor.setText(initialSource);

  donner::svg::Renderer renderer;
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  std::uint64_t lastRenderedVersion = 0;
  int textureWidth = 0;
  int textureHeight = 0;

  // ---------------------------------------------------------------------------
  // Main loop
  // ---------------------------------------------------------------------------
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    // Drain the command queue (handles ReplaceDocument from text edits and
    // SetTransform from SelectTool drags).
    app.flushFrame();

    // Re-render only when the document version has changed.
    const auto currentVersion = app.document().currentFrameVersion();
    if (currentVersion != lastRenderedVersion && app.hasDocument()) {
      renderer.draw(app.document().document());
      const donner::svg::RendererBitmap bitmap = renderer.takeSnapshot();
      if (!bitmap.empty()) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, bitmap.pixels.data());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        textureWidth = bitmap.dimensions.x;
        textureHeight = bitmap.dimensions.y;
      }
      lastRenderedVersion = currentVersion;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Source pane: TextEditor with full-regen on change.
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, kInitialWindowHeight), ImGuiCond_FirstUseEver);
    ImGui::Begin("Source");
    textEditor.render("##source");
    if (textEditor.isTextChanged()) {
      app.applyMutation(donner::editor::EditorCommand::ReplaceDocumentCommand(
          textEditor.getText()));
    }
    ImGui::End();

    // Render pane: SVG bitmap + SelectTool mouse handling.
    ImGui::SetNextWindowPos(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(720, kInitialWindowHeight), ImGuiCond_FirstUseEver);
    ImGui::Begin("Render");
    if (textureWidth > 0 && textureHeight > 0) {
      const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
      ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                   ImVec2(static_cast<float>(textureWidth), static_cast<float>(textureHeight)));

      // Screen → document mapping. The image is currently rendered at the
      // canvas's native size (no editor-side zoom), so the mapping is a
      // simple translate. When pan/zoom lands at the main-loop layer this
      // becomes a proper ViewportGeometry::screenToDocument call.
      const auto screenToDocument = [&](const ImVec2& screenPoint) {
        return donner::Vector2d(screenPoint.x - imageOrigin.x, screenPoint.y - imageOrigin.y);
      };

      if (ImGui::IsItemHovered()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          selectTool.onMouseDown(app, screenToDocument(ImGui::GetMousePos()));
        }
      }
      // Drag continues even when the cursor leaves the image (matches
      // standard editor UX).
      if (selectTool.isDragging()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
          selectTool.onMouseMove(app, screenToDocument(ImGui::GetMousePos()),
                                 /*buttonHeld=*/true);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
          selectTool.onMouseUp(app, screenToDocument(ImGui::GetMousePos()));
        }
      }
    } else {
      ImGui::TextUnformatted("(no rendered image)");
    }
    ImGui::End();

    ImGui::Render();
    int displayWidth = 0;
    int displayHeight = 0;
    glfwGetFramebufferSize(window, &displayWidth, &displayHeight);
    glViewport(0, 0, displayWidth, displayHeight);
    glClearColor(0.10f, 0.10f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
  }

  // ---------------------------------------------------------------------------
  // Teardown
  // ---------------------------------------------------------------------------
  glDeleteTextures(1, &texture);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
