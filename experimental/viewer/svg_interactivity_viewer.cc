/**
 * @file svg_interactivity_viewer.cc
 * @brief Interactive SVG viewer demonstrating hit testing and event handling.
 *
 * Demonstrates the interactivity API with an ImGui-based viewer that shows:
 * - Real-time hit testing with element highlighting
 * - Click-to-select with bounding box overlay
 * - Element inspector showing tag, id, bounds, and attributes
 * - Hover state tracking with cursor type display
 * - Event log showing dispatched events
 *
 * To run:
 * ```sh
 * bazel run -c opt --run_under="cd $PWD &&" //experimental/viewer:svg_interactivity_viewer -- input.svg
 * ```
 */

#include <fstream>
#include <iostream>
#include <sstream>

#include "donner/base/Box.h"
#include "glad/glad.h"

extern "C" {
#include "GLFW/glfw3.h"
}

#include "donner/base/FailureSignalHandler.h"
#include "donner/svg/DonnerController.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

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

std::string toString(CursorType cursor) {
  std::ostringstream oss;
  oss << cursor;
  return oss.str();
}

struct EventLogEntry {
  std::string description;
  float timestamp;
};

int main(int argc, char** argv) {
  donner::InstallFailureSignalHandler();

  if (argc != 2) {
    std::cerr << "Usage: svg_interactivity_viewer <filename>" << std::endl;
    return 1;
  }

  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit()) {
    return 1;
  }

  const char* glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  GLFWwindow* window = glfwCreateWindow(1280, 720, "SVG Interactivity Viewer", NULL, NULL);
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
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  // Load and parse the SVG.
  std::string svgSource = loadFile(argv[1]);

  SVGParser::Options options;
  options.enableExperimental = true;

  ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(svgSource, nullptr, options);
  if (maybeDocument.hasError()) {
    std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
    return 1;
  }

  SVGDocument document = std::move(maybeDocument.result());
  DonnerController controller(document);
  Renderer renderer;

  // Interaction state.
  std::optional<SVGElement> selectedElement;
  std::optional<Boxd> selectedBounds;
  Vector2d lastDocPos(0, 0);
  std::deque<EventLogEntry> eventLog;
  constexpr size_t kMaxLogEntries = 50;

  auto addLogEntry = [&](const std::string& desc) {
    eventLog.push_front({desc, static_cast<float>(glfwGetTime())});
    if (eventLog.size() > kMaxLogEntries) {
      eventLog.pop_back();
    }
  };

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Left panel: SVG rendering.
    float panelWidth = 350.0f;
    float svgWidth = io.DisplaySize.x - panelWidth;

    document.setCanvasSize(static_cast<int>(svgWidth), static_cast<int>(io.DisplaySize.y));

    // Compute document-space mouse position.
    ImVec2 mousePos = ImGui::GetMousePos();
    const float scale = document.documentFromCanvasTransform().data[0];
    Vector2d docPos(mousePos.x / scale, mousePos.y / scale);
    lastDocPos = docPos;

    // Update hover state.
    controller.updateHover(docPos);

    // Handle clicks.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mousePos.x < svgWidth) {
      if (auto hit = controller.findIntersecting(docPos)) {
        selectedElement = hit->cast<SVGElement>();
        selectedBounds = controller.getWorldBounds(*selectedElement);

        std::ostringstream oss;
        oss << "click <" << selectedElement->tagName().name << ">";
        if (auto id = selectedElement->getAttribute("id")) {
          oss << " id=\"" << *id << "\"";
        }
        addLogEntry(oss.str());
      } else {
        selectedElement = std::nullopt;
        selectedBounds = std::nullopt;
        addLogEntry("click (no element)");
      }
    }

    // Render SVG.
    renderer.draw(document);
    const RendererBitmap bitmap = renderer.takeSnapshot();
    if (!bitmap.empty()) {
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, bitmap.pixels.data());
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    // SVG viewport.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(svgWidth, io.DisplaySize.y));
    ImGui::Begin("SVG", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    ImVec2 svgOrigin = ImGui::GetCursorScreenPos();
    ImGui::Image(static_cast<ImTextureID>(texture),
                 ImVec2(static_cast<float>(renderer.width()),
                        static_cast<float>(renderer.height())));

    // Draw selection overlay.
    if (selectedBounds) {
      ImDrawList* drawList = ImGui::GetWindowDrawList();
      ImVec2 tl(svgOrigin.x + static_cast<float>(selectedBounds->topLeft.x * scale),
                svgOrigin.y + static_cast<float>(selectedBounds->topLeft.y * scale));
      ImVec2 br(svgOrigin.x + static_cast<float>(selectedBounds->bottomRight.x * scale),
                svgOrigin.y + static_cast<float>(selectedBounds->bottomRight.y * scale));
      drawList->AddRect(tl, br, IM_COL32(0, 191, 255, 255), 0.0f, 0, 2.0f);
    }
    ImGui::End();

    // Right panel: Inspector.
    ImGui::SetNextWindowPos(ImVec2(svgWidth, 0));
    ImGui::SetNextWindowSize(ImVec2(panelWidth, io.DisplaySize.y));
    ImGui::Begin("Inspector", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

    // Pointer info.
    if (ImGui::CollapsingHeader("Pointer", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Text("Position: (%.1f, %.1f)", lastDocPos.x, lastDocPos.y);

      CursorType cursor = controller.getCursorAt(lastDocPos);
      ImGui::Text("Cursor: %s", toString(cursor).c_str());

      if (auto hovered = controller.hoveredElement()) {
        ImGui::Text("Hover: <%s>", std::string(hovered->tagName().name).c_str());
      } else {
        ImGui::Text("Hover: (none)");
      }

      auto allUnder = controller.findAllIntersecting(lastDocPos);
      ImGui::Text("Elements under pointer: %zu", allUnder.size());
    }

    // Selected element inspector.
    if (ImGui::CollapsingHeader("Selected Element", ImGuiTreeNodeFlags_DefaultOpen)) {
      if (selectedElement) {
        ImGui::Text("Tag: <%s>", std::string(selectedElement->tagName().name).c_str());

        if (auto id = selectedElement->getAttribute("id")) {
          ImGui::Text("ID: %s", std::string(*id).c_str());
        }

        if (selectedBounds) {
          ImGui::Text("Bounds: (%.1f, %.1f) - (%.1f, %.1f)", selectedBounds->topLeft.x,
                      selectedBounds->topLeft.y, selectedBounds->bottomRight.x,
                      selectedBounds->bottomRight.y);
          ImGui::Text("Size: %.1f x %.1f", selectedBounds->width(), selectedBounds->height());
        }
      } else {
        ImGui::TextDisabled("Click an element to select it");
      }
    }

    // Event log.
    if (ImGui::CollapsingHeader("Event Log", ImGuiTreeNodeFlags_DefaultOpen)) {
      if (ImGui::Button("Clear")) {
        eventLog.clear();
      }
      ImGui::BeginChild("EventLogScroll", ImVec2(0, 200), true);
      for (const auto& entry : eventLog) {
        ImGui::TextWrapped("[%.1fs] %s", entry.timestamp, entry.description.c_str());
      }
      ImGui::EndChild();
    }

    ImGui::Text("%.1f FPS", io.Framerate);
    ImGui::End();

    // Render ImGui.
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
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
