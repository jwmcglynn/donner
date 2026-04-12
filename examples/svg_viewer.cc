/**
 * @file svg_viewer.cc
 * @brief Minimal WASM-compatible SVG viewer using ImGui + Donner.
 *
 * Two-panel UI: left panel is an SVG source code editor, right panel shows
 * the rendered result. Re-parses and re-renders on every edit.
 *
 * Builds natively (with glad for GL loading) and as WASM (with Emscripten's
 * built-in WebGL / GLFW3 support).
 */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <GLES3/gl3.h>
#else
#include <glad/glad.h>
#endif

#include <GLFW/glfw3.h>

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererTinySkia.h"

namespace {

// Default SVG shown on startup.
constexpr const char* kDefaultSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
  <rect fill="#2d2d30" width="200" height="200"/>
  <rect x="30" y="30" width="80" height="80" rx="8" fill="#e74c3c"/>
  <circle cx="130" cy="70" r="40" fill="#3498db"/>
  <polygon points="100,120 140,190 60,190" fill="#2ecc71"/>
  <line x1="10" y1="10" x2="190" y2="190" stroke="#f1c40f" stroke-width="3"/>
  <text x="100" y="110" text-anchor="middle" font-size="16" fill="white">Donner SVG</text>
</svg>)";

constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 720;
constexpr int kSvgBufferSize = 64 * 1024;  // 64 KiB text buffer

struct AppState {
  GLFWwindow* window = nullptr;
  GLuint textureId = 0;
  int textureWidth = 0;
  int textureHeight = 0;

  std::vector<char> svgBuffer;
  std::string lastRenderedSvg;
  std::string parseError;

  bool needsRender = true;
};

void glfwErrorCallback(int error, const char* description) {
  std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

void uploadBitmap(AppState& state, const donner::svg::RendererBitmap& bitmap) {
  if (bitmap.pixels.empty() || bitmap.dimensions.x <= 0 || bitmap.dimensions.y <= 0) {
    return;
  }

  if (state.textureId == 0) {
    glGenTextures(1, &state.textureId);
  }
  glBindTexture(GL_TEXTURE_2D, state.textureId);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#ifndef __EMSCRIPTEN__
  const int strideInPixels =
      bitmap.rowBytes > 0 ? static_cast<int>(bitmap.rowBytes / 4) : bitmap.dimensions.x;
  glPixelStorei(GL_UNPACK_ROW_LENGTH, strideInPixels);
#endif
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, bitmap.pixels.data());
#ifndef __EMSCRIPTEN__
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif

  state.textureWidth = bitmap.dimensions.x;
  state.textureHeight = bitmap.dimensions.y;
}

void renderSvg(AppState& state) {
  using namespace donner;
  using namespace donner::svg;
  using namespace donner::svg::parser;

  std::string svgSource(state.svgBuffer.data());
  if (svgSource == state.lastRenderedSvg) {
    return;  // No change.
  }
  state.lastRenderedSvg = svgSource;
  state.parseError.clear();

  ParseWarningSink warnings;
  ParseResult<SVGDocument> maybeDoc = SVGParser::ParseSVG(svgSource, warnings);
  if (maybeDoc.hasError()) {
    state.parseError = "Parse error: " + std::string(maybeDoc.error().reason);
    return;
  }

  SVGDocument doc = std::move(maybeDoc.result());
  doc.setCanvasSize(800, 600);

  RendererTinySkia renderer;
  renderer.draw(doc);

  RendererBitmap bitmap = renderer.takeSnapshot();
  if (!bitmap.empty()) {
    uploadBitmap(state, bitmap);
  }
}

void mainLoopIteration(void* arg) {
  auto* state = static_cast<AppState*>(arg);

#ifndef __EMSCRIPTEN__
  if (glfwWindowShouldClose(state->window)) {
    return;
  }
#endif

  glfwPollEvents();

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  // Full-window docking area.
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("##MainWindow", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  const float availWidth = ImGui::GetContentRegionAvail().x;
  const float panelWidth = availWidth * 0.45f;

  // Left panel — SVG source editor.
  ImGui::BeginChild("##CodePanel", ImVec2(panelWidth, 0), ImGuiChildFlags_Borders);
  ImGui::Text("SVG Source");
  ImGui::Separator();

  const float textHeight = ImGui::GetContentRegionAvail().y - 40.0f;
  if (ImGui::InputTextMultiline("##SvgSource", state->svgBuffer.data(),
                                static_cast<size_t>(kSvgBufferSize),
                                ImVec2(-FLT_MIN, textHeight),
                                ImGuiInputTextFlags_AllowTabInput)) {
    state->needsRender = true;
  }

  if (!state->parseError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
    ImGui::TextWrapped("%s", state->parseError.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndChild();

  ImGui::SameLine();

  // Right panel — rendered output.
  ImGui::BeginChild("##RenderPanel", ImVec2(0, 0), ImGuiChildFlags_Borders);
  ImGui::Text("Rendered Output (%dx%d)", state->textureWidth, state->textureHeight);
  ImGui::Separator();

  if (state->needsRender) {
    renderSvg(*state);
    state->needsRender = false;
  }

  if (state->textureId != 0) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // Fit the texture in the available space maintaining aspect ratio.
    float displayW = static_cast<float>(state->textureWidth);
    float displayH = static_cast<float>(state->textureHeight);
    if (displayW > 0 && displayH > 0) {
      const float scaleX = avail.x / displayW;
      const float scaleY = avail.y / displayH;
      const float scale = (scaleX < scaleY) ? scaleX : scaleY;
      if (scale < 1.0f) {
        displayW *= scale;
        displayH *= scale;
      }
    }
    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(state->textureId)),
                 ImVec2(displayW, displayH));
  }
  ImGui::EndChild();

  ImGui::End();

  // Render.
  ImGui::Render();
  int displayW = 0;
  int displayH = 0;
  glfwGetFramebufferSize(state->window, &displayW, &displayH);
  glViewport(0, 0, displayW, displayH);
  glClearColor(0.11f, 0.11f, 0.13f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glfwSwapBuffers(state->window);
}

}  // namespace

int main() {
  glfwSetErrorCallback(&glfwErrorCallback);
  if (glfwInit() == GLFW_FALSE) {
    std::fprintf(stderr, "glfwInit() failed\n");
    return 1;
  }

#ifdef __EMSCRIPTEN__
  // Emscripten provides WebGL 2.0 context via USE_WEBGL2=1.
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#else
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
#endif

  GLFWwindow* window =
      glfwCreateWindow(kInitialWidth, kInitialHeight, "Donner SVG Viewer", nullptr, nullptr);
  if (window == nullptr) {
    std::fprintf(stderr, "glfwCreateWindow() failed\n");
    glfwTerminate();
    return 1;
  }

  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

#ifndef __EMSCRIPTEN__
  if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
    std::fprintf(stderr, "glad failed to load GL symbols\n");
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }
#endif

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;

  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL(window, true);

#ifdef __EMSCRIPTEN__
  ImGui_ImplOpenGL3_Init("#version 300 es");
#else
  ImGui_ImplOpenGL3_Init("#version 330 core");
#endif

  // Initialize app state.
  AppState state;
  state.window = window;
  state.svgBuffer.resize(kSvgBufferSize, '\0');
  std::strncpy(state.svgBuffer.data(), kDefaultSvg, kSvgBufferSize - 1);

  std::printf("Donner SVG Viewer started.\n");

#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop_arg(mainLoopIteration, &state, 0, true);
#else
  while (!glfwWindowShouldClose(window)) {
    mainLoopIteration(&state);
  }
#endif

  // Cleanup.
  if (state.textureId != 0) {
    glDeleteTextures(1, &state.textureId);
  }
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
