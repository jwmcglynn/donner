#include "donner/editor/gui/EditorWindow.h"

#include <algorithm>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#define GLFW_INCLUDE_ES3
#include <GLES3/gl3.h>

#include "GLFW/emscripten_glfw3.h"
#else
#include <glad/glad.h>
// glad must be included before GLFW so it takes precedence.
#include <GLFW/glfw3.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "donner/editor/ImGuiBackendIncludes.h"
#include "donner/editor/TracyWrapper.h"

namespace donner::editor::gui {

namespace {

void GlfwErrorCallback(int error, const char* description) {
#ifdef __EMSCRIPTEN__
  // emscripten-glfw surfaces benign shim-limitation messages through the
  // error callback with a `[Warning]` prefix — e.g. ImGui's backend calls
  // `glfwSetWindowAttrib(GLFW_MOUSE_PASSTHROUGH)` every frame, which the
  // shim can't honor. Drop those so the console only shows real errors.
  if (description != nullptr && std::string_view(description).substr(0, 9) == "[Warning]") {
    return;
  }
#endif
  std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

void ApplyInputOverride(const EditorWindowInputOverride& inputOverride) {
  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = static_cast<float>(std::max(0.001, inputOverride.deltaSeconds));
  const float mouseX = static_cast<float>(inputOverride.mousePosition.x);
  const float mouseY = static_cast<float>(inputOverride.mousePosition.y);
  io.MousePos = ImVec2(mouseX, mouseY);
  io.AddMousePosEvent(mouseX, mouseY);
  io.AddFocusEvent(true);
  for (int i = 0;
       i < static_cast<int>(inputOverride.mouseDown.size()) && i < IM_ARRAYSIZE(io.MouseDown);
       ++i) {
    io.MouseDown[i] = inputOverride.mouseDown[i];
    io.AddMouseButtonEvent(i, inputOverride.mouseDown[i]);
  }
  io.KeyCtrl = inputOverride.keyCtrl;
  io.KeyShift = inputOverride.keyShift;
  io.KeyAlt = inputOverride.keyAlt;
  io.KeySuper = inputOverride.keySuper;
  io.AddKeyEvent(ImGuiKey_LeftCtrl, inputOverride.keyCtrl);
  io.AddKeyEvent(ImGuiKey_LeftShift, inputOverride.keyShift);
  io.AddKeyEvent(ImGuiKey_LeftAlt, inputOverride.keyAlt);
  io.AddKeyEvent(ImGuiKey_LeftSuper, inputOverride.keySuper);
  io.MouseWheelH = inputOverride.mouseWheelH;
  io.MouseWheel = inputOverride.mouseWheel;
  if (inputOverride.mouseWheelH != 0.0f || inputOverride.mouseWheel != 0.0f) {
    io.AddMouseWheelEvent(inputOverride.mouseWheelH, inputOverride.mouseWheel);
  }
}

#ifdef __EMSCRIPTEN__
EM_JS(int, CanvasPixelWidth, (), {
  if (Module.canvas) {
    return Module.canvas.width;
  }
  return Math.max(1, Math.floor(window.innerWidth * (window.devicePixelRatio || 1)));
});

EM_JS(int, CanvasPixelHeight, (), {
  if (Module.canvas) {
    return Module.canvas.height;
  }
  return Math.max(1, Math.floor(window.innerHeight * (window.devicePixelRatio || 1)));
});

EM_JS(int, CanvasCssWidth, (), { return Math.max(1, Math.floor(window.innerWidth)); });
EM_JS(double, BrowserDevicePixelRatio, (), { return window.devicePixelRatio || 1.0; });

double CurrentDisplayScale() {
  const int logicalWidth = CanvasCssWidth();
  const int framebufferWidth = CanvasPixelWidth();
  if (logicalWidth > 0 && framebufferWidth > 0) {
    return std::max(1.0, static_cast<double>(framebufferWidth) / static_cast<double>(logicalWidth));
  }
  return std::max(1.0, BrowserDevicePixelRatio());
}
#endif

}  // namespace

UiScaleConfig ComputeUiScaleConfig(int logicalWindowWidth, int framebufferWidth,
                                   double contentScaleX) {
  UiScaleConfig config;
  if (logicalWindowWidth > 0 && framebufferWidth > 0) {
    config.displayScale =
        static_cast<double>(framebufferWidth) / static_cast<double>(logicalWindowWidth);
  } else {
    config.displayScale = contentScaleX;
  }

  if (config.displayScale < 1.0) {
    config.displayScale = 1.0;
  }

  return config;
}

EditorWindow::EditorWindow(EditorWindowOptions options) : options_(std::move(options)) {
  glfwSetErrorCallback(&GlfwErrorCallback);

  bool useNullPlatform = false;
#ifdef __EMSCRIPTEN__
  glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_EMSCRIPTEN);
#else
  // Use GLFW's windowless "null" platform (OSMesa software GL) for offscreen
  // framebuffer-readback replay. On Linux we also fall back to it automatically
  // when there is no X11/Wayland display, so headless runs degrade gracefully;
  // other platforms only switch when offscreen is requested explicitly (their
  // display detection isn't a simple env var, and forcing null would break the
  // native path on real displays).
  useNullPlatform = options_.offscreen;
#if defined(__linux__)
  const bool hasDisplay =
      std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
  useNullPlatform = useNullPlatform || !hasDisplay;
#endif
  if (useNullPlatform) {
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
  }
#endif
  if (glfwInit() == GLFW_FALSE) {
    std::fprintf(stderr, "EditorWindow: glfwInit() failed\n");
    return;
  }

#ifdef __EMSCRIPTEN__
  // emscripten-glfw runs on WebGL2, not desktop GL, so neither the
  // version hints nor `GLFW_OPENGL_PROFILE` apply — setting them only
  // produces "Hint ... not currently supported on this platform"
  // warnings at startup.
  glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);
  emscripten_glfw_set_next_window_canvas_selector("#canvas");
#else
  // OpenGL 3.3 core is plenty — matches what imgui_impl_opengl3 targets
  // by default and what glad was generated for.
  glfwWindowHint(GLFW_VISIBLE, options_.visible ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
#endif

  const int initialWidth =
#ifdef __EMSCRIPTEN__
      CanvasPixelWidth();
#else
      options_.initialWidth;
#endif
  const int initialHeight =
#ifdef __EMSCRIPTEN__
      CanvasPixelHeight();
#else
      options_.initialHeight;
#endif
  // The null platform reports no HiDPI scale, so emulate the requested content
  // scale by allocating the framebuffer (== window size on the null platform) at
  // the scaled resolution. Real platforms apply HiDPI scaling themselves.
  const double offscreenScale = (useNullPlatform && options_.offscreenContentScale > 0.0)
                                    ? options_.offscreenContentScale
                                    : 1.0;
  const int createWidth = static_cast<int>(std::lround(initialWidth * offscreenScale));
  const int createHeight = static_cast<int>(std::lround(initialHeight * offscreenScale));
  window_ = glfwCreateWindow(createWidth, createHeight, options_.title.c_str(), /*monitor=*/nullptr,
                             /*share=*/nullptr);
  if (window_ == nullptr) {
    std::fprintf(stderr, "EditorWindow: glfwCreateWindow() failed\n");
    glfwTerminate();
    return;
  }

  glfwMakeContextCurrent(window_);
#ifdef __EMSCRIPTEN__
  emscripten_glfw_make_canvas_resizable(window_, "window", nullptr);
#else
  glfwSwapInterval(1);  // vsync

  if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
    std::fprintf(stderr, "EditorWindow: glad failed to load GL symbols\n");
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    return;
  }
#endif

  // Dear ImGui setup. Matches the canonical example from the imgui docs.
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;  // no persistent layout file on disk

  int logicalWindowWidth = 0;
  int logicalWindowHeight = 0;
  glfwGetWindowSize(window_, &logicalWindowWidth, &logicalWindowHeight);
  int framebufferWidth = 0;
  int framebufferHeight = 0;
#ifdef __EMSCRIPTEN__
  framebufferWidth = CanvasPixelWidth();
  framebufferHeight = CanvasPixelHeight();
#else
  glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
#endif
  if (offscreenScale != 1.0) {
    // Null platform: the framebuffer/window ratio is always 1, so derive the
    // display scale from the emulated value and reapply it every frame (see
    // beginFrameImpl) because ImGui_ImplGlfw_NewFrame would otherwise reset it.
    uiScaleConfig_.displayScale = offscreenScale;
    frameDisplayScaleOverride_ = offscreenScale;
  } else {
    const Vector2d scale = contentScale();
    uiScaleConfig_ = ComputeUiScaleConfig(logicalWindowWidth, framebufferWidth, scale.x);
  }
  io.DisplayFramebufferScale = ImVec2(static_cast<float>(uiScaleConfig_.displayScale),
                                      static_cast<float>(uiScaleConfig_.displayScale));
  io.FontGlobalScale = uiScaleConfig_.fontGlobalScale();

  ImGui::StyleColorsDark();
  if (!ImGui_ImplGlfw_InitForOpenGL(window_, /*install_callbacks=*/true)) {
    std::fprintf(stderr, "EditorWindow: ImGui_ImplGlfw_InitForOpenGL failed\n");
    return;
  }
#ifdef __EMSCRIPTEN__
  if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
#else
  if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
#endif
    std::fprintf(stderr, "EditorWindow: ImGui_ImplOpenGL3_Init failed\n");
    return;
  }
#ifdef __EMSCRIPTEN__
  ImGui_ImplGlfw_InstallEmscriptenCallbacks(window_, "#canvas");
#endif
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

#ifdef __EMSCRIPTEN__
  const double scale = CurrentDisplayScale();
  return Vector2d(scale, scale);
#else
  float xScale = 1.0f;
  float yScale = 1.0f;
  glfwGetWindowContentScale(window_, &xScale, &yScale);
  return Vector2d(xScale, yScale);
#endif
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

void EditorWindow::waitEvents() {
#ifdef __EMSCRIPTEN__
  // emscripten-glfw's `glfwWaitEvents` is a no-op; the browser drives
  // the main loop via `requestAnimationFrame`. Fall back to a regular
  // poll so the loop still processes queued input this tick.
  glfwPollEvents();
#else
  glfwWaitEvents();
#endif
}

void EditorWindow::wakeEventLoop() {
#ifdef __EMSCRIPTEN__
  // No-op — the browser's rAF cadence handles wake-ups implicitly.
#else
  glfwPostEmptyEvent();
#endif
}

void EditorWindow::beginFrame() {
  beginFrameImpl(nullptr);
}

void EditorWindow::beginFrameWithInput(const EditorWindowInputOverride& inputOverride) {
  beginFrameImpl(&inputOverride);
}

void EditorWindow::beginFrameImpl(const EditorWindowInputOverride* inputOverride) {
  ZoneScopedN("EditorWindow::beginFrame");
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  if (frameDisplayScaleOverride_ > 0.0) {
    // The null platform reports a 1:1 framebuffer/window ratio, so ImGui's GLFW
    // backend just reset DisplayFramebufferScale to 1. Restore the emulated
    // HiDPI scale and the matching logical DisplaySize for this frame.
    ImGuiIO& io = ImGui::GetIO();
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
    io.DisplaySize = ImVec2(static_cast<float>(framebufferWidth / frameDisplayScaleOverride_),
                            static_cast<float>(framebufferHeight / frameDisplayScaleOverride_));
    io.DisplayFramebufferScale = ImVec2(static_cast<float>(frameDisplayScaleOverride_),
                                        static_cast<float>(frameDisplayScaleOverride_));
  }
  if (inputOverride != nullptr) {
    ApplyInputOverride(*inputOverride);
  }
  ImGui::NewFrame();
}

void EditorWindow::endFrame() {
  endFrameImpl(nullptr);
}

svg::RendererBitmap EditorWindow::endFrameAndReadPixels() {
  svg::RendererBitmap readback;
  endFrameImpl(&readback);
  return readback;
}

void EditorWindow::endFrameImpl(svg::RendererBitmap* readback) {
  ZoneScopedN("EditorWindow::endFrame");
  {
    ZoneScopedN("ImGui::Render");
    ImGui::Render();
  }
  int displayW = 0;
  int displayH = 0;
#ifdef __EMSCRIPTEN__
  displayW = CanvasPixelWidth();
  displayH = CanvasPixelHeight();
#else
  glfwGetFramebufferSize(window_, &displayW, &displayH);
#endif
  glViewport(0, 0, displayW, displayH);
  glClearColor(options_.clearColor[0], options_.clearColor[1], options_.clearColor[2],
               options_.clearColor[3]);
  glClear(GL_COLOR_BUFFER_BIT);
  {
    ZoneScopedN("ImGui_ImplOpenGL3_RenderDrawData");
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  }
  if (readback != nullptr && displayW > 0 && displayH > 0) {
    ZoneScopedN("glReadPixels");
    constexpr int kChannels = 4;
    const std::size_t rowBytes = static_cast<std::size_t>(displayW) * kChannels;
    std::vector<uint8_t> bottomUp(rowBytes * static_cast<std::size_t>(displayH));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
#ifndef __EMSCRIPTEN__
    glReadBuffer(GL_BACK);
#endif
    glReadPixels(0, 0, displayW, displayH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
    readback->dimensions = Vector2i(displayW, displayH);
    readback->rowBytes = rowBytes;
    readback->alphaType = svg::AlphaType::Premultiplied;
    readback->pixels.resize(bottomUp.size());
    for (int y = 0; y < displayH; ++y) {
      const uint8_t* src = bottomUp.data() + static_cast<std::size_t>(displayH - 1 - y) * rowBytes;
      uint8_t* dst = readback->pixels.data() + static_cast<std::size_t>(y) * rowBytes;
      std::memcpy(dst, src, rowBytes);
    }
  }
#ifndef __EMSCRIPTEN__
  // emscripten-glfw intentionally doesn't implement `glfwSwapBuffers`;
  // the browser drives presentation via `requestAnimationFrame`.
  {
    ZoneScopedN("glfwSwapBuffers");
    glfwSwapBuffers(window_);
  }
#endif
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
