/// @file
/// Reaches browser GPU owner abort sites on the application pthread of a threaded module, the way
/// the editor's module runs, and reports a thread role seen by a reused worker.

#include <emscripten/em_asm.h>

#include <cstring>
#include <thread>

#include "donner/base/FatalAbort.h"
#include "donner/gpu/browser/BrowserGpuOwner.h"

int main(int argc, char** argv) {
  // The mode is the last argument: under Node the runtime passes the script arguments through.
  const char* mode = argc > 1 ? argv[argc - 1] : "";
  if (std::strcmp(mode, "register-twice") == 0) {
    donner::gpu::browser::RegisterBrowserGpuOwner();
    donner::gpu::browser::RegisterBrowserGpuOwner();
  } else if (std::strcmp(mode, "dispatch-after-stop") == 0) {
    donner::gpu::browser::RegisterBrowserGpuOwner();
    donner::gpu::browser::StopBrowserGpuOwner();
    donner::gpu::browser::RunOnBrowserGpuOwner([] {});
  } else if (std::strcmp(mode, "role-after-thread-reuse") == 0) {
    // The second thread runs on the worker the first one returned to the runtime's pool.
    std::thread([] { donner::SetFatalAbortThreadRole("render-worker"); }).join();
    std::thread([] {
      // clang-format off
      EM_ASM({ err('role-after-thread-reuse=' + (globalThis['__donnerThreadRole'] || 'none')); });
      // clang-format on
    }).join();
    return 0;
  }
  return 2;
}
