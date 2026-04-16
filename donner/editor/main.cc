/// @file

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "donner/editor/EditorShell.h"
#include "donner/editor/gui/EditorWindow.h"

namespace {

constexpr int kInitialWindowWidth = 1600;
constexpr int kInitialWindowHeight = 900;

}  // namespace

int main(int argc, char** argv) {
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  bool experimentalMode = false;
  std::optional<std::string> svgPath;
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

  donner::editor::gui::EditorWindow window({.title = "Donner Editor",
                                            .initialWidth = kInitialWindowWidth,
                                            .initialHeight = kInitialWindowHeight});
  if (!window.valid()) {
    std::cerr << "Failed to initialize editor window\n";
    return 1;
  }

  donner::editor::EditorShell shell(window,
                                    {.svgPath = *svgPath, .experimentalMode = experimentalMode});
  if (!shell.valid()) {
    std::cerr << "Could not open file " << *svgPath << "\n";
    return 1;
  }

  while (!window.shouldClose()) {
    window.pollEvents();
    window.beginFrame();
    shell.runFrame();
    window.endFrame();
  }

  return 0;
}
