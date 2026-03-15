/// @file FailureSignalHandler.cc
/// Implementation of crash signal handlers with a fixed-buffer demangled stack trace.
// LCOV_EXCL_START — Signal handlers cannot be safely exercised in unit tests.

#include "donner/base/FailureSignalHandler.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace donner {

namespace {

/// Maximum number of stack frames to capture.
constexpr int kMaxStackFrames = 64;
/// Size of the fixed demangling buffer used to avoid per-crash heap allocations.
constexpr size_t kDemangleBufferSize = 64 * 1024;

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

/// Returns storage for the fixed demangling buffer.
char& demangleBufferStorage() {
  static char buffer[kDemangleBufferSize];
  return buffer[0];
}

/// Returns the fixed demangling buffer as a writable char pointer.
char* demangleBuffer() { return &demangleBufferStorage(); }

/// Write a string to stderr (async-signal-safe).
void writeToStderr(std::string_view str) {
  (void)write(STDERR_FILENO, str.data(), str.size());
}

/// Write a single character to stderr (async-signal-safe).
void writeChar(char ch) { (void)write(STDERR_FILENO, &ch, 1); }

/// Write an unsigned integer in decimal to stderr.
void writeUnsigned(uint64_t value) {
  char buffer[32];
  int index = 0;

  do {
    buffer[index++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0 && index < static_cast<int>(sizeof(buffer)));

  while (index > 0) {
    writeChar(buffer[--index]);
  }
}

/// Write an address-sized integer in hexadecimal to stderr.
void writeHex(uintptr_t value) {
  constexpr char kDigits[] = "0123456789abcdef";
  char buffer[2 + sizeof(uintptr_t) * 2];
  int index = 0;

  writeToStderr("0x");
  do {
    buffer[index++] = kDigits[value & 0xfu];
    value >>= 4;
  } while (value != 0 && index < static_cast<int>(sizeof(buffer)));

  while (index > 0) {
    writeChar(buffer[--index]);
  }
}

/// Return the basename component of a path, or the original string if no slash exists.
const char* basenameOrSelf(const char* path) {
  if (path == nullptr) {
    return nullptr;
  }

  const char* slash = std::strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/// Try to demangle a symbol name into the fixed buffer. Returns nullptr on failure or overflow.
const char* tryDemangleIntoFixedBuffer(const char* mangled) {
  if (mangled == nullptr) {
    return nullptr;
  }

  size_t length = kDemangleBufferSize;
  int status = 0;
  char* result = abi::__cxa_demangle(mangled, demangleBuffer(), &length, &status);
  if (status == 0 && result == demangleBuffer()) {
    return result;
  }

  return nullptr;
}

/// Print a single backtrace frame, attempting to demangle the symbol into the fixed buffer.
void printBacktraceFrame(void* frame) {
  Dl_info info {};
  if (!dladdr(frame, &info)) {
    writeToStderr("  ");
    writeHex(reinterpret_cast<uintptr_t>(frame));
    writeToStderr("\n");
    return;
  }

  writeToStderr("  ");
  writeHex(reinterpret_cast<uintptr_t>(frame));

  const char* imageName = basenameOrSelf(info.dli_fname);
  if (imageName != nullptr) {
    writeToStderr("  ");
    writeToStderr(imageName);
  }

  if (info.dli_sname != nullptr) {
    writeToStderr("  ");
    if (const char* demangled = tryDemangleIntoFixedBuffer(info.dli_sname)) {
      writeToStderr(demangled);
    } else {
      writeToStderr(info.dli_sname);
    }

    if (info.dli_saddr != nullptr) {
      writeToStderr(" + ");
      const uintptr_t offset = reinterpret_cast<uintptr_t>(frame) -
                               reinterpret_cast<uintptr_t>(info.dli_saddr);
      writeUnsigned(offset);
    }
  }

  writeToStderr("\n");
}

/// Warm up symbolization and demangling paths so the signal handler can reuse initialized state.
void prewarmSymbolization() {
  void* frames[1] = {};
  (void)backtrace(frames, 1);

  Dl_info info {};
  (void)dladdr(reinterpret_cast<void*>(&InstallFailureSignalHandler), &info);

  const char* kDummyMangled = "_Z1fv";
  size_t length = kDemangleBufferSize;
  int status = 0;
  char* result = abi::__cxa_demangle(kDummyMangled, demangleBuffer(), &length, &status);
  if (result != nullptr && result != demangleBuffer()) {
    free(result);
  }
}

/// Signal handler that prints a stack trace and re-raises the signal.
void failureSignalHandler(int signo) {
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

  std::array<void*, kMaxStackFrames> frames = {};
  const int numFrames = backtrace(frames.data(), kMaxStackFrames);
  if (numFrames > 0) {
    writeToStderr("Stack trace:\n");
    for (int i = 2; i < numFrames; ++i) {
      printBacktraceFrame(frames[static_cast<size_t>(i)]);
    }
  }

  if (signalIndex >= 0) {
    sigaction(signo, &previousActions()[signalIndex], nullptr);
  } else {
    signal(signo, SIG_DFL);
  }
  raise(signo);
}

}  // namespace

void InstallFailureSignalHandler() {
  prewarmSymbolization();

  for (size_t i = 0; i < kNumFailureSignals; ++i) {
    struct sigaction action{};
    action.sa_handler = failureSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;

    sigaction(kFailureSignals[i].signo, &action, &previousActions()[i]);
  }
}

}  // namespace donner
// LCOV_EXCL_STOP
