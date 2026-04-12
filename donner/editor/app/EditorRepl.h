#pragma once
/// @file
///
/// `EditorRepl` is the stdin/stdout-driven command loop that sits on top
/// of `EditorApp`. It exists so that the MVP can be driven (and tested)
/// without an ImGui window — the shell layer is a thin parser that maps
/// line-based text commands to `EditorApp` methods and writes
/// human-readable output.
///
/// The REPL is parameterized by `std::istream&` + `std::ostream&` instead
/// of hard-coding `std::cin`/`std::cout`, so headless tests can feed a
/// scripted input stream and assert against the captured output without
/// touching the real terminal.
///
/// Supported commands (one per line, whitespace-separated):
///
///   `help`                 — list commands
///   `load <uri>`           — fetch + render
///   `reload`               — re-fetch the current URI
///   `resize <w> <h>`       — re-render at a new viewport
///   `status`               — print the latest status line
///   `show`                 — render the current frame to the terminal (ANSI)
///   `save <out.png>`       — write the current bitmap as a PNG
///   `inspect`              — dump the frame's decoded command stream
///   `record <out.rnr>`     — save the current frame as a `.rnr` recording
///   `watch on|off`          — toggle filesystem mtime polling for auto-reload
///   `quit` / `exit`        — leave the loop

#include <iosfwd>
#include <string>

namespace donner::editor::app {

class EditorApp;

/// Runtime options for the REPL. Defaults match the interactive binary.
struct EditorReplOptions {
  /// Prompt string printed before each command line. Tests pass an empty
  /// string so captured output doesn't include decoration.
  std::string prompt = "donner> ";
  /// If true, `show` attempts to render the frame to the output stream
  /// using `TerminalImageViewer`. Tests disable this because the sampled
  /// ANSI output is noisy to match against.
  bool showEnabled = true;
  /// If true, the REPL prints a one-line welcome banner before accepting
  /// commands. Tests disable this.
  bool printBanner = true;
};

class EditorRepl {
public:
  EditorRepl(EditorApp& app, std::istream& in, std::ostream& out,
             EditorReplOptions options = {});

  /// Runs the command loop until EOF or a `quit`/`exit` command. Returns
  /// the number of commands successfully dispatched.
  int run();

  /// Dispatches a single pre-tokenized command line. Exposed for tests
  /// that want to skip the input stream entirely.
  bool dispatch(const std::string& line);

private:
  void printHelp();
  void printStatus();
  void cmdLoad(const std::string& uri);
  void cmdReload();
  void cmdResize(int width, int height);
  void cmdShow();
  void cmdSave(const std::string& path);
  void cmdInspect();
  void cmdRecord(const std::string& path);
  void cmdWatch(const std::string& arg);

  EditorApp& app_;
  std::istream& in_;
  std::ostream& out_;
  EditorReplOptions options_;
  bool quit_ = false;
};

}  // namespace donner::editor::app
