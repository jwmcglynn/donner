#ifdef __EMSCRIPTEN__

// TODO(Chunk G): Implement WASM SvgFetcher backed by emscripten_fetch.
// This file is compiled only under __EMSCRIPTEN__ and provides
// MakeWasmFetcher(). For now it is a stub that static_asserts.

#include "donner/editor/SvgFetcher.h"

namespace donner::editor {

std::unique_ptr<SvgFetcher> MakeWasmFetcher() {
  static_assert(false, "WASM SvgFetcher is not yet implemented (Chunk G).");
  return nullptr;
}

}  // namespace donner::editor

#endif  // __EMSCRIPTEN__
