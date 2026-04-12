/// @file
///
/// `donner_editor` — the MVP editor binary. Spins up an `EditorApp` and an
/// `EditorRepl` driven by stdin/stdout, so you can:
///
///     $ bazel run //donner/editor/app:donner_editor
///     donner> load donner_splash.svg
///     [rendered 512x384] uri=donner_splash.svg
///     donner> show       # prints an ANSI approximation to the terminal
///     donner> save out.png
///     donner> inspect    # dumps the decoded wire stream
///     donner> quit
///
/// This is deliberately the smallest possible "editor" binary: no window,
/// no GL, no ImGui. Every piece of the sandbox infrastructure that landed
/// in S1-S6.1 is exercised end-to-end via the REPL commands.

#include <cstdlib>
#include <iostream>

#include "donner/editor/app/EditorApp.h"
#include "donner/editor/app/EditorRepl.h"

int main(int argc, char* argv[]) {
  // Respect bazel's BUILD_WORKING_DIRECTORY so `bazel run` resolves
  // user-supplied paths against their original cwd, matching the pattern
  // used by //examples:svg_to_png.
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::error_code ec;
    std::filesystem::current_path(bwd, ec);
  }

  donner::editor::app::EditorAppOptions options;
  // Fetch paths relative to the invoking user's cwd.
  options.sourceOptions.baseDirectory = std::filesystem::current_path();

  // Accept a one-shot initial URL as argv[1] so the binary is scriptable:
  //   donner_editor mini.svg
  donner::editor::app::EditorApp app(std::move(options));
  if (argc > 1) {
    app.navigate(argv[1]);
  }

  donner::editor::app::EditorRepl repl(app, std::cin, std::cout);
  repl.run();
  return 0;
}
