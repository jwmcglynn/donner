#include "donner/editor/BrowserImageTransport.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <pthread.h>

#include <bit>
#include <cstdint>
#endif

namespace donner::editor {

#ifdef __EMSCRIPTEN__
// clang-format off: the EM_JS body is JavaScript.
EM_JS(void, InstallBrowserImageTransportWorker, (int role, unsigned int pthreadKey), {
  const existing = globalThis['__donnerGpuImageTransport'];
  if (existing && existing.role === role) {
    return;
  }
  const state = { role: role, port: null, generation: 0, images: new Map(), pending: new Map() };
  globalThis['__donnerGpuImageTransport'] = state;
  globalThis.addEventListener('message', function(event) {
    const message = event.data;
    if (!message || message.cmd !== 'donnerGpuImagePort') {
      return;
    }
    event.stopImmediatePropagation();
    if (state.port) {
      state.port.close();
    }
    for (const image of state.images.values()) {
      image.close();
    }
    state.images.clear();
    state.port = message.port;
    state.generation = message.generation;
    state.port.onmessage = function(received) {
      const payload = received.data;
      if (!payload || payload.generation !== state.generation) {
        payload?.bitmap?.close();
        return;
      }
      if (state.role === 1 && payload.kind === 'image') {
        if (state.images.has(payload.token)) {
          state.images.get(payload.token).close();
        }
        state.images.set(payload.token, payload.bitmap);
        state.port.postMessage({ kind: 'received', generation: state.generation,
                                 token: payload.token });
      } else if (state.role === 2 && payload.kind === 'received') {
        const settle = state.pending.get(payload.token);
        if (settle) {
          state.pending.delete(payload.token);
          settle();
        }
      } else if (state.role === 1 && payload.kind === 'release') {
        const image = state.images.get(payload.token);
        if (image) {
          image.close();
          state.images.delete(payload.token);
        }
      }
    };
    state.port.start();
    postMessage({ cmd: 'callHandler', handler: 'donnerGpuImageTransportReady',
                  args: [role, state.generation] });
  }, true);
  postMessage({ cmd: 'callHandler', handler: 'registerDonnerGpuImageWorker',
                args: [role, pthreadKey] });
});
// clang-format on
#endif

void RegisterBrowserImageTransportWorker(bool rasterWorker) {
#ifdef __EMSCRIPTEN__
  static_assert(sizeof(pthread_t) == sizeof(std::uint32_t));
  const std::uint32_t pthreadKey = std::bit_cast<std::uint32_t>(pthread_self());
  InstallBrowserImageTransportWorker(rasterWorker ? 2 : 1, pthreadKey);
#else
  (void)rasterWorker;
#endif
}

}  // namespace donner::editor
