/// @file
///
/// `//donner/editor:editor` — the full-featured Donner editor binary.
///
/// This is the thin-client editor shell. It drives an `EditorBackendClient`
/// for all document operations (parse, selection, undo, export) and handles
/// only viewport, text editing, and chrome display locally.
///
/// Run with:
///
/// ```sh
/// bazel run //donner/editor -- donner_splash.svg
/// ```
///
/// For the minimal viewer demo (no tools, no overlay chrome, just a
/// rendered SVG and a text pane) see `//examples:svg_viewer`.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseDiagnostic.h"
#include "donner/base/Transform.h"
#include "donner/editor/EditorBackendClient.h"
#include "donner/editor/EditorIcon.h"
#include "donner/editor/EditorSplash.h"
#include "donner/editor/Notice.h"
#include "donner/editor/PinchEventMonitor.h"
#include "donner/editor/SelectionOverlay.h"
#include "donner/editor/TextBuffer.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/editor/ViewportState.h"

#ifndef __EMSCRIPTEN__
#include "donner/editor/sandbox/SandboxSession.h"
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#define GLFW_INCLUDE_ES3
#include <GLES3/gl3.h>

#include "GLFW/emscripten_glfw3.h"
#else
#include "glad/glad.h"
#endif

extern "C" {
#include "GLFW/glfw3.h"
}

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"

#include "embed_resources/FiraCodeFont.h"
#include "embed_resources/RobotoFont.h"

namespace {

#ifndef __EMSCRIPTEN__
constexpr int kInitialWindowWidth = 1600;
constexpr int kInitialWindowHeight = 900;
#endif
constexpr float kInitialMainLeftDockFraction = 0.78f;
constexpr float kInitialSourceDockFraction = 0.45f;
constexpr float kInitialTreeDockFraction = 0.4f;
constexpr float kWheelZoomStep = 1.1f;
constexpr float kKeyboardZoomStep = 1.5f;
constexpr double kTrackpadPanPixelsPerScrollUnit = 10.0;
constexpr float kTextChangeDebounceSeconds = 0.15f;
constexpr std::size_t kFrameHistoryCapacity = 120;
constexpr float kTargetFrameMs = 1000.0f / 60.0f;
constexpr float kFrameGraphWidth = 240.0f;
constexpr float kFrameGraphHeight = 32.0f;
constexpr ImU32 kSelectionChromeColor = IM_COL32(0x00, 0xc8, 0xff, 0xff);
constexpr float kSelectionChromeThickness = 1.5f;
constexpr ImU32 kMarqueeFillColor = IM_COL32(0x00, 0xc8, 0xff, 0x33);
constexpr ImU32 kMarqueeStrokeColor = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr float kMarqueeStrokeThickness = 1.5f;
#ifdef __EMSCRIPTEN__
constexpr int kMaxWasmUploadBytes = 32 * 1024 * 1024;
#endif

/// Fixed-size ring buffer of recent frame times (in milliseconds).
struct FrameHistory {
  std::array<float, kFrameHistoryCapacity> deltaMs{};
  std::size_t writeIndex = 0;
  std::size_t samples = 0;

  void push(float ms) {
    deltaMs[writeIndex] = ms;
    writeIndex = (writeIndex + 1) % kFrameHistoryCapacity;
    if (samples < kFrameHistoryCapacity) {
      ++samples;
    }
  }

  [[nodiscard]] float latest() const {
    if (samples == 0) {
      return 0.0f;
    }
    const std::size_t idx = (writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
    return deltaMs[idx];
  }

  [[nodiscard]] float max() const {
    float m = 0.0f;
    for (std::size_t i = 0; i < samples; ++i) {
      m = std::max(m, deltaMs[i]);
    }
    return m;
  }
};

/// Raw scroll events queued from GLFW until the next ImGui frame.
struct PendingScrollEvents {
  GLFWscrollfun previousCallback = nullptr;
  std::vector<donner::editor::RenderPaneScrollEvent> events;
};

[[nodiscard]] bool IsZoomModifierHeld(GLFWwindow* window) {
  return glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
}

void EditorScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
  auto* state = static_cast<PendingScrollEvents*>(glfwGetWindowUserPointer(window));
  if (state == nullptr) {
    return;
  }

  if (state->previousCallback != nullptr) {
    state->previousCallback(window, xoffset, yoffset);
  }

  double cursorX = 0.0;
  double cursorY = 0.0;
  glfwGetCursorPos(window, &cursorX, &cursorY);
  state->events.push_back(donner::editor::RenderPaneScrollEvent{
      .scrollDelta = donner::Vector2d(xoffset, yoffset),
      .cursorScreen = donner::Vector2d(cursorX, cursorY),
      .zoomModifierHeld = IsZoomModifierHeld(window),
  });
}

void RenderFrameGraph(const FrameHistory& history) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float latestMs = history.latest();
  static float msEma = 0.0f;
  static double lastDisplayUpdateTime = 0.0;
  static float displayedMs = 0.0f;

  msEma = msEma == 0.0f ? latestMs : 0.9f * msEma + 0.1f * latestMs;
  const double now = ImGui::GetTime();
  if (displayedMs == 0.0f || now - lastDisplayUpdateTime > 0.25) {
    displayedMs = msEma;
    lastDisplayUpdateTime = now;
  }
  const float displayedFps = displayedMs > 0.0f ? 1000.0f / displayedMs : 0.0f;

  const ImVec2 bottomRight(origin.x + kFrameGraphWidth, origin.y + kFrameGraphHeight);
  dl->AddRectFilled(origin, bottomRight, IM_COL32(30, 30, 30, 255));

  constexpr float scaleMs = kTargetFrameMs * 2.0f;
  const float budgetY =
      origin.y + kFrameGraphHeight - (kTargetFrameMs / scaleMs) * kFrameGraphHeight;
  dl->AddLine(ImVec2(origin.x, budgetY), ImVec2(bottomRight.x, budgetY),
              IM_COL32(255, 255, 255, 80), 1.0f);

  const float barWidth = kFrameGraphWidth / static_cast<float>(kFrameHistoryCapacity);
  for (std::size_t i = 0; i < history.samples; ++i) {
    const std::size_t readIdx =
        (history.writeIndex + kFrameHistoryCapacity - history.samples + i) % kFrameHistoryCapacity;
    const float ms = history.deltaMs[readIdx];
    const float clamped = std::min(ms, scaleMs);
    const float barHeight = (clamped / scaleMs) * kFrameGraphHeight;
    const bool overBudget = ms > kTargetFrameMs;
    const ImU32 color = overBudget ? IM_COL32(220, 60, 60, 255) : IM_COL32(80, 200, 100, 255);

    const float x = origin.x + static_cast<float>(i) * barWidth;
    dl->AddRectFilled(ImVec2(x, origin.y + kFrameGraphHeight - barHeight),
                      ImVec2(x + barWidth, origin.y + kFrameGraphHeight), color);
  }

  ImGui::Dummy(ImVec2(kFrameGraphWidth, kFrameGraphHeight));
  const ImU32 textColor =
      displayedMs > kTargetFrameMs ? IM_COL32(220, 60, 60, 255) : IM_COL32(255, 255, 255, 255);
  ImGui::PushStyleColor(ImGuiCol_Text, textColor);
  ImGui::Text("%.2f ms / %.1f FPS", displayedMs, displayedFps);
  ImGui::PopStyleColor();
}

void GlfwErrorCallback(int error, const char* description) {
  std::cerr << "GLFW error " << error << ": " << description << "\n";
}

// clang-format off
#ifdef __EMSCRIPTEN__
std::optional<std::string> gPendingBrowserUploadPath;

extern "C" void OnBrowserFileReadyPath(const char* path) {
  if (path != nullptr && path[0] != '\0') {
    gPendingBrowserUploadPath = std::string(path);
  }
}

EM_JS(int, canvasPixelWidth, (), {
  if (Module.canvas) {
    return Module.canvas.width;
  }
  return Math.max(1, Math.floor(window.innerWidth * (window.devicePixelRatio || 1)));
});

EM_JS(int, canvasPixelHeight, (), {
  if (Module.canvas) {
    return Module.canvas.height;
  }
  return Math.max(1, Math.floor(window.innerHeight * (window.devicePixelRatio || 1)));
});

EM_JS(int, canvasCssWidth, (), { return Math.max(1, Math.floor(window.innerWidth)); });

EM_JS(int, canvasCssHeight, (), { return Math.max(1, Math.floor(window.innerHeight)); });

EM_JS(double, browserDevicePixelRatio, (), { return window.devicePixelRatio || 1.0; });

double CurrentDisplayScale() {
  const int logicalWidth = canvasCssWidth();
  const int framebufferWidth = canvasPixelWidth();
  if (logicalWidth > 0 && framebufferWidth > 0) {
    return std::max(1.0, static_cast<double>(framebufferWidth) / static_cast<double>(logicalWidth));
  }
  return std::max(1.0, browserDevicePixelRatio());
}

EM_JS(void, ShowBrowserOpenFileDialog, (int maxBytes), {
  const input = document.createElement("input");
  input.type = "file";
  input.accept = "image/svg+xml,.svg,text/plain,.txt";
  input.onchange = () => {
    if (!input.files || input.files.length === 0) {
      input.remove();
      return;
    }

    const file = input.files[0];
    if (file.size > maxBytes) {
      alert(`File is too large. Limit is ${Math.floor(maxBytes / (1024 * 1024))} MiB.`);
      input.remove();
      return;
    }

    const reader = new FileReader();
    reader.onload = () => {
      FS.writeFile("/tmp/upload.svg", reader.result);
      ccall("OnBrowserFileReadyPath", null, ["string"], ["/tmp/upload.svg"]);
      input.remove();
    };
    reader.readAsText(file);
  };
  input.click();
});

EM_JS(void, InstallBrowserDropHandler, (int maxBytes), {
  const canvas = Module.canvas || document.getElementById("canvas");
  if (!canvas || canvas.__donnerDropInstalled) {
    return;
  }

  const rejectOversizedFile = file => {
    if (file.size > maxBytes) {
      alert(`File is too large. Limit is ${Math.floor(maxBytes / (1024 * 1024))} MiB.`);
      return true;
    }
    return false;
  };

  canvas.addEventListener("dragover", event => { event.preventDefault(); });

  canvas.addEventListener(
      "drop", event => {
        event.preventDefault();
        if (!event.dataTransfer || !event.dataTransfer.files ||
                event.dataTransfer.files.length === 0) {
          return;
        }

        const file = event.dataTransfer.files[0];
        if (rejectOversizedFile(file)) {
          return;
        }

        const reader = new FileReader();
        reader.onload = () => {
          FS.writeFile("/tmp/upload.svg", reader.result);
          ccall("OnBrowserFileReadyPath", null, ["string"], ["/tmp/upload.svg"]);
        };
        reader.readAsText(file);
      });

  canvas.__donnerDropInstalled = true;
});
#endif
// clang-format on

/// Build a `TextEditor::ErrorMarkers` map from a parser diagnostic.
donner::editor::TextEditor::ErrorMarkers ParseErrorToMarkers(const donner::ParseDiagnostic& diag) {
  donner::editor::TextEditor::ErrorMarkers markers;
  const auto& start = diag.range.start;
  const int line = start.lineInfo.has_value() ? static_cast<int>(start.lineInfo->line) : 1;
  markers.emplace(line, std::string(std::string_view(diag.reason)));
  return markers;
}

#ifndef __EMSCRIPTEN__
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
#endif

std::string EmbeddedBytesToString(std::span<const unsigned char> bytes) {
  std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!text.empty() && text.back() == '\0') {
    text.pop_back();
  }
  return text;
}

/// Process a FrameResult: upload bitmap to texture, apply writebacks to
/// text editor, update error markers. Returns true if a bitmap was uploaded.
bool ProcessFrameResult(const donner::editor::FrameResult& result, GLuint texture,
                        int& textureWidth, int& textureHeight,
                        donner::editor::TextEditor& textEditor, int& lastShownErrorLine,
                        std::string& lastShownErrorReason) {
  if (!result.ok) {
    return false;
  }

  bool bitmapUploaded = false;
  if (!result.bitmap.empty()) {
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(result.bitmap.rowBytes / 4u));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, result.bitmap.dimensions.x,
                 result.bitmap.dimensions.y, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 result.bitmap.pixels.data());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    textureWidth = result.bitmap.dimensions.x;
    textureHeight = result.bitmap.dimensions.y;
    bitmapUploaded = true;
  }

  // Apply source writebacks (e.g. from drag completion).
  for (const auto& wb : result.writebacks) {
    std::string source = textEditor.getText();
    if (wb.sourceStart <= source.size() && wb.sourceEnd <= source.size() &&
        wb.sourceStart <= wb.sourceEnd) {
      source.replace(wb.sourceStart, wb.sourceEnd - wb.sourceStart, wb.newText);
      textEditor.setText(source, /*preserveScroll=*/true);
    }
  }

  // Full source replacement (undo/redo).
  if (result.sourceReplaceAll.has_value()) {
    textEditor.setText(*result.sourceReplaceAll, /*preserveScroll=*/true);
  }

  // Error markers.
  constexpr int kNoErrorLine = -1;
  if (!result.parseDiagnostics.empty()) {
    const auto& diag = result.parseDiagnostics.front();
    const int line = diag.range.start.lineInfo.has_value()
                         ? static_cast<int>(diag.range.start.lineInfo->line)
                         : 1;
    const std::string_view reasonSv = diag.reason;
    if (line != lastShownErrorLine || reasonSv != lastShownErrorReason) {
      textEditor.setErrorMarkers(ParseErrorToMarkers(diag));
      lastShownErrorLine = line;
      lastShownErrorReason.assign(reasonSv);
    }
  } else if (lastShownErrorLine != kNoErrorLine) {
    textEditor.setErrorMarkers({});
    lastShownErrorLine = kNoErrorLine;
    lastShownErrorReason.clear();
  }

  return bitmapUploaded;
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

#ifdef __EMSCRIPTEN__
  const std::string initialSource = EmbeddedBytesToString(donner::embedded::kEditorIconSvg);
  const std::optional<std::string> initialPath = std::string("donner_icon.svg");
#else
  if (argc != 2) {
    std::cerr << "Usage: donner-editor <filename>\n";
    return 1;
  }

  const std::string svgPath = argv[1];
  const std::string initialSource = LoadFile(svgPath);
  if (initialSource.empty()) {
    return 1;
  }
  const std::optional<std::string> initialPath = svgPath;
#endif

  // ---------------------------------------------------------------------------
  // GLFW + OpenGL
  // ---------------------------------------------------------------------------
  glfwSetErrorCallback(GlfwErrorCallback);
  if (!glfwInit()) {
    std::cerr << "Failed to initialize GLFW\n";
    return 1;
  }

#ifdef __EMSCRIPTEN__
  glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_EMSCRIPTEN);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);
#else
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

#ifdef __EMSCRIPTEN__
  emscripten_glfw_set_next_window_canvas_selector("#canvas");
#endif
  const int initialWindowWidth =
#ifdef __EMSCRIPTEN__
      canvasPixelWidth();
#else
      kInitialWindowWidth;
#endif
  const int initialWindowHeight =
#ifdef __EMSCRIPTEN__
      canvasPixelHeight();
#else
      kInitialWindowHeight;
#endif
  GLFWwindow* window =
      glfwCreateWindow(initialWindowWidth, initialWindowHeight, "Donner Editor", nullptr, nullptr);
  if (!window) {
    std::cerr << "Failed to create GLFW window\n";
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
#ifdef __EMSCRIPTEN__
  emscripten_glfw_make_canvas_resizable(window, "window", nullptr);
#endif
#ifndef __EMSCRIPTEN__
  glfwSwapInterval(1);

  if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
    std::cerr << "Failed to initialize OpenGL loader\n";
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }
#endif

  // ---------------------------------------------------------------------------
  // ImGui
  // ---------------------------------------------------------------------------
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  double initialDisplayScale = 1.0;
#ifdef __EMSCRIPTEN__
  initialDisplayScale = CurrentDisplayScale();
#else
  {
    int logicalWindowWidth = 0;
    int logicalWindowHeight = 0;
    glfwGetWindowSize(window, &logicalWindowWidth, &logicalWindowHeight);

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    if (logicalWindowWidth > 0 && framebufferWidth > 0) {
      initialDisplayScale =
          static_cast<double>(framebufferWidth) / static_cast<double>(logicalWindowWidth);
    }
  }
#endif
  initialDisplayScale = std::max(1.0, initialDisplayScale);
  ImFontConfig fontCfg;
  fontCfg.FontDataOwnedByAtlas = false;
  ImFont* uiFontRegular = io.Fonts->AddFontFromMemoryTTF(
      const_cast<unsigned char*>(donner::embedded::kRobotoRegularTtf.data()),
      static_cast<int>(donner::embedded::kRobotoRegularTtf.size()),
      /*size_pixels=*/static_cast<float>(15.0 * initialDisplayScale), &fontCfg);
  ImFont* uiFontBold = io.Fonts->AddFontFromMemoryTTF(
      const_cast<unsigned char*>(donner::embedded::kRobotoBoldTtf.data()),
      static_cast<int>(donner::embedded::kRobotoBoldTtf.size()),
      /*size_pixels=*/static_cast<float>(15.0 * initialDisplayScale), &fontCfg);
  ImFont* codeFont = io.Fonts->AddFontFromMemoryTTF(
      const_cast<unsigned char*>(donner::embedded::kFiraCodeRegularTtf.data()),
      static_cast<int>(donner::embedded::kFiraCodeRegularTtf.size()),
      /*size_pixels=*/static_cast<float>(14.0 * initialDisplayScale), &fontCfg);
  (void)uiFontRegular;
  (void)uiFontBold;
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.DisplayFramebufferScale =
      ImVec2(static_cast<float>(initialDisplayScale), static_cast<float>(initialDisplayScale));
  io.FontGlobalScale = static_cast<float>(1.0 / initialDisplayScale);
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __EMSCRIPTEN__
  ImGui_ImplOpenGL3_Init("#version 300 es");
  ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#else
  ImGui_ImplOpenGL3_Init("#version 330");
#endif

  PendingScrollEvents pendingScrollEvents;
  glfwSetWindowUserPointer(window, &pendingScrollEvents);
  pendingScrollEvents.previousCallback = glfwSetScrollCallback(window, EditorScrollCallback);
  (void)donner::editor::InstallPinchEventMonitor(window, &pendingScrollEvents.events,
                                                  kWheelZoomStep);
#ifdef __EMSCRIPTEN__
  InstallBrowserDropHandler(kMaxWasmUploadBytes);
#endif

  // ---------------------------------------------------------------------------
  // Editor state
  // ---------------------------------------------------------------------------
  using SteadyClock = std::chrono::steady_clock;
  const auto editorStart = SteadyClock::now();
  const auto msSinceStart = [&editorStart]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - editorStart)
        .count();
  };

  // Create the backend client.
#ifdef __EMSCRIPTEN__
  auto backend = donner::editor::EditorBackendClient::MakeInProcess();
#else
  donner::editor::sandbox::SandboxSession session(
      donner::editor::sandbox::SandboxSessionOptions{
          .childBinaryPath = "donner/editor/sandbox/donner_editor_backend",
      });
  auto backend = donner::editor::EditorBackendClient::MakeSessionBacked(session);
#endif

  // Load the initial document.
  {
    auto loadFuture = backend->loadBytes(
        std::span(reinterpret_cast<const uint8_t*>(initialSource.data()), initialSource.size()),
        initialPath);
    auto loadResult = loadFuture.get();
    if (!loadResult.ok) {
      std::cerr << "Failed to parse initial SVG";
      if (initialPath.has_value()) {
        std::cerr << ": " << *initialPath;
      }
      std::cerr << "\n";
    }
  }
  std::cerr << "[startup] +" << msSinceStart() << "ms loadBytes done\n";

  donner::editor::TextEditor textEditor;
  textEditor.setLanguageDefinition(donner::editor::TextEditor::LanguageDefinition::SVG());
  textEditor.setText(initialSource);
  textEditor.resetTextChanged();
  textEditor.setActiveAutocomplete(true);
  std::string editorNoticeText = EmbeddedBytesToString(donner::embedded::kEditorNoticeText);

  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  int textureWidth = 0;
  int textureHeight = 0;

  // Upload the initial frame bitmap if available.
  {
    const auto& bitmap = backend->latestBitmap();
    if (!bitmap.empty()) {
      glBindTexture(GL_TEXTURE_2D, texture);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, bitmap.pixels.data());
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      textureWidth = bitmap.dimensions.x;
      textureHeight = bitmap.dimensions.y;
    }
  }

  // Surface initial parse error.
  constexpr int kNoErrorLine = -1;
  int lastShownErrorLine = kNoErrorLine;
  std::string lastShownErrorReason;
  if (auto initialErr = backend->lastParseError(); initialErr.has_value()) {
    textEditor.setErrorMarkers(ParseErrorToMarkers(*initialErr));
    lastShownErrorLine = initialErr->range.start.lineInfo.has_value()
                             ? static_cast<int>(initialErr->range.start.lineInfo->line)
                             : 1;
    lastShownErrorReason = std::string(std::string_view(initialErr->reason));
  }

  donner::editor::ViewportState viewport;
  bool viewportInitialized = false;

  bool panning = false;
  ImVec2 lastPanMouse(0.0f, 0.0f);
  FrameHistory frameHistory;

  // Pending async results from backend.
  std::optional<std::future<donner::editor::FrameResult>> pendingFrame;

  // Text change debounce state.
  bool textChangePending = false;
  bool textDispatchThrottled = false;
  float textChangeIdleTimer = 0.0f;

  // Track current file path for title bar.
  std::optional<std::string> currentFilePath = initialPath;
  bool documentDirty = false;

  std::string lastWindowTitle;
  bool dockspaceInitialized = false;
  bool openFileModalRequested = false;
  std::array<char, 4096> openFilePathBuffer{};
  std::string openFileError;

  std::cerr << "[startup] +" << msSinceStart() << "ms entering main loop\n";

  // ---------------------------------------------------------------------------
  // Main loop
  // ---------------------------------------------------------------------------
  while (!glfwWindowShouldClose(window)) {
    ZoneScopedN("main_loop");
    frameHistory.push(ImGui::GetIO().DeltaTime * 1000.0f);

    glfwPollEvents();

    // Window title.
    {
      std::string title;
      if (documentDirty) {
        title += "● ";
      }
      if (currentFilePath.has_value()) {
        title += std::filesystem::path(*currentFilePath).filename().string();
      } else {
        title += "untitled";
      }
      title += " — Donner SVG Editor";
      if (title != lastWindowTitle) {
        glfwSetWindowTitle(window, title.c_str());
        lastWindowTitle = std::move(title);
      }
    }

    // Poll pending async frame results.
    if (pendingFrame.has_value()) {
      if (pendingFrame->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = pendingFrame->get();
        ProcessFrameResult(result, texture, textureWidth, textureHeight, textEditor,
                           lastShownErrorLine, lastShownErrorReason);
        pendingFrame.reset();
      }
    }

    // Also sync error markers from cached backend state (for latest frame).
    {
      const auto parseError = backend->lastParseError();
      if (parseError.has_value()) {
        const int line = parseError->range.start.lineInfo.has_value()
                             ? static_cast<int>(parseError->range.start.lineInfo->line)
                             : 1;
        const std::string_view reasonSv = parseError->reason;
        if (line != lastShownErrorLine || reasonSv != lastShownErrorReason) {
          textEditor.setErrorMarkers(ParseErrorToMarkers(*parseError));
          lastShownErrorLine = line;
          lastShownErrorReason.assign(reasonSv);
        }
      } else if (lastShownErrorLine != kNoErrorLine) {
        textEditor.setErrorMarkers({});
        lastShownErrorLine = kNoErrorLine;
        lastShownErrorReason.clear();
      }
    }

    {
      double currentDisplayScale = 1.0;
#ifdef __EMSCRIPTEN__
      currentDisplayScale = CurrentDisplayScale();
#else
      float xScale = 1.0f;
      float yScale = 1.0f;
      glfwGetWindowContentScale(window, &xScale, &yScale);
      currentDisplayScale = static_cast<double>(xScale);
#endif
      currentDisplayScale = std::max(1.0, currentDisplayScale);
      io.DisplayFramebufferScale =
          ImVec2(static_cast<float>(currentDisplayScale), static_cast<float>(currentDisplayScale));
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const auto applyZoom = [&](double factor, const donner::Vector2d& focalScreen) {
      viewport.zoomAround(viewport.zoom * factor, focalScreen);
    };

    const auto tryOpenPath = [&](const std::string& rawPath) {
      const std::filesystem::path path(rawPath);
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        openFileError = "Could not open file.";
        return false;
      }

      std::ostringstream out;
      out << file.rdbuf();
      const std::string contents = std::move(out).str();

      auto loadFuture = backend->loadBytes(
          std::span(reinterpret_cast<const uint8_t*>(contents.data()), contents.size()),
          path.string());
      auto loadResult = loadFuture.get();
      if (!loadResult.ok) {
        openFileError = "Failed to parse SVG.";
        return false;
      }

      textEditor.setText(contents);
      textEditor.resetTextChanged();
      currentFilePath = path.string();
      documentDirty = false;
      textChangePending = false;
      textDispatchThrottled = false;
      textChangeIdleTimer = 0.0f;
      pendingFrame.reset();
      openFileError.clear();

      // Upload the new bitmap.
      ProcessFrameResult(loadResult, texture, textureWidth, textureHeight, textEditor,
                         lastShownErrorLine, lastShownErrorReason);
      return true;
    };

#ifdef __EMSCRIPTEN__
    if (gPendingBrowserUploadPath.has_value()) {
      const std::string path = *gPendingBrowserUploadPath;
      gPendingBrowserUploadPath.reset();
      (void)tryOpenPath(path);
    }
#endif

    const auto triggerOpen = [&]() {
#ifdef __EMSCRIPTEN__
      ShowBrowserOpenFileDialog(kMaxWasmUploadBytes);
#else
      std::fill(openFilePathBuffer.begin(), openFilePathBuffer.end(), '\0');
      if (currentFilePath.has_value()) {
        std::strncpy(openFilePathBuffer.data(), currentFilePath->c_str(),
                     openFilePathBuffer.size() - 1);
      }
      openFileError.clear();
      openFileModalRequested = true;
#endif
    };
    const auto triggerUndo = [&]() {
      auto future = backend->undo();
      auto result = future.get();
      ProcessFrameResult(result, texture, textureWidth, textureHeight, textEditor,
                         lastShownErrorLine, lastShownErrorReason);
    };
    const auto triggerRedo = [&]() {
      auto future = backend->redo();
      auto result = future.get();
      ProcessFrameResult(result, texture, textureWidth, textureHeight, textEditor,
                         lastShownErrorLine, lastShownErrorReason);
    };
    const auto triggerQuit = [&]() { glfwSetWindowShouldClose(window, GLFW_TRUE); };
    const auto triggerZoomIn = [&]() { applyZoom(kKeyboardZoomStep, viewport.paneCenter()); };
    const auto triggerZoomOut = [&]() {
      applyZoom(1.0 / kKeyboardZoomStep, viewport.paneCenter());
    };
    const auto triggerActualSize = [&]() { viewport.resetTo100Percent(); };

    const bool sourcePaneFocused = textEditor.isFocused();
    bool openAboutPopup = false;
    bool openLicensesPopup = false;

    // Global keyboard shortcuts.
    {
      const bool anyPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
      const bool cmd = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
      const bool shift = ImGui::GetIO().KeyShift;
      const bool pressedZ = ImGui::IsKeyPressed(ImGuiKey_Z, /*repeat=*/false);

      if (!anyPopupOpen && cmd && !shift && ImGui::IsKeyPressed(ImGuiKey_O, /*repeat=*/false)) {
        triggerOpen();
      }

      if (!anyPopupOpen && cmd && !shift && ImGui::IsKeyPressed(ImGuiKey_Q, /*repeat=*/false)) {
        triggerQuit();
      }

      if (!sourcePaneFocused) {
        if (pressedZ && cmd && !shift) {
          triggerUndo();
        } else if (pressedZ && cmd && shift) {
          triggerRedo();
        }
      }

      if (!anyPopupOpen && cmd &&
          (ImGui::IsKeyPressed(ImGuiKey_Equal, /*repeat=*/false) ||
           ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, /*repeat=*/false))) {
        triggerZoomIn();
      }
      if (!anyPopupOpen && cmd &&
          (ImGui::IsKeyPressed(ImGuiKey_Minus, /*repeat=*/false) ||
           ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, /*repeat=*/false))) {
        triggerZoomOut();
      }
      if (!anyPopupOpen && cmd && ImGui::IsKeyPressed(ImGuiKey_0, /*repeat=*/false)) {
        triggerActualSize();
      }

      // Escape: clear selection via a no-op pointer event at an impossible location.
      if (!anyPopupOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false)) {
        // Deselect by sending a click to void space (-9999, -9999).
        pendingFrame = backend->pointerEvent(donner::editor::PointerEventPayload{
            .phase = donner::editor::sandbox::PointerPhase::kDown,
            .documentPoint = donner::Vector2d(-9999.0, -9999.0),
        });
      }

      // Delete / Backspace: send key event to backend.
      const bool deleteKey = ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false) ||
                             ImGui::IsKeyPressed(ImGuiKey_Backspace, /*repeat=*/false);
      if (deleteKey && !anyPopupOpen && !ImGui::GetIO().WantCaptureKeyboard) {
        pendingFrame = backend->keyEvent(donner::editor::KeyEventPayload{
            .phase = donner::editor::sandbox::KeyPhase::kDown,
            .keyCode = GLFW_KEY_DELETE,
        });
      }
    }

    if (ImGui::BeginMainMenuBar()) {
      ImGui::PushFont(uiFontBold);
      if (ImGui::BeginMenu("Donner SVG Editor")) {
        ImGui::PopFont();
        if (ImGui::MenuItem("About...")) {
          openAboutPopup = true;
        }
#ifndef __EMSCRIPTEN__
        if (ImGui::MenuItem("Quit Donner", "Cmd+Q")) {
          triggerQuit();
        }
#endif
        ImGui::EndMenu();
      } else {
        ImGui::PopFont();
      }

      if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Cmd+O")) {
          triggerOpen();
        }
        ImGui::EndMenu();
      }

      if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Cmd+Z")) {
          triggerUndo();
        }
        if (ImGui::MenuItem("Redo", "Cmd+Shift+Z")) {
          triggerRedo();
        }
        ImGui::Separator();

        if (!sourcePaneFocused) {
          ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Cut", "Cmd+X", false, sourcePaneFocused)) {
          textEditor.cut();
        }
        if (!sourcePaneFocused) {
          ImGui::EndDisabled();
        }

        if (!sourcePaneFocused) {
          ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Copy", "Cmd+C", false, sourcePaneFocused)) {
          textEditor.copy();
        }
        if (!sourcePaneFocused) {
          ImGui::EndDisabled();
        }

        if (!sourcePaneFocused) {
          ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Paste", "Cmd+V", false, sourcePaneFocused)) {
          textEditor.paste();
        }
        if (!sourcePaneFocused) {
          ImGui::EndDisabled();
        }

        if (!sourcePaneFocused) {
          ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Select All", "Cmd+A", false, sourcePaneFocused)) {
          textEditor.selectAll();
        }
        if (!sourcePaneFocused) {
          ImGui::EndDisabled();
        }
        ImGui::EndMenu();
      }

      if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Zoom In", "Cmd+=")) {
          triggerZoomIn();
        }
        if (ImGui::MenuItem("Zoom Out", "Cmd+-")) {
          triggerZoomOut();
        }
        if (ImGui::MenuItem("Actual Size", "Cmd+0")) {
          triggerActualSize();
        }
        ImGui::EndMenu();
      }

      ImGui::EndMainMenuBar();
    }
    if (openAboutPopup) {
      ImGui::OpenPopup("About Donner SVG Editor");
    }
    if (openFileModalRequested) {
      ImGui::OpenPopup("Open SVG");
      openFileModalRequested = false;
    }

    {
      const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(720.0f, 0.0f), ImGuiCond_Appearing);
      ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                              ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      if (ImGui::BeginPopupModal("Open SVG", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Enter the SVG path to open.");
        const bool submitted =
            ImGui::InputText("Path", openFilePathBuffer.data(), openFilePathBuffer.size(),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (!openFileError.empty()) {
          ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", openFileError.c_str());
        }

        if (submitted || ImGui::Button("Open")) {
          if (tryOpenPath(openFilePathBuffer.data())) {
            ImGui::CloseCurrentPopup();
          }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          openFileError.clear();
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    }

    {
      const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
      ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                              ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      if (ImGui::BeginPopupModal("About Donner SVG Editor", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("About Donner SVG Editor");
        ImGui::Separator();
        ImGui::TextUnformatted("(c) 2024-2026 Jeff McGlynn");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "An SVG editor powered by the Donner, a browser-grade SVG2 rendering engine written in "
            "C++20.");
        ImGui::TextUnformatted("https://github.com/jwmcglynn/donner");
        ImGui::Separator();
        if (ImGui::Button("Show Licenses")) {
          openLicensesPopup = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    }
    if (openLicensesPopup) {
      ImGui::OpenPopup("Third-Party Licenses");
    }

    {
      const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(displaySize.x * 0.75f, displaySize.y * 0.8f),
                               ImGuiCond_Appearing);
      ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                              ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      if (ImGui::BeginPopupModal("Third-Party Licenses", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextUnformatted("Third-party license notices");
        ImGui::Separator();
        ImGui::InputTextMultiline(
            "##third_party_licenses", editorNoticeText.data(), editorNoticeText.size() + 1,
            ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing()), ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("Close")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    }

    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mainViewport->WorkPos);
    ImGui::SetNextWindowSize(mainViewport->WorkSize);
    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    const ImGuiWindowFlags dockspaceHostFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##EditorDockspaceHost", nullptr, dockspaceHostFlags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    if (!dockspaceInitialized) {
      dockspaceInitialized = true;

      ImGui::DockBuilderRemoveNode(dockspaceId);
      ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
      ImGui::DockBuilderSetNodeSize(dockspaceId, mainViewport->WorkSize);

      ImGuiID dockIdMainLeft = dockspaceId;
      ImGuiID dockIdRight = 0;
      ImGui::DockBuilderSplitNode(dockIdMainLeft, ImGuiDir_Left, kInitialMainLeftDockFraction,
                                  &dockIdMainLeft, &dockIdRight);

      ImGuiID dockIdSource = dockIdMainLeft;
      ImGuiID dockIdRender = 0;
      ImGui::DockBuilderSplitNode(dockIdSource, ImGuiDir_Left, kInitialSourceDockFraction,
                                  &dockIdSource, &dockIdRender);

      ImGuiID dockIdTree = dockIdRight;
      ImGuiID dockIdInspector = 0;
      ImGui::DockBuilderSplitNode(dockIdTree, ImGuiDir_Up, kInitialTreeDockFraction, &dockIdTree,
                                  &dockIdInspector);

      ImGui::DockBuilderDockWindow("Source", dockIdSource);
      ImGui::DockBuilderDockWindow("Render", dockIdRender);
      ImGui::DockBuilderDockWindow("Tree View", dockIdTree);
      ImGui::DockBuilderDockWindow("Inspector", dockIdInspector);
      ImGui::DockBuilderFinish(dockspaceId);
    }

    ImGui::End();

    ImGuiWindowClass windowClassNoUndocking;
    windowClassNoUndocking.DockNodeFlagsOverrideSet =
        ImGuiDockNodeFlags_NoUndocking |
        static_cast<ImGuiDockNodeFlags_>(ImGuiDockNodeFlags_NoTabBar);

    // --- Source pane ---
    ImGui::SetNextWindowClass(&windowClassNoUndocking);
    ImGui::Begin("Source");
    ImGui::PushFont(codeFont);
    textEditor.render("##source");
    ImGui::PopFont();

    // Text change dispatch (leading-edge + trailing debounce).
    const auto dispatchTextChange = [&]() {
      const std::string newSource = textEditor.getText();
      documentDirty = true;
      // Fire-and-forget: send to backend, next frame picks up result.
      pendingFrame = backend->replaceSource(std::string(newSource), false);
    };

    if (textEditor.isTextChanged()) {
      textEditor.resetTextChanged();
      documentDirty = true;

      if (!textDispatchThrottled) {
        dispatchTextChange();
        textDispatchThrottled = true;
        textChangePending = false;
      } else {
        textChangePending = true;
      }
      textChangeIdleTimer = 0.0f;
    } else if (textDispatchThrottled) {
      textChangeIdleTimer += ImGui::GetIO().DeltaTime;
      if (textChangeIdleTimer >= kTextChangeDebounceSeconds) {
        if (textChangePending) {
          dispatchTextChange();
          textChangePending = false;
        }
        textDispatchThrottled = false;
        textChangeIdleTimer = 0.0f;
      }
    }
    ImGui::End();

    // --- Render pane ---
    ImGui::SetNextWindowClass(&windowClassNoUndocking);
    ImGui::Begin("Render");

    const ImVec2 contentRegion = ImGui::GetContentRegionAvail();
    const ImVec2 paneOriginImGui = ImGui::GetCursorScreenPos();
    viewport.paneOrigin = donner::Vector2d(paneOriginImGui.x, paneOriginImGui.y);
    viewport.paneSize = donner::Vector2d(contentRegion.x, contentRegion.y);
    {
#ifdef __EMSCRIPTEN__
      viewport.devicePixelRatio = CurrentDisplayScale();
#else
      float xScale = 1.0f;
      float yScale = 1.0f;
      glfwGetWindowContentScale(window, &xScale, &yScale);
      viewport.devicePixelRatio = static_cast<double>(xScale);
#endif
    }

    const bool renderPaneUsable = contentRegion.x > 0.0f && contentRegion.y > 0.0f;

    if (!viewportInitialized && renderPaneUsable) {
      viewport.resetTo100Percent();
      viewportInitialized = true;
    }

    // Set viewport size on the backend so it renders at the right resolution.
    if (renderPaneUsable) {
      const donner::Vector2i desiredCanvasSize = viewport.desiredCanvasSize();
      // Fire and forget — don't block on setViewport.
      (void)backend->setViewport(desiredCanvasSize.x, desiredCanvasSize.y);
    }

    bool paneHovered = false;
    donner::Box2d paneRect;
    if (renderPaneUsable) {
      ImGui::InvisibleButton("##render_canvas", contentRegion,
                             ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
      paneHovered = ImGui::IsItemHovered();
      paneRect = donner::Box2d::FromXYWH(viewport.paneOrigin.x, viewport.paneOrigin.y,
                                         viewport.paneSize.x, viewport.paneSize.y);
    } else {
      ImGui::TextDisabled("(render pane is collapsed)");
    }

    // Pan gesture.
    const bool spaceHeld = ImGui::IsKeyDown(ImGuiKey_Space);
    const bool middleDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    if (paneHovered && (spaceHeld || middleDown) && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !panning) {
      panning = true;
      lastPanMouse = ImGui::GetMousePos();
    } else if (middleDown && !panning) {
      panning = true;
      lastPanMouse = ImGui::GetMousePos();
    }
    if (panning) {
      const ImVec2 now = ImGui::GetMousePos();
      viewport.panBy(donner::Vector2d(now.x - lastPanMouse.x, now.y - lastPanMouse.y));
      lastPanMouse = now;
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
          !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        panning = false;
      }
    }

    // Scroll gesture (zoom / pan).
    const donner::editor::RenderPaneGestureContext gestureContext{
        .paneRect = paneRect,
        .mouseDragPanActive = panning,
    };
    const bool modalCapturingInput = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
    if (!modalCapturingInput) {
      for (const auto& event : pendingScrollEvents.events) {
        const auto action = donner::editor::ClassifyRenderPaneScrollGesture(
            event, gestureContext, kWheelZoomStep, kTrackpadPanPixelsPerScrollUnit);
        if (!action.has_value()) {
          continue;
        }
        donner::editor::ApplyRenderPaneGesture(viewport, *action);
      }
    }
    pendingScrollEvents.events.clear();

    // Draw the document image and selection chrome.
    if (textureWidth > 0 && textureHeight > 0) {
      const donner::Box2d screenRect = viewport.imageScreenRect();
      const ImVec2 imageOrigin(static_cast<float>(screenRect.topLeft.x),
                               static_cast<float>(screenRect.topLeft.y));
      const ImVec2 imageBottomRight(static_cast<float>(screenRect.bottomRight.x),
                                    static_cast<float>(screenRect.bottomRight.y));

      ImDrawList* paneDrawList = ImGui::GetWindowDrawList();

      // Checkerboard background.
      const auto DrawCheckerboard = [](ImDrawList* drawList, const ImVec2& topLeft,
                                       const ImVec2& bottomRight) {
        constexpr float kCheckerSize = 16.0f;
        const ImVec2 snappedTopLeft(std::floor(topLeft.x), std::floor(topLeft.y));
        const ImVec2 snappedBottomRight(std::floor(bottomRight.x), std::floor(bottomRight.y));
        if (snappedTopLeft.x >= snappedBottomRight.x || snappedTopLeft.y >= snappedBottomRight.y) {
          return;
        }

        drawList->PushClipRect(snappedTopLeft, snappedBottomRight, true);
        const float startY = std::floor(snappedTopLeft.y / kCheckerSize) * kCheckerSize;
        const float startX = std::floor(snappedTopLeft.x / kCheckerSize) * kCheckerSize;
        for (float y = startY; y < snappedBottomRight.y; y += kCheckerSize) {
          const int row = static_cast<int>(std::floor(y / kCheckerSize));
          for (float x = startX; x < snappedBottomRight.x; x += kCheckerSize) {
            const int column = static_cast<int>(std::floor(x / kCheckerSize));
            const ImU32 color =
                ((row + column) % 2 == 0) ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255);
            drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + kCheckerSize, y + kCheckerSize),
                                    color);
          }
        }
        drawList->PopClipRect();
      };
      DrawCheckerboard(paneDrawList, imageOrigin, imageBottomRight);
      paneDrawList->AddImage(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                             imageOrigin, imageBottomRight);

      const auto screenToDocument = [&](const ImVec2& screenPoint) -> donner::Vector2d {
        return viewport.screenToDocument(donner::Vector2d(screenPoint.x, screenPoint.y));
      };

      // Pointer events → backend.
      const bool toolEligible = paneHovered && !panning && !spaceHeld;
      if (toolEligible && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const donner::Vector2d docPoint = screenToDocument(ImGui::GetMousePos());
        uint32_t mods = 0;
        if (ImGui::GetIO().KeyShift) {
          mods |= 1;  // Shift modifier bit.
        }
        if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) {
          mods |= 2;  // Ctrl/Cmd modifier bit.
        }
        pendingFrame = backend->pointerEvent(donner::editor::PointerEventPayload{
            .phase = donner::editor::sandbox::PointerPhase::kDown,
            .documentPoint = docPoint,
            .buttons = 1,
            .modifiers = mods,
        });
      }
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !panning && !spaceHeld &&
          ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const donner::Vector2d docPoint = screenToDocument(ImGui::GetMousePos());
        pendingFrame = backend->pointerEvent(donner::editor::PointerEventPayload{
            .phase = donner::editor::sandbox::PointerPhase::kMove,
            .documentPoint = docPoint,
            .buttons = 1,
        });
      }
      if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !panning) {
        const donner::Vector2d docPoint = screenToDocument(ImGui::GetMousePos());
        pendingFrame = backend->pointerEvent(donner::editor::PointerEventPayload{
            .phase = donner::editor::sandbox::PointerPhase::kUp,
            .documentPoint = docPoint,
        });
      }

      // Draw selection chrome from backend's SelectionOverlay.
      const auto& sel = backend->selection();
      for (const auto& s : sel.selections) {
        const donner::Box2d docBBox = s.worldBBox;
        const donner::Box2d screenBBox = viewport.documentToScreen(docBBox);
        paneDrawList->AddRect(ImVec2(static_cast<float>(screenBBox.topLeft.x),
                                     static_cast<float>(screenBBox.topLeft.y)),
                              ImVec2(static_cast<float>(screenBBox.bottomRight.x),
                                     static_cast<float>(screenBBox.bottomRight.y)),
                              kSelectionChromeColor, 0.0f, ImDrawFlags_None,
                              kSelectionChromeThickness);
      }

      // Marquee from selection overlay.
      if (sel.marquee.has_value()) {
        const donner::Box2d marqueeScreen = viewport.documentToScreen(*sel.marquee);
        paneDrawList->AddRectFilled(ImVec2(static_cast<float>(marqueeScreen.topLeft.x),
                                           static_cast<float>(marqueeScreen.topLeft.y)),
                                    ImVec2(static_cast<float>(marqueeScreen.bottomRight.x),
                                           static_cast<float>(marqueeScreen.bottomRight.y)),
                                    kMarqueeFillColor);
        paneDrawList->AddRect(ImVec2(static_cast<float>(marqueeScreen.topLeft.x),
                                     static_cast<float>(marqueeScreen.topLeft.y)),
                              ImVec2(static_cast<float>(marqueeScreen.bottomRight.x),
                                     static_cast<float>(marqueeScreen.bottomRight.y)),
                              kMarqueeStrokeColor, 0.0f, ImDrawFlags_None, kMarqueeStrokeThickness);
      }

      // Hover rect.
      if (sel.hoverRect.has_value()) {
        const donner::Box2d hoverScreen = viewport.documentToScreen(*sel.hoverRect);
        paneDrawList->AddRect(
            ImVec2(static_cast<float>(hoverScreen.topLeft.x),
                   static_cast<float>(hoverScreen.topLeft.y)),
            ImVec2(static_cast<float>(hoverScreen.bottomRight.x),
                   static_cast<float>(hoverScreen.bottomRight.y)),
            IM_COL32(0xff, 0xff, 0xff, 0x80), 0.0f, ImDrawFlags_None, 1.0f);
      }
    } else {
      ImGui::TextUnformatted("(no rendered image)");
    }

    // Frame graph.
    {
      constexpr float kFramePadding = 8.0f;
      const float graphHeight = kFrameGraphHeight + ImGui::GetTextLineHeightWithSpacing();
      ImGui::SetCursorPos(ImVec2(kFramePadding, contentRegion.y - graphHeight - kFramePadding));
      RenderFrameGraph(frameHistory);
    }

    ImGui::End();

    // --- Tree View pane ---
    ImGui::Begin("Tree View");
    {
      const auto& treeSummary = backend->tree();
      if (treeSummary.nodes.empty()) {
        ImGui::TextDisabled("No document loaded.");
      } else {
        ImGui::BeginChild("TreeViewScroll", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);

        // Single-pass depth-walk using the flat node array.
        // Track open/close state via a depth stack of "currently open" depths.
        int prevDepth = -1;
        bool scrollToSelected = false;

        for (size_t i = 0; i < treeSummary.nodes.size(); ++i) {
          const auto& node = treeSummary.nodes[i];
          int depth = static_cast<int>(node.depth);

          // Close tree nodes that are deeper than current depth.
          while (prevDepth >= depth) {
            ImGui::TreePop();
            --prevDepth;
          }

          ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                     ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                     ImGuiTreeNodeFlags_SpanAvailWidth |
                                     ImGuiTreeNodeFlags_DefaultOpen;

          if (node.selected) {
            flags |= ImGuiTreeNodeFlags_Selected;
          }

          // Check if this node has children (next node is deeper).
          bool hasChildren = (i + 1 < treeSummary.nodes.size() &&
                              treeSummary.nodes[i + 1].depth > node.depth);
          if (!hasChildren) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
          }

          ImGui::PushID(static_cast<int>(i));
          bool open = ImGui::TreeNodeEx(node.displayName.c_str(), flags);
          ImGui::PopID();

          // Handle click → selection.
          if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            uint8_t mode = 0;  // Replace.
            if (ImGui::GetIO().KeyCtrl) {
              mode = 1;  // Toggle.
            } else if (ImGui::GetIO().KeyShift) {
              mode = 2;  // Add.
            }
            (void)backend->selectElement(node.entityId, node.entityGeneration, mode);
          }

          // Scroll to selected node.
          if (node.selected && !scrollToSelected) {
            ImGui::SetScrollHereY();
            scrollToSelected = true;
          }

          if (hasChildren && open) {
            prevDepth = depth;
          } else if (!hasChildren) {
            // Leaf — don't change prevDepth (no TreePush happened).
          } else {
            // Node closed — don't descend. Skip all children.
            // (ImGui handles the skip via TreeNodeEx returning false.)
          }
        }

        // Close remaining open tree nodes.
        while (prevDepth >= 0) {
          ImGui::TreePop();
          --prevDepth;
        }

        ImGui::EndChild();
      }
    }
    ImGui::End();

    // --- Inspector pane (placeholder) ---
    ImGui::Begin("Inspector");
    ImGui::TextDisabled("Element inspector pending backend element-detail API (S9 follow-up).");
    ImGui::Separator();
    ImGui::Text("Zoom: %.0f%%", viewport.zoom * 100.0);
    ImGui::Text("Pan anchor: doc=(%.1f, %.1f) screen=(%.0f, %.0f)", viewport.panDocPoint.x,
                viewport.panDocPoint.y, viewport.panScreenPoint.x, viewport.panScreenPoint.y);
    ImGui::Text("DPR: %.2fx", viewport.devicePixelRatio);
    ImGui::TextDisabled("scroll = pan");
    ImGui::TextDisabled("Cmd+scroll = zoom");
    ImGui::TextDisabled("space+drag = pan");
    ImGui::TextDisabled("Cmd+0 = 100%%");
    ImGui::End();

    ImGui::Render();
    int displayWidth = 0;
    int displayHeight = 0;
#ifdef __EMSCRIPTEN__
    displayWidth = canvasPixelWidth();
    displayHeight = canvasPixelHeight();
#else
    glfwGetFramebufferSize(window, &displayWidth, &displayHeight);
#endif
    glViewport(0, 0, displayWidth, displayHeight);
    glClearColor(0.10f, 0.10f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
    FrameMark;
#ifdef __EMSCRIPTEN__
    emscripten_sleep(0);
#endif
  }

  // ---------------------------------------------------------------------------
  // Teardown
  // ---------------------------------------------------------------------------
  glfwSetScrollCallback(window, pendingScrollEvents.previousCallback);
  glfwSetWindowUserPointer(window, nullptr);
  glDeleteTextures(1, &texture);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
