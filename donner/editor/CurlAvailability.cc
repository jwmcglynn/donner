#include <atomic>
#include <cstdlib>
#include <mutex>

#include "donner/editor/ResourcePolicy.h"

#ifdef __linux__
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;  // POSIX: provided by the C library.
#elif defined(__APPLE__)
#include <crt_externs.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#define environ (*_NSGetEnviron())
#endif

namespace donner::editor {

namespace {

/// Backing storage for the test override. When `testOverrideActive_` is true,
/// `check()` returns `testOverrideState_` instead of probing the real system.
std::atomic<bool> testOverrideActive_{false};
std::atomic<CurlAvailability::State> testOverrideState_{CurlAvailability::State::kUnknown};

/// The real, one-shot probe result. Populated exactly once by `std::call_once`.
std::once_flag probeOnce;
CurlAvailability::State probeResult{CurlAvailability::State::kUnknown};

void RunProbe() {
#if defined(__linux__) || defined(__APPLE__)
  // Use posix_spawn to run `curl --version` with all fds pointed at /dev/null.
  pid_t pid = -1;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

  // NOLINTNEXTLINE - argv must be mutable char* per POSIX spec.
  char arg0[] = "curl";
  char arg1[] = "--version";
  char* argv[] = {arg0, arg1, nullptr};

  const int spawnResult = posix_spawnp(&pid, "curl", &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);

  if (spawnResult != 0) {
    probeResult = CurlAvailability::State::kMissing;
    return;
  }

  int status = 0;
  if (waitpid(pid, &status, 0) == -1) {
    probeResult = CurlAvailability::State::kMissing;
    return;
  }

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    probeResult = CurlAvailability::State::kAvailable;
  } else {
    probeResult = CurlAvailability::State::kMissing;
  }
#else
  // Unsupported platform — assume missing.
  probeResult = CurlAvailability::State::kMissing;
#endif
}

}  // namespace

CurlAvailability::State CurlAvailability::check() {
  if (testOverrideActive_.load(std::memory_order_acquire)) {
    return testOverrideState_.load(std::memory_order_relaxed);
  }
  std::call_once(probeOnce, RunProbe);
  return probeResult;
}

std::string CurlAvailability::installHint() {
#if defined(__linux__)
  return "Install curl: sudo apt install curl (Debian/Ubuntu) or sudo dnf install curl (Fedora).";
#elif defined(__APPLE__)
  return "Install curl: brew install curl — the system curl at /usr/bin/curl should work; this "
         "suggests it was removed or PATH is broken.";
#else
  return "Install the curl CLI and ensure it is on your PATH.";
#endif
}

CurlAvailability::TestOverride::TestOverride(State forced) {
  testOverrideState_.store(forced, std::memory_order_relaxed);
  testOverrideActive_.store(true, std::memory_order_release);
}

CurlAvailability::TestOverride::~TestOverride() {
  testOverrideActive_.store(false, std::memory_order_release);
  // Reset probeOnce so the next real check() will re-probe.
  // std::once_flag cannot be reset, but after TestOverride destruction the active flag gates
  // access to testOverrideState_ vs the call_once path. The real probe is still cached.
}

}  // namespace donner::editor
