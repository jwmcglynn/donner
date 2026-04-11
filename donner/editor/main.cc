/// @file
///
/// `//donner/editor:editor` — the full-featured Donner editor binary.
///
/// This is the advanced editor application. It wires `EditorApp`,
/// `SelectTool`, `OverlayRenderer`, and `TextEditor` into an ImGui
/// shell with a live source pane and a click-and-drag render pane.
///
/// Run with:
///
/// ```sh
/// bazel run //donner/editor -- donner_splash.svg
/// ```
///
/// For the minimal viewer demo (no tools, no overlay chrome, just a
/// rendered SVG and a text pane) see `//examples:svg_viewer`.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "donner/base/FileOffset.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/Notice.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TextBuffer.h"
#include "donner/editor/TextEditor.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/shape/ShapeSystem.h"
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
constexpr float kSourcePaneWidth = 560.0f;

void GlfwErrorCallback(int error, const char* description) {
  std::cerr << "GLFW error " << error << ": " << description << "\n";
}

/// Convert a donner `FileOffset` (1-based line) into a `TextEditor`
/// `Coordinates` (0-based line).
donner::editor::Coordinates FileOffsetToEditorCoordinates(const donner::FileOffset& offset) {
  if (!offset.lineInfo.has_value()) {
    return donner::editor::Coordinates(0, 0);
  }
  return donner::editor::Coordinates(static_cast<int>(offset.lineInfo->line) - 1,
                                     static_cast<int>(offset.lineInfo->offsetOnLine));
}

/// Render a small inspector line that describes the currently-selected
/// element (tag, id, world bounds, local transform). No-op if nothing
/// is selected.
void RenderInspector(const donner::editor::EditorApp& app) {
  if (!app.hasSelection() || !app.hasDocument()) {
    ImGui::TextDisabled("Nothing selected. Click an element to inspect.");
    return;
  }

  auto& mutableRegistry =
      const_cast<donner::Registry&>(app.document().document().registry());
  donner::EntityHandle handle(mutableRegistry, app.selectedEntity());
  if (!handle) {
    ImGui::TextDisabled("Selected entity no longer exists.");
    return;
  }

  // Tag name + id via the XML node wrapper (the same wrapper that
  // powers the XML source highlight on click). `RcString` / `RcStringOrRef`
  // convert to `std::string_view` but aren't null-terminated, so we use
  // the `%.*s` form which takes length + data directly.
  auto xmlNode = donner::xml::XMLNode::TryCast(handle);
  if (xmlNode.has_value()) {
    const std::string_view tagSv = xmlNode->tagName().name;
    const auto id = xmlNode->getAttribute(donner::xml::XMLQualifiedNameRef("id"));
    if (id.has_value()) {
      const std::string_view idSv = *id;
      ImGui::Text("Selected: <%.*s id=\"%.*s\">", static_cast<int>(tagSv.size()), tagSv.data(),
                  static_cast<int>(idSv.size()), idSv.data());
    } else {
      ImGui::Text("Selected: <%.*s>", static_cast<int>(tagSv.size()), tagSv.data());
    }
  } else {
    ImGui::TextUnformatted("Selected: (non-XML element)");
  }

  // World-space bounds via the same path OverlayRenderer uses for the
  // selection chrome rect.
  if (auto bounds = donner::svg::components::ShapeSystem().getShapeWorldBounds(handle);
      bounds.has_value()) {
    ImGui::Text("Bounds: (%.1f, %.1f) %.1f × %.1f", bounds->topLeft.x, bounds->topLeft.y,
                bounds->width(), bounds->height());
  }

  // Local (parent-from-entity) transform — the same quantity SelectTool
  // drags and UndoTimeline snapshots.
  const donner::Transform2d xform =
      donner::svg::components::LayoutSystem().getRawEntityFromParentTransform(handle);
  ImGui::Text("Transform: [%.3f %.3f %.3f %.3f  %.2f %.2f]", xform.data[0], xform.data[1],
              xform.data[2], xform.data[3], xform.data[4], xform.data[5]);
}

/// Highlight an entity's XML source span in the text editor. No-op if the
/// entity doesn't carry XML source offsets (which should never happen for
/// a parse from `SVGParser::ParseSVG`, but is defensive).
void HighlightEntitySource(const donner::editor::EditorApp& app,
                           donner::editor::TextEditor& textEditor,
                           donner::Entity entity) {
  if (entity == entt::null || !app.hasDocument()) {
    return;
  }
  auto& mutableRegistry =
      const_cast<donner::Registry&>(app.document().document().registry());
  donner::EntityHandle handle(mutableRegistry, entity);
  if (!handle) {
    return;
  }
  auto xmlNode = donner::xml::XMLNode::TryCast(handle);
  if (!xmlNode.has_value()) {
    return;
  }
  auto range = xmlNode->getNodeLocation();
  if (!range.has_value()) {
    return;
  }
  textEditor.selectAndFocus(FileOffsetToEditorCoordinates(range->start),
                            FileOffsetToEditorCoordinates(range->end));
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
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  if (argc != 2) {
    std::cerr << "Usage: donner-editor <filename>\n";
    return 1;
  }

  const std::string svgPath = argv[1];
  const std::string initialSource = LoadFile(svgPath);
  if (initialSource.empty()) {
    return 1;
  }

  // ---------------------------------------------------------------------------
  // GLFW + OpenGL
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
                                        "Donner Editor", nullptr, nullptr);
  if (!window) {
    std::cerr << "Failed to create GLFW window\n";
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

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
  // imgui.ini persistence is disabled per the editor security policy in
  // docs/design_docs/editor.md.
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
    // Keep running with an empty document so the user can still fix it
    // in the source pane.
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
  donner::Entity lastRenderedSelection = entt::null;
  donner::Entity lastHighlightedSelection = entt::null;
  donner::Vector2i lastRenderedCanvasSize{0, 0};
  int textureWidth = 0;
  int textureHeight = 0;

  // ---------------------------------------------------------------------------
  // Main loop
  // ---------------------------------------------------------------------------
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    // Drain the command queue (ReplaceDocument from text edits,
    // SetTransform from SelectTool drags).
    app.flushFrame();

    // Re-render whenever the document version changes, the selection
    // changes, OR the canvas size changed (the latter happens when
    // the render pane is resized — the main loop pokes the document
    // below before this check).
    const auto currentVersion = app.document().currentFrameVersion();
    const donner::Entity currentSelection = app.selectedEntity();
    const donner::Vector2i currentCanvasSize =
        app.hasDocument() ? app.document().document().canvasSize() : donner::Vector2i{0, 0};
    if ((currentVersion != lastRenderedVersion || currentSelection != lastRenderedSelection ||
         currentCanvasSize != lastRenderedCanvasSize) &&
        app.hasDocument()) {
      renderer.draw(app.document().document());
      donner::editor::OverlayRenderer::drawChrome(renderer, app);
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
      lastRenderedSelection = currentSelection;
      lastRenderedCanvasSize = currentCanvasSize;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Main menu bar. Kept deliberately minimal — Help → About is the
    // only required entry for this milestone, but the bar exists so
    // future menus (File, Edit, Window, etc.) can slot in without
    // restructuring.
    bool openAboutPopup = false;
    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About Donner Editor")) {
          openAboutPopup = true;
        }
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }
    if (openAboutPopup) {
      ImGui::OpenPopup("About Donner Editor");
    }

    // About popup — displays the embedded open-source notice. Required
    // for every distributed build to satisfy the attribution obligations
    // of the imgui, glfw, tracy, skia, freetype, and harfbuzz licenses.
    // Text comes from `//third_party/licenses:notice_editor` embedded
    // via `//donner/editor:notice`.
    {
      const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(displaySize.x * 0.7f, displaySize.y * 0.8f),
                               ImGuiCond_Appearing);
      ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                              ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      if (ImGui::BeginPopupModal("About Donner Editor", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextUnformatted("Donner Editor — in-tree SVG editor built on donner/svg");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted("Open-source license notices:");
        ImGui::Spacing();
        if (ImGui::BeginChild("##notice_scroll",
                              ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()),
                              /*border=*/true)) {
          const auto& notice = donner::embedded::kEditorNoticeText;
          ImGui::TextUnformatted(reinterpret_cast<const char*>(notice.data()),
                                 reinterpret_cast<const char*>(notice.data() + notice.size()));
        }
        ImGui::EndChild();
        if (ImGui::Button("Close")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    }

    // Global keyboard shortcuts. ImGui translates Cmd on macOS and Ctrl
    // elsewhere into `Super`/`Ctrl` key mods, so we accept either.
    {
      const bool cmd = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
      const bool shift = ImGui::GetIO().KeyShift;
      const bool pressedZ = ImGui::IsKeyPressed(ImGuiKey_Z, /*repeat=*/false);

      if (pressedZ && cmd && !shift) {
        // Cmd+Z: undo
        if (app.canUndo()) {
          app.undo();
        }
      } else if (pressedZ && cmd && shift) {
        // Cmd+Shift+Z: redo (break chain + undo-of-undo)
        app.redo();
      }

      // Escape: clear the current selection. Modal popups capture Escape
      // first (to close themselves), so this only fires when no popup is
      // active.
      if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) &&
          ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) && app.hasSelection()) {
        app.setSelection(entt::null);
      }
    }

    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);

    // Windows are locked in place: positions and sizes are re-asserted
    // every frame with `ImGuiCond_Always`, and the NoMove / NoResize /
    // NoCollapse flags prevent any in-frame attempts to drag them
    // around (otherwise a click-and-drag on the image surface falls
    // through to the parent window and the user ends up moving the
    // pane instead of the editor target).
    const ImGuiWindowFlags kPaneFlags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    // Source pane.
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSourcePaneWidth, static_cast<float>(windowHeight)),
                             ImGuiCond_Always);
    ImGui::Begin("Source", nullptr, kPaneFlags);
    textEditor.render("##source");
    if (textEditor.isTextChanged()) {
      app.applyMutation(donner::editor::EditorCommand::ReplaceDocumentCommand(
          textEditor.getText()));
      // `isTextChanged` is sticky until explicitly reset — without this
      // the next frame would re-submit another ReplaceDocument command
      // and clobber any in-progress drag state.
      textEditor.resetTextChanged();
    }
    ImGui::End();

    // Render pane + SelectTool interaction.
    ImGui::SetNextWindowPos(ImVec2(kSourcePaneWidth, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(static_cast<float>(windowWidth) - kSourcePaneWidth, static_cast<float>(windowHeight)),
        ImGuiCond_Always);
    ImGui::Begin("Render", nullptr, kPaneFlags);

    // Inspector header — always-visible summary of what's selected.
    RenderInspector(app);
    ImGui::Separator();

    // Resize the document's canvas to match the content region, so the
    // image scales to fit the pane instead of overflowing (for big SVGs)
    // or leaving whitespace (for small ones). Only call setCanvasSize
    // when the desired size changes — otherwise we'd bump the document
    // every frame. The render re-trigger picks up the change via the
    // `currentCanvasSize != lastRenderedCanvasSize` check above.
    const ImVec2 contentRegion = ImGui::GetContentRegionAvail();
    const int desiredW = static_cast<int>(contentRegion.x);
    const int desiredH = static_cast<int>(contentRegion.y);
    if (app.hasDocument() && desiredW > 0 && desiredH > 0) {
      const donner::Vector2i currentSize = app.document().document().canvasSize();
      if (currentSize.x != desiredW || currentSize.y != desiredH) {
        app.document().document().setCanvasSize(desiredW, desiredH);
      }
    }

    if (textureWidth > 0 && textureHeight > 0) {
      const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
      ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                   ImVec2(static_cast<float>(textureWidth), static_cast<float>(textureHeight)));

      // Screen → document mapping. The canvas may be scaled relative to
      // the document viewBox, so run screen-space clicks through
      // `documentFromCanvasTransform` to recover the document-space
      // point that `SelectTool` and `hitTest` want.
      const donner::Transform2d documentFromCanvas =
          app.document().document().documentFromCanvasTransform();
      const auto screenToDocument = [&](const ImVec2& screenPoint) {
        const donner::Vector2d canvasPoint(screenPoint.x - imageOrigin.x,
                                            screenPoint.y - imageOrigin.y);
        return documentFromCanvas.transformPosition(canvasPoint);
      };

      if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        selectTool.onMouseDown(app, screenToDocument(ImGui::GetMousePos()));
      }
      // Drag continues even after the cursor leaves the image.
      if (selectTool.isDragging()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
          selectTool.onMouseMove(app, screenToDocument(ImGui::GetMousePos()),
                                 /*buttonHeld=*/true);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
          selectTool.onMouseUp(app, screenToDocument(ImGui::GetMousePos()));
        }
      }

      // Whenever the selection changes, jump the source pane to the
      // selected element's XML span. Gated on actual selection change so
      // the highlight only fires on click, not every frame.
      const donner::Entity selectionNow = app.selectedEntity();
      if (selectionNow != lastHighlightedSelection) {
        HighlightEntitySource(app, textEditor, selectionNow);
        lastHighlightedSelection = selectionNow;
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
