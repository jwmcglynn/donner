/**
 * @example svg_viewer.cc Minimal Donner SVG viewer with a live text pane.
 *
 * A small ImGui demo: load an SVG, display it, click to select a shape,
 * and edit the source in a text pane that re-parses on every keystroke.
 * Selection chrome is drawn by injecting an `editor-only` `<rect>` and
 * `<path>` into the document tree — no separate overlay renderer and no
 * editor-side command queue. The **only** dependency on the editor tree
 * is `donner::editor::TextEditor`, the syntax-aware text widget.
 *
 * For the full editor binary (EditorApp + SelectTool + OverlayRenderer
 * + command queue + mutation seam) see `//donner/editor`.
 *
 * To run:
 *
 * ```sh
 * bazel run //examples:svg_viewer -- donner_splash.svg
 * ```
 */

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "donner/base/Box.h"
#include "donner/base/FileOffset.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Vector2.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/TextBuffer.h"
#include "donner/editor/TextEditor.h"
#include "donner/svg/DonnerController.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGUnknownElement.h"
#include "donner/svg/parser/SVGParser.h"
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

/// Bundles the loaded document with its hit-test controller and the
/// editor-only overlay nodes that render the current selection. Re-parsing
/// throws all of this away and rebuilds it.
struct ViewerState {
  donner::svg::SVGDocument document;
  std::optional<donner::svg::DonnerController> controller;
  std::optional<donner::svg::SVGRectElement> boundsShape;
  std::optional<donner::svg::SVGPathElement> selectedPathOutline;
  std::optional<donner::svg::SVGElement> selectedElement;
  bool valid = false;

  void loadFromString(std::string_view source) {
    using namespace donner;
    using namespace donner::svg;
    using namespace donner::svg::parser;

    document = SVGDocument();
    valid = false;
    controller.reset();
    boundsShape.reset();
    selectedPathOutline.reset();
    selectedElement.reset();

    ParseWarningSink disabled = ParseWarningSink::Disabled();
    ParseResult<SVGDocument> maybe = SVGParser::ParseSVG(source, disabled);
    if (maybe.hasError()) {
      return;
    }

    document = std::move(maybe.result());
    controller = DonnerController(document);

    // Inject an editor-only container holding the selection chrome as
    // regular SVG elements. The renderer draws them alongside the real
    // document; toggling display:inline/none shows or hides them.
    auto editorOnly = SVGUnknownElement::Create(document, "editor-only");
    document.svgElement().appendChild(editorOnly);

    boundsShape = SVGRectElement::Create(document);
    editorOnly.appendChild(boundsShape.value());
    boundsShape->setStyle(
        "display: none; fill: none; stroke: deepskyblue; stroke-width: 1px; "
        "pointer-events: none");

    selectedPathOutline = SVGPathElement::Create(document);
    editorOnly.appendChild(selectedPathOutline.value());
    selectedPathOutline->setStyle(
        "display: none; fill: none; stroke: deepskyblue; stroke-width: 1px; "
        "pointer-events: none");

    valid = true;
  }

  void selectNone() {
    selectedElement.reset();
    if (boundsShape) {
      boundsShape->setStyle("display: none");
    }
    if (selectedPathOutline) {
      selectedPathOutline->setStyle("display: none");
    }
  }

  void selectElement(const donner::svg::SVGElement& element) {
    using namespace donner::svg;

    selectedElement = element;
    if (!element.isa<SVGGeometryElement>()) {
      return;
    }

    auto geom = element.cast<SVGGeometryElement>();
    if (auto spline = geom.computedSpline()) {
      if (selectedPathOutline) {
        selectedPathOutline->setStyle("display: inline");
        selectedPathOutline->setSpline(*spline);
        selectedPathOutline->setTransform(geom.elementFromWorld());
      }
      if (auto bounds = geom.worldBounds()) {
        if (boundsShape) {
          boundsShape->setStyle("display: inline");
          boundsShape->setX(donner::Lengthd(bounds->topLeft.x));
          boundsShape->setY(donner::Lengthd(bounds->topLeft.y));
          boundsShape->setWidth(donner::Lengthd(bounds->width()));
          boundsShape->setHeight(donner::Lengthd(bounds->height()));
        }
      }
    }
  }

  /// Click a document-space point. Returns the newly-selected element's
  /// source-location range (in the original SVG text) if the click hit a
  /// geometry element that carries XML source offsets, so the caller can
  /// highlight it in the text pane.
  ///
  /// Selection is **sticky** — clicking empty space is a no-op rather than
  /// a deselect. Only a click that lands on an element changes the
  /// selection. This matches the behavior of most vector editors and
  /// avoids accidental deselection while pan/zoom lands later.
  std::optional<donner::SourceRange> handleClick(const donner::Vector2d& documentPoint) {
    if (!controller) {
      return std::nullopt;
    }
    auto hit = controller->findIntersecting(documentPoint);
    if (!hit.has_value()) {
      return std::nullopt;
    }

    selectElement(*hit);

    if (auto xmlNode = donner::xml::XMLNode::TryCast(hit->entityHandle())) {
      return xmlNode->getNodeLocation();
    }
    return std::nullopt;
  }
};

/// Convert a `FileOffset` from donner's XML source location into a
/// `TextEditor` coordinate. donner's line is 1-based; `TextEditor` is
/// 0-based.
donner::editor::Coordinates FileOffsetToEditorCoordinates(const donner::FileOffset& offset) {
  if (!offset.lineInfo.has_value()) {
    return donner::editor::Coordinates(0, 0);
  }
  return donner::editor::Coordinates(static_cast<int>(offset.lineInfo->line) - 1,
                                     static_cast<int>(offset.lineInfo->offsetOnLine));
}

}  // namespace

int main(int argc, char** argv) {
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
                                        "Donner SVG Viewer", nullptr, nullptr);
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
  io.IniFilename = nullptr;  // no persistence
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 330");

  // ---------------------------------------------------------------------------
  // Viewer state
  // ---------------------------------------------------------------------------
  ViewerState state;
  state.loadFromString(initialSource);

  donner::editor::TextEditor textEditor;
  textEditor.setLanguageDefinition(donner::editor::TextEditor::LanguageDefinition::SVG());
  textEditor.setText(initialSource);

  donner::svg::Renderer renderer;
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  int textureWidth = 0;
  int textureHeight = 0;

  // ---------------------------------------------------------------------------
  // Main loop
  // ---------------------------------------------------------------------------
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    // Re-render every frame — this is a minimal demo and the document is
    // small. A real application would track a dirty flag.
    if (state.valid) {
      renderer.draw(state.document);
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
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);

    // Lock both panes in place. Without this, clicking on an image
    // inside an ImGui window falls through to the parent window and
    // tries to drag the pane around, since `ImGui::Image` doesn't
    // consume the mouse event.
    const ImGuiWindowFlags kPaneFlags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    // Source pane: TextEditor with full re-parse on change.
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSourcePaneWidth, static_cast<float>(windowHeight)),
                             ImGuiCond_Always);
    ImGui::Begin("Source", nullptr, kPaneFlags);
    textEditor.render("##source");
    if (textEditor.isTextChanged()) {
      state.loadFromString(textEditor.getText());
      // `isTextChanged` is a sticky flag — the caller is responsible for
      // clearing it. Without this reset, every frame would re-parse the
      // document and wipe any selection state we just built up via the
      // click handler below.
      textEditor.resetTextChanged();
    }
    ImGui::End();

    // Render pane: image + click-to-select.
    ImGui::SetNextWindowPos(ImVec2(kSourcePaneWidth, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(static_cast<float>(windowWidth) - kSourcePaneWidth,
               static_cast<float>(windowHeight)),
        ImGuiCond_Always);
    ImGui::Begin("Render", nullptr, kPaneFlags);

    // Size the document's canvas to the render pane so the SVG scales
    // to fit instead of overflowing large or leaving whitespace for
    // small documents. The original experimental viewer did the same.
    const ImVec2 contentRegion = ImGui::GetContentRegionAvail();
    const int desiredW = static_cast<int>(contentRegion.x);
    const int desiredH = static_cast<int>(contentRegion.y);
    if (state.valid && desiredW > 0 && desiredH > 0) {
      const donner::Vector2i currentSize = state.document.canvasSize();
      if (currentSize.x != desiredW || currentSize.y != desiredH) {
        state.document.setCanvasSize(desiredW, desiredH);
      }
    }

    if (textureWidth > 0 && textureHeight > 0) {
      const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
      ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                   ImVec2(static_cast<float>(textureWidth), static_cast<float>(textureHeight)));

      if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        // Map screen → canvas → document via the canvas-to-document
        // transform. When the canvas is scaled to fit the pane, the
        // transform bakes in the scale factor.
        const donner::Transform2d documentFromCanvas =
            state.document.documentFromCanvasTransform();
        const donner::Vector2d canvasPoint(mouse.x - imageOrigin.x, mouse.y - imageOrigin.y);
        const auto sourceRange = state.handleClick(documentFromCanvas.transformPosition(canvasPoint));
        if (sourceRange.has_value()) {
          textEditor.selectAndFocus(FileOffsetToEditorCoordinates(sourceRange->start),
                                    FileOffsetToEditorCoordinates(sourceRange->end));
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
