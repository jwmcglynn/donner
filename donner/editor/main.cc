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
#endif

#include "donner/base/FailureSignalHandler.h"
#include "donner/editor/EditorIcon.h"
#include "donner/editor/Notice.h"
#include "donner/editor/EditorShell.h"
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
  donner::InstallFailureSignalHandler();

  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  bool experimentalMode = false;
  std::optional<std::string> svgPath;
  std::optional<std::string> initialSource;
  std::optional<std::string> initialPath;
#ifdef __EMSCRIPTEN__
  initialSource = EmbeddedBytesToString(donner::embedded::kEditorIconSvg);
  initialPath = std::string("donner_icon.svg");
#else
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--experimental") {
      experimentalMode = true;
      continue;
    }

    if (svgPath.has_value()) {
      std::cerr << "Usage: donner-editor [--experimental] <filename>\n";
      return 1;
    }

    svgPath = std::string(arg);
  }

  if (!svgPath.has_value()) {
    std::cerr << "Usage: donner-editor [--experimental] <filename>\n";
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

  donner::editor::EditorShell shell(window, {.svgPath = svgPath.value_or(""),
                                             .initialSource = initialSource,
                                             .initialPath = initialPath,
                                             .editorNoticeText =
                                                 EmbeddedBytesToString(donner::embedded::kEditorNoticeText),
                                             .experimentalMode = experimentalMode});
  if (!shell.valid()) {
    if (svgPath.has_value()) {
      std::cerr << "Could not open file " << *svgPath << "\n";
    } else {
      std::cerr << "Could not initialize editor content\n";
    }
    return 1;
  }

  while (!window.shouldClose()) {
    window.pollEvents();
    window.beginFrame();
    shell.runFrame();
    window.endFrame();
#ifdef __EMSCRIPTEN__
    // Emscripten-glfw's `glfwSwapBuffers` is a no-op, so we need an
    // explicit asyncify yield every frame; otherwise the main loop
    // would block the browser forever (preventing `Loading…` from
    // clearing and freezing the canvas).
    emscripten_sleep(0);
#endif
  }

  return 0;
}
