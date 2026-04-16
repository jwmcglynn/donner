#include "donner/editor/gui/EditorWindow.h"

#include <glad/glad.h>
// glad must be included before GLFW so it takes precedence.
#include <GLFW/glfw3.h>

#include <cstdio>

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "imgui.h"

namespace donner::editor::gui {

namespace {

void GlfwErrorCallback(int error, const char* description) {
  std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

}  // namespace

EditorWindow::EditorWindow(EditorWindowOptions options) : options_(std::move(options)) {
  glfwSetErrorCallback(&GlfwErrorCallback);

  if (glfwInit() == GLFW_FALSE) {
    std::fprintf(stderr, "EditorWindow: glfwInit() failed\n");
    return;
  }

  // OpenGL 3.3 core is plenty — matches what imgui_impl_opengl3 targets
  // by default and what glad was generated for.
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

  window_ = glfwCreateWindow(options_.initialWidth, options_.initialHeight, options_.title.c_str(),
                             /*monitor=*/nullptr,
                             /*share=*/nullptr);
  if (window_ == nullptr) {
    std::fprintf(stderr, "EditorWindow: glfwCreateWindow() failed\n");
    glfwTerminate();
    return;
  }

  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);  // vsync

  if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
    std::fprintf(stderr, "EditorWindow: glad failed to load GL symbols\n");
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    return;
  }

  // Dear ImGui setup. Matches the canonical example from the imgui docs.
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;  // no persistent layout file on disk

  ImGui::StyleColorsDark();
  if (!ImGui_ImplGlfw_InitForOpenGL(window_, /*install_callbacks=*/true)) {
    std::fprintf(stderr, "EditorWindow: ImGui_ImplGlfw_InitForOpenGL failed\n");
    return;
  }
  if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
    std::fprintf(stderr, "EditorWindow: ImGui_ImplOpenGL3_Init failed\n");
    return;
  }
  imguiInitialized_ = true;
  valid_ = true;
}

EditorWindow::~EditorWindow() {
  if (imguiInitialized_) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
  }
  if (textureId_ != 0) {
    glDeleteTextures(1, &textureId_);
    textureId_ = 0;
  }
  if (window_ != nullptr) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
  glfwTerminate();
}

bool EditorWindow::shouldClose() const {
  return window_ == nullptr || glfwWindowShouldClose(window_) != 0;
}

void EditorWindow::setTitle(std::string_view title) {
  if (window_ == nullptr) {
    return;
  }

  glfwSetWindowTitle(window_, std::string(title).c_str());
}

Vector2i EditorWindow::windowSize() const {
  if (window_ == nullptr) {
    return Vector2i::Zero();
  }

  int width = 0;
  int height = 0;
  glfwGetWindowSize(window_, &width, &height);
  return Vector2i(width, height);
}

Vector2d EditorWindow::contentScale() const {
  if (window_ == nullptr) {
    return Vector2d::Zero();
  }

  float xScale = 1.0f;
  float yScale = 1.0f;
  glfwGetWindowContentScale(window_, &xScale, &yScale);
  return Vector2d(xScale, yScale);
}

void EditorWindow::setUserPointer(void* pointer) {
  if (window_ == nullptr) {
    return;
  }

  glfwSetWindowUserPointer(window_, pointer);
}

GLFWscrollfun EditorWindow::setScrollCallback(GLFWscrollfun callback) {
  if (window_ == nullptr) {
    return nullptr;
  }

  return glfwSetScrollCallback(window_, callback);
}

void EditorWindow::pollEvents() {
  glfwPollEvents();
}

void EditorWindow::beginFrame() {
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
}

void EditorWindow::endFrame() {
  ImGui::Render();
  int displayW = 0;
  int displayH = 0;
  glfwGetFramebufferSize(window_, &displayW, &displayH);
  glViewport(0, 0, displayW, displayH);
  glClearColor(options_.clearColor[0], options_.clearColor[1], options_.clearColor[2],
               options_.clearColor[3]);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glfwSwapBuffers(window_);
}

void EditorWindow::uploadBitmap(const svg::RendererBitmap& bitmap) {
  if (bitmap.pixels.empty() || bitmap.dimensions.x <= 0 || bitmap.dimensions.y <= 0) {
    return;
  }

  if (textureId_ == 0) {
    glGenTextures(1, &textureId_);
  }
  glBindTexture(GL_TEXTURE_2D, textureId_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  const int strideInPixels =
      bitmap.rowBytes > 0 ? static_cast<int>(bitmap.rowBytes / 4) : bitmap.dimensions.x;
  glPixelStorei(GL_UNPACK_ROW_LENGTH, strideInPixels);
  glTexImage2D(GL_TEXTURE_2D, /*level=*/0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y,
               /*border=*/0, GL_RGBA, GL_UNSIGNED_BYTE, bitmap.pixels.data());
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  textureWidth_ = bitmap.dimensions.x;
  textureHeight_ = bitmap.dimensions.y;
}

}  // namespace donner::editor::gui
