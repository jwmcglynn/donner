/// @file

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <csignal>
#include <cstdio>
#include <execinfo.h>
#include <unistd.h>
#endif

#include "donner/editor/EditorIcon.h"
#include "donner/editor/Notice.h"
#include "donner/editor/EditorShell.h"
#include "donner/editor/gui/EditorWindow.h"

namespace {

constexpr int kInitialWindowWidth = 1600;
constexpr int kInitialWindowHeight = 900;

#ifndef __EMSCRIPTEN__

/// Async-signal-safe stack dump — installed for SIGSEGV / SIGBUS / SIGABRT /
/// SIGILL / SIGFPE. Uses `backtrace` + `backtrace_symbols_fd` (documented
/// signal-safe on both glibc and macOS libSystem). After dumping, reraises
/// with the default handler so the OS still produces a core dump / crash
/// report and the shell sees the signal in the exit status.
void EditorCrashHandler(int signo) {
  // Use write() directly — async-signal-safe, unlike fprintf.
  constexpr const char* kBanner = "\n=== Donner editor crash: signal ";
  ::write(STDERR_FILENO, kBanner, std::strlen(kBanner));

  // Write the signal number as a decimal. Cap at 3 digits; signals are small.
  char sigBuf[8];
  int len = 0;
  int n = signo;
  if (n == 0) {
    sigBuf[len++] = '0';
  } else {
    char tmp[8];
    int tlen = 0;
    while (n > 0 && tlen < 7) {
      tmp[tlen++] = '0' + (n % 10);
      n /= 10;
    }
    while (tlen > 0) sigBuf[len++] = tmp[--tlen];
  }
  ::write(STDERR_FILENO, sigBuf, len);
  ::write(STDERR_FILENO, " (", 2);

  const char* name = "?";
  switch (signo) {
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGBUS: name = "SIGBUS"; break;
    case SIGABRT: name = "SIGABRT"; break;
    case SIGILL: name = "SIGILL"; break;
    case SIGFPE: name = "SIGFPE"; break;
    default: break;
  }
  ::write(STDERR_FILENO, name, std::strlen(name));
  ::write(STDERR_FILENO, ") ===\n", 6);

  void* frames[64];
  const int count = ::backtrace(frames, 64);
  ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
  ::write(STDERR_FILENO, "==========================================\n", 44);

  // Re-raise with the default handler so the OS still produces whatever
  // core dump / crash report it would have, and the parent shell sees the
  // signal in the exit status instead of a success code.
  std::signal(signo, SIG_DFL);
  std::raise(signo);
}

void InstallCrashHandler() {
  const int kSignals[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};
  for (const int signo : kSignals) {
    std::signal(signo, &EditorCrashHandler);
  }
}

#endif  // __EMSCRIPTEN__

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
  InstallCrashHandler();
#endif

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
