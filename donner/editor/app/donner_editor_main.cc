/// @file
///
/// `render_repl` — a non-GUI render/debug REPL. Spins up a `RenderSession` and a
/// `RenderSessionRepl` driven by stdin/stdout.
///
///     $ bazel run //donner/editor/app:render_repl
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

  donner::editor::app::RenderSessionOptions options;
  // Fetch paths relative to the invoking user's cwd.
  options.sourceOptions.baseDirectory = std::filesystem::current_path();

  // Accept a one-shot initial URL as argv[1] so the binary is scriptable:
  //   render_repl mini.svg
  donner::editor::app::RenderSession app(std::move(options));
  if (argc > 1) {
    app.navigate(argv[1]);
  }

  donner::editor::app::RenderSessionRepl repl(app, std::cin, std::cout);
  repl.run();
  return 0;
}
