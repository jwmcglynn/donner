#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/PackagedResourceTestProvider.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::svg {

/**
 * Fuzzer that parses a full SVG document and rasterizes it with the default renderer.
 *
 * \ref SVGDocumentRender_fuzzer.cc stops after preparing the document, so the filter graph
 * executor, mask and clip compositing, pattern tiles, markers, image sampling, and stroking are
 * built but never run. This target runs them on attacker-controlled documents, including the
 * filter-surface limits that bound attacker-sized intermediate allocations.
 *
 * The canvas is small and fixed so each input stays cheap; the document still controls its own
 * viewBox, transforms, filter regions, and pattern and mask units.
 *
 * See https://llvm.org/docs/LibFuzzer.html
 */
extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** argv) {
  donner::tests::InitializeFuzzerPackagedResourceFixtures((*argv)[0]);
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view buffer(reinterpret_cast<const char*>(data), size);

  ParseWarningSink parseSink = ParseWarningSink::Disabled();
  auto maybeResult = parser::SVGParser::ParseSVG(buffer, parseSink);
  if (!maybeResult.hasResult()) {
    return 0;
  }

  SVGDocument document = std::move(maybeResult).result();
  document.setCanvasSize(64, 64);

  Renderer renderer;
  renderer.draw(document);
  (void)renderer.takeSnapshot();

  return 0;
}

}  // namespace donner::svg
