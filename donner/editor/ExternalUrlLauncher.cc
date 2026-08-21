#include "donner/editor/ExternalUrlLauncher.h"

#include <string>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/threading.h>
#elif defined(__linux__)
#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>

extern char** environ;
#endif

namespace donner::editor {

bool LaunchExternalUrl(ExternalUrlTarget target) {
  const std::string_view url = ExternalUrlValue(target);
  if (url.empty()) {
    return false;
  }

#if defined(__EMSCRIPTEN__)
  // The editor runs on a worker, so browser globals must be touched on the main thread. The URL
  // points into static storage and remains valid until the asynchronous callback consumes it.
  // clang-format off
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        const url = UTF8ToString($0, $1);
        const opened = window.open(url, '_blank', 'noopener');
        if (!opened) {
          window.location.assign(url);
        }
      },
      url.data(), static_cast<int>(url.size()));
  // clang-format on
  return true;
#elif defined(__linux__)
  std::string program = "xdg-open";
  std::string ownedUrl(url);
  char* arguments[] = {program.data(), ownedUrl.data(), nullptr};
  pid_t child = 0;
  if (posix_spawnp(&child, program.c_str(), nullptr, nullptr, arguments, environ) != 0) {
    return false;
  }

  int status = 0;
  pid_t waitResult = 0;
  do {
    waitResult = waitpid(child, &status, 0);
  } while (waitResult == -1 && errno == EINTR);
  return waitResult == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
  return false;
#endif
}

}  // namespace donner::editor
