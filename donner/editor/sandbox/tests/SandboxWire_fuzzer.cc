/// @file SandboxWire_fuzzer.cc
///
/// LibFuzzer target that validates the sandbox wire deserializer never crashes
/// on adversarial input.  Feeds arbitrary bytes into
/// `ReplayingRenderer::pumpFrame()` wrapping a `RendererTinySkia` sink.
/// Decode errors surfacing as non-`kOk` `ReplayStatus` returns are expected
/// and harmless; only crashes are bugs.

#include <cstddef>
#include <cstdint>
#include <span>

#include "donner/editor/sandbox/ReplayingRenderer.h"
#include "donner/svg/renderer/RendererTinySkia.h"

namespace donner::editor::sandbox {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  svg::RendererTinySkia sink;
  ReplayingRenderer replayer(sink);
  ReplayReport report;
  (void)replayer.pumpFrame({data, size}, report);
  return 0;
}

}  // namespace donner::editor::sandbox
