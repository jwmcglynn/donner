/// @file FailureSignalHandler.cc
/// Implementation of crash signal handlers with stack trace printing for macOS and Linux.

#include "donner/base/FailureSignalHandler.h"

#include <cxxabi.h>
#include <execinfo.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace donner {

namespace {

/// Maximum number of stack frames to capture.
constexpr int kMaxStackFrames = 64;

/// Signals to handle and their names.
struct SignalEntry {
  int signo;
  const char* name;
};

constexpr SignalEntry kFailureSignals[] = {
    {SIGSEGV, "SIGSEGV"}, {SIGABRT, "SIGABRT"}, {SIGFPE, "SIGFPE"},
    {SIGILL, "SIGILL"},   {SIGBUS, "SIGBUS"},   {SIGTRAP, "SIGTRAP"},
};

constexpr size_t kNumFailureSignals = sizeof(kFailureSignals) / sizeof(kFailureSignals[0]);

/// Returns the array of previous signal actions, so we can restore them after printing.
std::array<struct sigaction, kNumFailureSignals>& previousActions() {
  static std::array<struct sigaction, kNumFailureSignals> actions = {};
  return actions;
}

/// Write a string to stderr (async-signal-safe).
void writeToStderr(std::string_view str) {
  // write() is async-signal-safe, unlike fprintf.
  // Ignore return value in signal handler - nothing we can do if write fails.
  std::ignore = write(STDERR_FILENO, str.data(), str.size());
}

/// Try to demangle a symbol name. Returns the demangled name in a unique_ptr, or nullptr.
std::unique_ptr<char, decltype(&free)> tryDemangle(const char* mangled) {
  int status = 0;
  std::unique_ptr<char, decltype(&free)> result(
      abi::__cxa_demangle(mangled, nullptr, nullptr, &status), &free);
  if (status != 0) {
    result.reset();
  }
  return result;
}

/// Print a single backtrace symbol line, attempting to demangle.
void printBacktraceSymbol(const char* symbol) {
  // backtrace_symbols() output format:
  //   macOS:  "index  module  address  mangled_name + offset"
  //   Linux:  "module(mangled_name+0xoffset) [address]"

#ifdef __APPLE__
  // Parse macOS format: "index  module  address  mangled_name + offset"
  // Find the mangled name by looking for the 4th whitespace-separated token.
  const char* p = symbol;
  int spaces = 0;
  while (*p && spaces < 3) {
    if (*p == ' ') {
      ++spaces;
      while (*(p + 1) == ' ') {
        ++p;
      }
    }
    ++p;
  }

  if (*p) {
    // p now points to the mangled name
    const char* nameStart = p;
    const char* nameEnd = nameStart;
    while (*nameEnd && *nameEnd != ' ') {
      ++nameEnd;
    }

    std::string mangledName(nameStart, nameEnd);
    if (auto demangled = tryDemangle(mangledName.c_str())) {
      // Print everything before the mangled name, then the demangled name, then the rest.
      writeToStderr(std::string_view(symbol, static_cast<size_t>(nameStart - symbol)));
      writeToStderr(demangled.get());
      writeToStderr(nameEnd);
      writeToStderr("\n");
      return;
    }
  }
#elif defined(__linux__)
  // Parse Linux format: "module(mangled_name+0xoffset) [address]"
  const char* parenOpen = strchr(symbol, '(');
  if (parenOpen) {
    const char* plus = strchr(parenOpen, '+');
    const char* parenClose = strchr(parenOpen, ')');
    if (plus && parenClose && plus > parenOpen + 1) {
      std::string mangledName(parenOpen + 1, plus);
      if (auto demangled = tryDemangle(mangledName.c_str())) {
        writeToStderr(std::string_view(symbol, static_cast<size_t>(parenOpen - symbol + 1)));
        writeToStderr(demangled.get());
        writeToStderr(std::string_view(plus, strlen(symbol) - static_cast<size_t>(plus - symbol)));
        writeToStderr("\n");
        return;
      }
    }
  }
#endif

  // Fallback: print the raw symbol.
  writeToStderr(symbol);
  writeToStderr("\n");
}

/// Signal handler that prints a stack trace and re-raises the signal.
void failureSignalHandler(int signo) {
  // Find the signal name.
  const char* signalName = "UNKNOWN";
  int signalIndex = -1;
  for (size_t i = 0; i < kNumFailureSignals; ++i) {
    if (kFailureSignals[i].signo == signo) {
      signalName = kFailureSignals[i].name;
      signalIndex = static_cast<int>(i);
      break;
    }
  }

  writeToStderr("\n*** ");
  writeToStderr(signalName);
  writeToStderr(" received ***\n");

  // Capture backtrace.
  std::array<void*, kMaxStackFrames> frames = {};
  const int numFrames = backtrace(frames.data(), kMaxStackFrames);

  if (numFrames > 0) {
    std::unique_ptr<char*, decltype(&free)> symbols(
        backtrace_symbols(frames.data(), numFrames), &free);
    if (symbols) {
      writeToStderr("Stack trace:\n");
      // Skip the first few frames (signal handler internals).
      for (int i = 2; i < numFrames; ++i) {
        writeToStderr("  ");
        printBacktraceSymbol(symbols.get()[i]);
      }
    } else {
      // Fallback: use backtrace_symbols_fd which writes directly.
      writeToStderr("Stack trace (raw):\n");
      backtrace_symbols_fd(frames.data(), numFrames, STDERR_FILENO);
    }
  }

  // Restore the previous signal action and re-raise to get default behavior (e.g. core dump).
  if (signalIndex >= 0) {
    sigaction(signo, &previousActions()[signalIndex], nullptr);
  } else {
    signal(signo, SIG_DFL);
  }
  raise(signo);
}

}  // namespace

void InstallFailureSignalHandler() {
  for (size_t i = 0; i < kNumFailureSignals; ++i) {
    struct sigaction action{};
    action.sa_handler = failureSignalHandler;
    sigemptyset(&action.sa_mask);
    // SA_RESETHAND: restore the default handler after the first signal.
    action.sa_flags = SA_RESETHAND;

    sigaction(kFailureSignals[i].signo, &action, &previousActions()[i]);
  }
}

}  // namespace donner
