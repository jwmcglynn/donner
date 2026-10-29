#include <emscripten.h>

#include "donner/base/PackagedResource.h"
#include "donner/editor/WholeAppWorkerBridge.h"

namespace donner {

extern "C" EMSCRIPTEN_KEEPALIVE void DonnerPackagedResourceWake() {
  editor::whole_app_worker::RequestFrame();
}

void RequestBrowserPackagedResource(const char* sha) {
  // clang-format off
  MAIN_THREAD_ASYNC_EM_ASM({
    const session = window['__donnerPackagedResourceSession'];
    if (!session?.active) return;
    const current = function() {
      return session.active && window['__donnerPackagedResourceSession'] === session;
    };
    const address = $0;
    const digest = UTF8ToString(address);
    const fail = function() {
      if (!current()) return;
      _DonnerFailPackagedResource(address);
      _DonnerPackagedResourceWake();
    };
    const request = window['__donnerRequestPackagedResource'];
    if (!request) {
      fail();
      return;
    }
    Promise.resolve(request(digest)).then(function() {
      if (current()) _DonnerPackagedResourceWake();
    }, fail);
  }, sha);
  // clang-format on
}

}  // namespace donner
