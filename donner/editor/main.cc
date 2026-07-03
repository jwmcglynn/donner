/// @file

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include "donner/editor/EditorSplash.h"
#else
#include "donner/base/FailureSignalHandler.h"
#endif

#include "donner/editor/EditorShell.h"
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
#ifdef __EMSCRIPTEN__
  initialSource = EmbeddedBytesToString(donner::embedded::kEditorSplashSvg);
  initialPath = std::string("donner_splash.svg");
#else
  constexpr std::string_view kUsage =
      "Usage: donner-editor [--experimental] [--save-repro <path>] <filename>\n";
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
    std::cerr << kUsage;
    return 1;
  }
#endif

  donner::editor::gui::EditorWindow window({.title = "Donner SVG Editor",
                                            .initialWidth = kInitialWindowWidth,
                                            .initialHeight = kInitialWindowHeight});
  if (!window.valid()) {
    std::cerr << "Failed to initialize editor window\n";
    return 1;
  }

  donner::editor::EditorShell shell(
      window, {.svgPath = svgPath.value_or(""),
               .initialSource = initialSource,
               .initialPath = initialPath,
               .editorNoticeText = EmbeddedBytesToString(donner::embedded::kEditorNoticeText),
               .reproOutputPath = reproOutputPath});
  if (!shell.valid()) {
    if (svgPath.has_value()) {
      std::cerr << "Could not open file " << *svgPath << "\n";
    } else {
      std::cerr << "Could not initialize editor content\n";
    }
    return 1;
  }

  while (!window.shouldClose()) {
    {
      ZoneScopedN("waitEvents");
      // On native this blocks until a user input, window event, timed
      // UI wake, or a `wakeEventLoop()` post from the render worker.
      // The editor is event-driven: no frames are produced between user
      // inputs, worker results, and active timed UI work.
      //
      // On Emscripten `waitEvents` falls through to `glfwPollEvents`
      // since the browser drives the loop via `requestAnimationFrame`.
      if (const std::optional<float> wakeSeconds = shell.nextIdleWakeSeconds()) {
        window.waitEventsTimeout(*wakeSeconds);
      } else {
        window.waitEvents();
      }
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
#ifdef __EMSCRIPTEN__
    // Emscripten-glfw's `glfwSwapBuffers` is a no-op, so we need an
    // explicit asyncify yield every frame; otherwise the main loop
    // would block the browser forever (preventing `Loading…` from
    // clearing and freezing the canvas).
    emscripten_sleep(0);
#endif
    FrameMark;
  }

  return 0;
}
