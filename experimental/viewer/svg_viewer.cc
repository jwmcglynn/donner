#include <fstream>
#include <iostream>
#include <sstream>

#include "donner/base/Box.h"
#include "glad/glad.h"

extern "C" {
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
}

#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "donner/svg/AllSVGElements.h"
#include "donner/svg/DonnerController.h"
#include "donner/svg/SVG.h"  // IWYU pragma keep: Used for SVGDocument and SVGParser
#include "donner/svg/renderer/RendererSkia.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

using namespace donner;
using namespace donner::svg;
using namespace donner::svg::parser;

static void glfw_error_callback(int error, const char* description) {
  std::cerr << "Glfw Error " << error << ": " << description << std::endl;
}

std::string loadFile(const char* filename) {
  std::ifstream file(filename);
  if (!file) {
    std::cerr << "Could not open file " << filename << std::endl;
    std::abort();
  }

  std::string fileData;
  file.seekg(0, std::ios::end);
  const std::streamsize fileLength = file.tellg();
  file.seekg(0);

  fileData.resize(fileLength);
  file.read(fileData.data(), fileLength);

  return fileData;
}

struct SVGState {
  bool valid = false;
  SVGDocument document;
  std::optional<DonnerController> controller;
  std::optional<ParseError> lastError;
  std::optional<SVGRectElement> boundsShape;
  std::optional<SVGPathElement> selectedPathOutline;

  std::optional<SVGElement> selectedElement;

  void loadSVG(const std::string& source) {
    document = SVGDocument();

    ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(source);
    if (maybeDocument.hasError()) {
      lastError = maybeDocument.error();
      valid = false;
      return;
    }

    document = std::move(maybeDocument.result());
    controller = DonnerController(document);

    auto editorOnlyContainer = SVGUnknownElement::Create(document, "editor-only");
    document.svgElement().appendChild(editorOnlyContainer);

    boundsShape = SVGRectElement::Create(document);
    editorOnlyContainer.appendChild(boundsShape.value());
    boundsShape->setStyle(
        "display: none; fill: none; stroke: deepskyblue; stroke-width: 1px; pointer-events: none");

    selectedPathOutline = SVGPathElement::Create(document);
    editorOnlyContainer.appendChild(selectedPathOutline.value());
    selectedPathOutline->setStyle(
        "display: none; fill: none; stroke: deepskyblue; stroke-width: 1px; pointer-events: none");

    selectedElement = std::nullopt;
    lastError = std::nullopt;
    valid = true;
  }

  void setBounds(const donner::Boxd& box) {
    if (boundsShape) {
      boundsShape->setStyle("display: inline");
      boundsShape->setX(donner::Lengthd(box.topLeft.x));
      boundsShape->setY(donner::Lengthd(box.topLeft.y));
      boundsShape->setWidth(donner::Lengthd(box.width()));
      boundsShape->setHeight(donner::Lengthd(box.height()));
    }
  }

  void setOverlayScale(float scale) {
    if (boundsShape) {
      boundsShape->setStyle("stroke-width: " + std::to_string(scale) + "px;");
    }

    if (selectedPathOutline) {
      selectedPathOutline->setStyle("stroke-width: " + std::to_string(scale) + "px;");
    }
  }

  void selectElement(const SVGElement& element) {
    selectedElement = element;

    if (element.isa<SVGGeometryElement>()) {
      auto geomElement = element.cast<SVGGeometryElement>();

      if (selectedPathOutline) {
        selectedPathOutline->setStyle("display: inline");
        if (auto computedSpline = geomElement.computedSpline()) {
          selectedPathOutline->setSpline(computedSpline.value());
          selectedPathOutline->setTransform(geomElement.elementFromWorld());
          setBounds(geomElement.worldBounds().value());
        }
      }
    }
  }

  void selectNone() {
    selectedElement = std::nullopt;
    if (selectedPathOutline) {
      selectedPathOutline->setStyle("display: none");
    }

    if (boundsShape) {
      boundsShape->setStyle("display: none");
    }
  }

  void handleClick(double x, double y) {
    if (auto maybeElm = controller->findIntersecting(Vector2d(x, y))) {
      selectElement(*maybeElm);
    } else {
      selectNone();
    }
  }
};

int main(int argc, char** argv) {
  // Initialize the symbolizer to get a human-readable stack trace
  absl::InitializeSymbolizer(argv[0]);

  absl::FailureSignalHandlerOptions options;
  absl::InstallFailureSignalHandler(options);

  if (argc != 2) {
    std::cerr << "Usage: svg_viewer <filename>" << std::endl;
    return 1;
  }

  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit()) {
    return 1;
  }

  const char* glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  GLFWwindow* window = glfwCreateWindow(1280, 720, "SVG Viewer", NULL, NULL);
  if (window == NULL) {
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
    std::cerr << "Failed to initialize OpenGL loader!" << std::endl;
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;

  // Enable Docking and Viewports
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable Docking
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;    // Enable Multi-Viewport / Platform Windows

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();

  // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look
  // identical to regular ones.
  ImGuiStyle& style = ImGui::GetStyle();
  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
  }

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  SVGState state;
  std::string svgString = loadFile(argv[1]);

  state.loadSVG(svgString);

  RendererSkia renderer;

  bool svgChanged = false;

  // Variables for window visibility
  static bool dockspaceInitialized = false;

  static int lastWindowWidth = 0;
  static int lastWindowHeight = 0;

  while (!glfwWindowShouldClose(window)) {
    if (svgChanged) {
      state.loadSVG(svgString);
    }

    // Get current window size
    int windowWidth, windowHeight;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);

    // Check if the window size has changed
    if (windowWidth != lastWindowWidth || windowHeight != lastWindowHeight) {
      lastWindowWidth = windowWidth;
      lastWindowHeight = windowHeight;

      // Rebuild the dock layout
      dockspaceInitialized = false;
    }

    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();

    ImGui::NewFrame();

    // Begin DockSpace
    static ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_None;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    const ImGuiWindowFlags mainWindowFlags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("MainWindow", nullptr, mainWindowFlags);

    ImGui::PopStyleVar(2);

    // DockSpace
    ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockspaceFlags);

    // Set up initial docking layout
    if (!dockspaceInitialized) {
      dockspaceInitialized = true;

      ImGui::DockBuilderRemoveNode(dockspaceId);  // clear any previous layout
      ImGui::DockBuilderAddNode(dockspaceId, dockspaceFlags | ImGuiDockNodeFlags_DockSpace);
      ImGui::DockBuilderSetNodeSize(dockspaceId, io.DisplaySize);

      ImGuiID dockIdLeft = dockspaceId;
      ImGuiID dockIdRight;

      ImGui::DockBuilderSplitNode(dockIdLeft, ImGuiDir_Right, 0.5f, &dockIdRight, &dockIdLeft);

      ImGui::DockBuilderDockWindow("Code", dockIdLeft);
      ImGui::DockBuilderDockWindow("Drawing", dockIdRight);

      ImGui::DockBuilderFinish(dockspaceId);
    }

    // Text Editor Window
    ImGuiWindowClass windowClassNoUndocking;
    windowClassNoUndocking.DockNodeFlagsOverrideSet =
        ImGuiDockNodeFlags_NoUndocking | ImGuiDockNodeFlags_NoTabBar;

    ImGui::SetNextWindowClass(&windowClassNoUndocking);
    ImGui::Begin("Code", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

    {
      static ImGuiInputTextFlags flags = 0;
      svgChanged =
          ImGui::InputTextMultiline("##source", &svgString, ImVec2(-FLT_MIN, -FLT_MIN), flags);
    }

    if (state.lastError) {
      ImGui::Text("Error: %s", state.lastError->reason.c_str());
    }

    ImGui::End();  // End of Code Window

    // SVG Viewer Window
    ImGui::SetNextWindowClass(&windowClassNoUndocking);
    ImGui::Begin("Drawing");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
                ImGui::GetIO().Framerate);

    const ImVec2 regionSize = ImGui::GetContentRegionAvail();
    if (regionSize.x > 0 && regionSize.y > 0) {
      state.document.setCanvasSize(regionSize.x, regionSize.y);
    }
    const float scale = state.document.documentFromCanvasTransform().data[0];
    state.setOverlayScale(1.0f / scale);

    ImVec2 mousePositionAbsolute = ImGui::GetMousePos();
    ImVec2 screenPositionAbsolute = ImGui::GetWindowPos();
    screenPositionAbsolute.x += ImGui::GetCursorPosX();
    screenPositionAbsolute.y += ImGui::GetCursorPosY();
    ImVec2 mousePositionRelative =
        ImVec2((mousePositionAbsolute.x - screenPositionAbsolute.x) / scale,
               (mousePositionAbsolute.y - screenPositionAbsolute.y) / scale);

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
      state.handleClick(mousePositionRelative.x, mousePositionRelative.y);
    }

    if (state.valid) {
      renderer.draw(state.document);
      const SkBitmap& bitmap = renderer.bitmap();
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.width(), bitmap.height(), 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, bitmap.getPixels());
    }

    ImGui::Image(static_cast<ImTextureID>(texture), ImVec2(renderer.width(), renderer.height()));

    ImGui::End();  // End of Drawing Window
    ImGui::End();  // End of MainWindow

    // Rendering
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Update and Render additional Platform Windows
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      GLFWwindow* backup_current_context = glfwGetCurrentContext();
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
      glfwMakeContextCurrent(backup_current_context);
    }

    glfwSwapBuffers(window);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
