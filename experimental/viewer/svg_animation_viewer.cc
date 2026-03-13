/**
 * @file svg_animation_viewer.cc
 * @brief Interactive SVG animation viewer with timeline controls.
 *
 * Demonstrates the animation API with an ImGui-based viewer that includes play/pause,
 * timeline scrubbing, speed control, and frame-by-frame stepping.
 *
 * To run:
 * ```sh
 * bazel run -c opt --run_under="cd $PWD &&" //experimental/viewer:svg_animation_viewer -- input.svg
 * ```
 */

#include <fstream>
#include <iostream>

#include "glad/glad.h"

extern "C" {
#include "GLFW/glfw3.h"
}

#include "donner/base/FailureSignalHandler.h"
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

int main(int argc, char** argv) {
  donner::InstallFailureSignalHandler();

  if (argc != 2) {
    std::cerr << "Usage: svg_animation_viewer <filename>" << std::endl;
    return 1;
  }

  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit()) {
    return 1;
  }

  const char* glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  GLFWwindow* window = glfwCreateWindow(1280, 720, "SVG Animation Viewer", NULL, NULL);
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
  Renderer renderer;

  // Animation state.
  bool playing = true;
  float currentTime = 0.0f;
  float playbackSpeed = 1.0f;
  float maxTime = 10.0f;
  bool loop = true;
  float stepSize = 1.0f / 30.0f;  // 30fps step
  double lastFrameTime = glfwGetTime();

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    // Advance animation time.
    double now = glfwGetTime();
    double deltaTime = now - lastFrameTime;
    lastFrameTime = now;

    if (playing) {
      currentTime += static_cast<float>(deltaTime) * playbackSpeed;
      if (loop && currentTime > maxTime) {
        currentTime = 0.0f;
      } else if (!loop && currentTime > maxTime) {
        currentTime = maxTime;
        playing = false;
      }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Timeline control panel.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(io.DisplaySize.x), 0));
    ImGui::Begin("Animation Controls", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

    // Play/Pause/Stop buttons.
    if (ImGui::Button(playing ? "Pause" : "Play")) {
      playing = !playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
      playing = false;
      currentTime = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step <<")) {
      playing = false;
      currentTime = std::max(0.0f, currentTime - stepSize);
    }
    ImGui::SameLine();
    if (ImGui::Button("Step >>")) {
      playing = false;
      currentTime = std::min(maxTime, currentTime + stepSize);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &loop);

    // Timeline scrubber.
    ImGui::SliderFloat("Time", &currentTime, 0.0f, maxTime, "%.3f s");

    // Speed and duration controls.
    ImGui::SliderFloat("Speed", &playbackSpeed, 0.1f, 5.0f, "%.1fx");
    ImGui::SameLine();
    ImGui::InputFloat("Duration", &maxTime, 1.0f, 5.0f, "%.1f s");

    ImGui::Text("%.1f FPS | Time: %.3f s", io.Framerate, currentTime);

    float controlPanelHeight = ImGui::GetWindowSize().y;
    ImGui::End();

    // Set animation time and render.
    document.setTime(static_cast<double>(currentTime));

    float svgAreaHeight = io.DisplaySize.y - controlPanelHeight;
    if (svgAreaHeight > 0) {
      document.setCanvasSize(static_cast<int>(io.DisplaySize.x),
                             static_cast<int>(svgAreaHeight));
    }

    renderer.draw(document);
    const RendererBitmap bitmap = renderer.takeSnapshot();
    if (!bitmap.empty()) {
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, bitmap.pixels.data());
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    // SVG rendering area.
    ImGui::SetNextWindowPos(ImVec2(0, controlPanelHeight));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, svgAreaHeight));
    ImGui::Begin("SVG", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    ImGui::Image(static_cast<ImTextureID>(texture),
                 ImVec2(static_cast<float>(renderer.width()),
                        static_cast<float>(renderer.height())));
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
