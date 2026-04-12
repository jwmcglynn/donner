#include "donner/editor/app/EditorRepl.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef __linux__
#include <poll.h>
#include <unistd.h>
#endif

#include "donner/editor/app/EditorApp.h"
#include "donner/editor/sandbox/FrameInspector.h"
#include "donner/editor/sandbox/RnrFile.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/TerminalImageViewer.h"

namespace donner::editor::app {

namespace {

std::vector<std::string> Tokenize(const std::string& line) {
  std::vector<std::string> tokens;
  std::istringstream iss(line);
  std::string tok;
  while (iss >> tok) {
    tokens.push_back(std::move(tok));
  }
  return tokens;
}

}  // namespace

EditorRepl::EditorRepl(EditorApp& app, std::istream& in, std::ostream& out,
                       EditorReplOptions options)
    : app_(app), in_(in), out_(out), options_(std::move(options)) {}

int EditorRepl::run() {
  if (options_.printBanner) {
    out_ << "Donner Editor MVP — type 'help' for commands, 'quit' to exit.\n";
  }

  int count = 0;
  std::string line;
  while (!quit_) {
    if (!options_.prompt.empty()) {
      out_ << options_.prompt;
      out_.flush();
    }

#ifdef __linux__
    // When watch mode is enabled and we're reading from a real fd (stdin),
    // use poll() with a 500ms timeout so we can periodically check for
    // file changes without blocking indefinitely on user input.
    if (app_.watchEnabled() && &in_ == &std::cin) {
      bool gotLine = false;
      while (!quit_ && !gotLine) {
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        int ready = ::poll(&pfd, 1, 500);
        if (ready > 0) {
          if (!std::getline(in_, line)) {
            return count;  // EOF
          }
          gotLine = true;
        } else {
          if (app_.pollForChanges()) {
            out_ << "[auto-reloaded] " << app_.current().message << "\n";
            out_.flush();
          }
        }
      }
    } else
#endif
    {
      // Standard blocking path: stringstream input (tests) or watch disabled.
      if (!std::getline(in_, line)) break;
    }

    // Poll once even on the non-poll path (for stringstream-based tests).
    if (app_.watchEnabled()) {
      if (app_.pollForChanges()) {
        out_ << "[auto-reloaded] " << app_.current().message << "\n";
      }
    }

    if (dispatch(line)) ++count;
  }
  return count;
}

bool EditorRepl::dispatch(const std::string& line) {
  const auto tokens = Tokenize(line);
  if (tokens.empty()) return false;
  const auto& cmd = tokens.front();

  if (cmd == "help" || cmd == "?") {
    printHelp();
    return true;
  }
  if (cmd == "quit" || cmd == "exit") {
    quit_ = true;
    return true;
  }
  if (cmd == "status") {
    printStatus();
    return true;
  }
  if (cmd == "load") {
    if (tokens.size() != 2) {
      out_ << "usage: load <uri>\n";
      return false;
    }
    cmdLoad(tokens[1]);
    return true;
  }
  if (cmd == "reload") {
    cmdReload();
    return true;
  }
  if (cmd == "resize") {
    if (tokens.size() != 3) {
      out_ << "usage: resize <width> <height>\n";
      return false;
    }
    auto parseDim = [&](const std::string& s, int& out) -> bool {
      int v = 0;
      const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
      if (ec != std::errc() || ptr != s.data() + s.size() || v <= 0) {
        out_ << "resize: invalid dimension '" << s << "'\n";
        return false;
      }
      out = v;
      return true;
    };
    int w = 0, h = 0;
    if (!parseDim(tokens[1], w) || !parseDim(tokens[2], h)) return false;
    cmdResize(w, h);
    return true;
  }
  if (cmd == "show") {
    cmdShow();
    return true;
  }
  if (cmd == "save") {
    if (tokens.size() != 2) {
      out_ << "usage: save <out.png>\n";
      return false;
    }
    cmdSave(tokens[1]);
    return true;
  }
  if (cmd == "inspect") {
    cmdInspect();
    return true;
  }
  if (cmd == "record") {
    if (tokens.size() != 2) {
      out_ << "usage: record <out.rnr>\n";
      return false;
    }
    cmdRecord(tokens[1]);
    return true;
  }
  if (cmd == "watch") {
    if (tokens.size() != 2) {
      out_ << "usage: watch on|off\n";
      return false;
    }
    cmdWatch(tokens[1]);
    return true;
  }

  out_ << "unknown command '" << cmd << "' — type 'help' for the list\n";
  return false;
}

void EditorRepl::printHelp() {
  out_ << "Commands:\n"
       << "  help                    show this help\n"
       << "  load <uri>              fetch + render a new SVG (file:// or path)\n"
       << "  reload                  re-fetch the current URI\n"
       << "  resize <w> <h>          re-render at a new viewport\n"
       << "  status                  print the latest status line\n"
       << "  show                    render current frame to the terminal\n"
       << "  save <out.png>          write current bitmap as PNG\n"
       << "  inspect                 dump decoded wire stream\n"
       << "  record <out.rnr>        save current frame as a .rnr recording\n"
       << "  watch on|off            toggle filesystem watch for auto-reload\n"
       << "  quit | exit             leave the editor\n";
}

void EditorRepl::printStatus() {
  const auto& snap = app_.current();
  out_ << "[" << snap.message << "]";
  if (!snap.uri.empty()) {
    out_ << " uri=" << snap.uri;
  }
  out_ << "\n";
}

void EditorRepl::cmdLoad(const std::string& uri) {
  app_.navigate(uri);
  printStatus();
}

void EditorRepl::cmdReload() {
  app_.reload();
  printStatus();
}

void EditorRepl::cmdResize(int width, int height) {
  app_.resize(width, height);
  printStatus();
}

void EditorRepl::cmdShow() {
  const auto& bitmap = app_.lastGoodBitmap();
  if (bitmap.pixels.empty()) {
    out_ << "show: no frame available (try 'load <uri>' first)\n";
    return;
  }
  if (!options_.showEnabled) {
    out_ << "show: disabled in this repl session\n";
    return;
  }

  svg::TerminalImageView view{
      .data = std::span<const uint8_t>(bitmap.pixels),
      .width = bitmap.dimensions.x,
      .height = bitmap.dimensions.y,
      .strideInPixels = bitmap.rowBytes / 4,
  };
  auto config = svg::TerminalImageViewer::DetectConfigFromEnvironment();
  config.autoScale = true;
  svg::TerminalImageViewer{}.render(view, out_, config);
}

void EditorRepl::cmdSave(const std::string& path) {
  const auto& bitmap = app_.lastGoodBitmap();
  if (bitmap.pixels.empty() || bitmap.dimensions.x <= 0) {
    out_ << "save: no frame available\n";
    return;
  }
  const auto png = svg::RendererImageIO::writeRgbaPixelsToPngMemory(
      bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
      bitmap.rowBytes / 4);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    out_ << "save: cannot open " << path << "\n";
    return;
  }
  out.write(reinterpret_cast<const char*>(png.data()),
            static_cast<std::streamsize>(png.size()));
  out_ << "save: wrote " << path << " (" << png.size() << " bytes)\n";
}

void EditorRepl::cmdInspect() {
  const auto& wire = app_.lastGoodWire();
  if (wire.empty()) {
    out_ << "inspect: no frame available\n";
    return;
  }
  out_ << sandbox::FrameInspector::Dump(wire);
}

void EditorRepl::cmdRecord(const std::string& path) {
  const auto& wire = app_.lastGoodWire();
  if (wire.empty()) {
    out_ << "record: no frame available\n";
    return;
  }
  sandbox::RnrHeader header;
  header.width = static_cast<uint32_t>(app_.width());
  header.height = static_cast<uint32_t>(app_.height());
  header.backend = sandbox::BackendHint::kTinySkia;
  header.uri = app_.current().uri;
  const auto status = sandbox::SaveRnrFile(path, header, wire);
  if (status != sandbox::RnrIoStatus::kOk) {
    out_ << "record: save failed\n";
    return;
  }
  out_ << "record: wrote " << path << " (" << wire.size() << " wire bytes)\n";
}

void EditorRepl::cmdWatch(const std::string& arg) {
  if (arg == "on") {
    app_.setWatchEnabled(true);
    out_ << "watch: enabled\n";
  } else if (arg == "off") {
    app_.setWatchEnabled(false);
    out_ << "watch: disabled\n";
  } else {
    out_ << "usage: watch on|off\n";
  }
}

}  // namespace donner::editor::app
