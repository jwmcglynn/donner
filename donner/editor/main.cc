/// @file

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include "donner/base/FailureSignalHandler.h"
#endif

#include "donner/editor/EditorShell.h"
#include "donner/editor/EditorSplash.h"
#include "donner/editor/Notice.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/editor/gui/EditorWindow.h"

namespace {

constexpr int kInitialWindowWidth = 1600;
constexpr int kInitialWindowHeight = 900;

std::string EmbeddedBytesToString(std::span<const unsigned char> bytes) {
  std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!text.empty() && text.back() == '\0') {
    text.pop_back();
  }
  return text;
}

#ifndef __EMSCRIPTEN__
// Scoped per-user path for the ImGui settings (.ini) file that persists the
// editor's dock layout and window state across sessions. Returns an empty string
// if no writable config directory can be resolved, in which case the editor
// keeps its settings in-memory and always starts from the default layout.
std::string ScopedImguiIniPath() {
  std::error_code ec;
  std::filesystem::path configDir;
#if defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) {
    configDir = std::filesystem::path(home) / "Library" / "Application Support" / "Donner";
  }
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
    configDir = std::filesystem::path(xdg) / "donner";
  } else if (const char* home = std::getenv("HOME")) {
    configDir = std::filesystem::path(home) / ".config" / "donner";
  }
#endif
  if (configDir.empty()) {
    return {};
  }
  std::filesystem::create_directories(configDir, ec);
  if (ec) {
    return {};
  }
  return (configDir / "editor_imgui.ini").string();
}
#endif

void RunEditorFrame(donner::editor::gui::EditorWindow& window, donner::editor::EditorShell& shell) {
  {
    ZoneScopedN("waitEvents");
    if (const std::optional<float> wakeSeconds = shell.nextIdleWakeSeconds()) {
      window.waitEventsTimeout(*wakeSeconds);
    } else {
      window.waitEvents();
    }
  }
  {
    ZoneScopedN("shell.prepareFrame");
    shell.prepareFrame();
  }
  {
    ZoneScopedN("beginFrame");
    window.beginFrame();
  }
  {
    ZoneScopedN("shell.runFrame");
    shell.runFrame();
  }
  {
    ZoneScopedN("endFrame");
    window.endFrame();
  }
  FrameMark;
}

#ifdef __EMSCRIPTEN__
struct WasmEditorLoopState {
  std::unique_ptr<donner::editor::gui::EditorWindow> window;
  std::unique_ptr<donner::editor::EditorShell> shell;
  bool frameActive = false;
};

void RunWasmEditorFrame(void* userdata) {
  auto* state = static_cast<WasmEditorLoopState*>(userdata);
  // Synchronous browser calls proxied from the pthread can let WebKit service
  // another requestAnimationFrame callback before the suspended frame resumes.
  // A nested frame would call ImGui::NewFrame() twice without an intervening
  // Render() and recurse until the JS stack overflows. Drop that callback; the
  // active frame will finish and the browser will schedule the next one.
  if (state->frameActive) {
    return;
  }
  if (state->window->shouldClose()) {
    emscripten_cancel_main_loop();
    delete state;
    return;
  }

  state->frameActive = true;
  RunEditorFrame(*state->window, *state->shell);
  state->frameActive = false;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifndef __EMSCRIPTEN__
  donner::InstallFailureSignalHandler();
#endif

  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  std::optional<std::string> svgPath;
  std::optional<std::string> initialSource;
  std::optional<std::string> initialPath;
  std::optional<std::string> reproOutputPath;
  bool showWelcome = false;
#ifdef __EMSCRIPTEN__
  initialSource = EmbeddedBytesToString(donner::embedded::kEditorSplashSvg);
  showWelcome = true;
#else
  constexpr std::string_view kUsage =
      "Usage: donner-editor [--experimental] [--save-repro <path>] [filename]\n";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--experimental") {
      // Developer CLI contract: keep accepting this flag even when it is a
      // no-op. Old repro scripts and launch aliases pass it, and removing it
      // breaks callers.
      continue;
    }

    if (arg == "--save-repro") {
      if (i + 1 >= argc) {
        std::cerr << "--save-repro requires a filename argument\n" << kUsage;
        return 1;
      }
      reproOutputPath = std::string(argv[++i]);
      continue;
    }

    if (arg.starts_with("--")) {
      std::cerr << "Unknown option " << arg << "\n" << kUsage;
      return 1;
    }

    if (svgPath.has_value()) {
      std::cerr << kUsage;
      return 1;
    }

    svgPath = std::string(arg);
  }

  if (!svgPath.has_value()) {
    initialSource = EmbeddedBytesToString(donner::embedded::kEditorSplashSvg);
    showWelcome = true;
  }
#endif

  std::string imguiIniPath;
#ifndef __EMSCRIPTEN__
  imguiIniPath = ScopedImguiIniPath();
#endif
  auto window = std::make_unique<donner::editor::gui::EditorWindow>(
      donner::editor::gui::EditorWindowOptions{.title = "Donner SVG Editor",
                                               .initialWidth = kInitialWindowWidth,
                                               .initialHeight = kInitialWindowHeight,
                                               .imguiIniPath = imguiIniPath});
  if (!window->valid()) {
    std::cerr << "Failed to initialize editor window\n";
    return 1;
  }

  auto shell = std::make_unique<donner::editor::EditorShell>(
      *window, donner::editor::EditorShellOptions{
                   .svgPath = svgPath.value_or(""),
                   .initialSource = initialSource,
                   .initialPath = initialPath,
                   .showWelcome = showWelcome,
                   .editorNoticeText = EmbeddedBytesToString(donner::embedded::kEditorNoticeText),
                   .reproOutputPath = reproOutputPath});
  if (!shell->valid()) {
    if (svgPath.has_value()) {
      std::cerr << "Could not open file " << *svgPath << "\n";
    } else {
      std::cerr << "Could not initialize editor content\n";
    }
    return 1;
  }

#ifdef __EMSCRIPTEN__
  auto* loopState = new WasmEditorLoopState{std::move(window), std::move(shell)};
  // The browser presents the WebGPU canvas when the requestAnimationFrame callback returns.
  emscripten_set_main_loop_arg(&RunWasmEditorFrame, loopState, /*fps=*/0,
                               /*simulateInfiniteLoop=*/true);
#else
  while (!window->shouldClose()) {
    RunEditorFrame(*window, *shell);
  }
#endif

  return 0;
}
