#include <fstream>
#include <iostream>

#include "donner/base/Box.h"
#include "glad/glad.h"

extern "C" {
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
}

#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "donner/svg/DonnerController.h"
#include "donner/svg/SVG.h"  // IWYU pragma keep: Used for SVGDocument and SVGParser
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/renderer/RendererSkia.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "misc/cpp/imgui_stdlib.h"

using namespace donner;
using namespace donner::base;
using namespace donner::base::parser;
using namespace donner::svg;
using namespace donner::svg::parser;

static void glfw_error_callback(int error, const char* description) {
  std::cerr << "Glfw Error " << error << ": " << description << std::endl;
}

SVGParser::InputBuffer loadFile(const char* filename) {
  std::ifstream file(filename);
  if (!file) {
    std::cerr << "Could not open file " << filename << std::endl;
    std::abort();
  }

  SVGParser::InputBuffer fileData;
  fileData.loadFromStream(file);

  return fileData;
}

std::string inputBufferToString(const SVGParser::InputBuffer& inputBuffer) {
  std::string result;
  result.reserve(inputBuffer.size());

  for (size_t i = 0; i < inputBuffer.size(); i++) {
    result += inputBuffer[i];
  }

  return result;
}

struct SVGState {
  bool valid = false;
  SVGDocument document;
  std::optional<DonnerController> controller;
  std::optional<ParseError> lastError;
  std::optional<SVGRectElement> boundsShape;
  std::optional<SVGPathElement> selectedPathOutline;

  void loadSVG(SVGParser::InputBuffer& inputBuffer) {
    document = SVGDocument();

    ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(inputBuffer);
    if (maybeDocument.hasError()) {
      lastError = maybeDocument.error();
      valid = false;
      return;
    }

    document = std::move(maybeDocument.result());
    controller = DonnerController(document);

    boundsShape = SVGRectElement::Create(document);
    document.svgElement().appendChild(boundsShape.value());
    boundsShape->setStyle(
        "fill: none; stroke: deepskyblue; stroke-width: 1px; pointer-events: none");

    selectedPathOutline = SVGPathElement::Create(document);
    document.svgElement().appendChild(selectedPathOutline.value());
    selectedPathOutline->setStyle(
        "fill: none; stroke: deepskyblue; stroke-width: 1px; pointer-events: none");

    lastError = std::nullopt;
    valid = true;
  }

  void setBounds(const donner::Boxd& box) {
    if (boundsShape) {
      boundsShape->setX(donner::Lengthd(box.topLeft.x));
      boundsShape->setY(donner::Lengthd(box.topLeft.y));
      boundsShape->setWidth(donner::Lengthd(box.width()));
      boundsShape->setHeight(donner::Lengthd(box.height()));
    }
  }

  void selectShape(const SVGGeometryElement& element) {
    if (selectedPathOutline) {
      if (auto computedSpline = element.computedSpline()) {
        selectedPathOutline->setSpline(computedSpline.value());
        selectedPathOutline->setTransform(element.elementFromWorld());
        setBounds(element.worldBounds().value());
      }
    }
  }

  void handleClick(double x, double y) {
    if (auto maybeElm = controller->findIntersecting(Vector2d(x, y))) {
      selectShape(*maybeElm);
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
  (void)io;
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  SVGState state;
  SVGParser::InputBuffer inputBuffer = loadFile(argv[1]);
  std::string svgString = inputBufferToString(inputBuffer);

  state.loadSVG(inputBuffer);

  RendererSkia renderer;
  bool svgChanged = false;

  while (!glfwWindowShouldClose(window)) {
    if (svgChanged) {
      inputBuffer = SVGParser::InputBuffer(svgString);
      state.loadSVG(inputBuffer);
    }

    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();

    ImGui::NewFrame();

    ImGui::Begin("SVG Viewer");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
                ImGui::GetIO().Framerate);

    const ImVec2 regionSize = ImGui::GetContentRegionAvail();
    const float scale =
        std::min(regionSize.x / (float)renderer.width(), regionSize.y / (float)renderer.height());

    ImVec2 mousePositionAbsolute = ImGui::GetMousePos();
    ImVec2 screenPositionAbsolute = ImGui::GetWindowPos();
    screenPositionAbsolute.x += ImGui::GetCursorPosX();
    screenPositionAbsolute.y += ImGui::GetCursorPosY();
    ImVec2 mousePositionRelative =
        ImVec2((mousePositionAbsolute.x - screenPositionAbsolute.x) / scale,
               (mousePositionAbsolute.y - screenPositionAbsolute.y) / scale);

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      state.handleClick(mousePositionRelative.x, mousePositionRelative.y);
    }

    if (state.valid) {
      renderer.draw(state.document);
      const SkBitmap& bitmap = renderer.bitmap();
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.width(), bitmap.height(), 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, bitmap.getPixels());
    }

    ImGui::Image((void*)(intptr_t)texture,
                 ImVec2(scale * renderer.width(), scale * renderer.height()));

    ImGui::End();

    ImGui::Begin("Text");
    ImGui::Text("Position: %f, %f", mousePositionRelative.x, mousePositionRelative.y);
    ImGui::Text("Mouse clicked: %s", ImGui::IsMouseDown(ImGuiMouseButton_Left) ? "Yes" : "No");

    {
      static ImGuiInputTextFlags flags = 0;
      svgChanged = ImGui::InputTextMultiline(
          "##source", &svgString, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 16), flags);
    }

    if (state.lastError) {
      ImGui::Text("Error: %s", state.lastError->reason.c_str());
    }

    ImGui::End();

    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
