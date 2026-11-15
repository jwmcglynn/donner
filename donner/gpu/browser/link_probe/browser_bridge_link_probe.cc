/// @file
/// Links the browser bridge into a WebAssembly module.
///
/// Constructing the bridge emits its vtable, which references every entry point it declares, so
/// the link fails unless the JavaScript library beside it implements all of them. That is what
/// makes this a gate rather than a compile: the two halves of the bridge are written in different
/// languages and nothing else checks that they still name the same set of functions.

#include <memory>
#include <utility>

#include "donner/gpu/browser/BrowserDevice.h"
#include "donner/gpu/browser/EmscriptenBrowserBridge.h"

int main() {
  using donner::gpu::browser::BrowserDeviceRequest;
  using donner::gpu::browser::EmscriptenBrowserBridge;

  BrowserDeviceRequest request =
      BrowserDeviceRequest::Begin(std::make_unique<EmscriptenBrowserBridge>());
  return static_cast<int>(request.state());
}
