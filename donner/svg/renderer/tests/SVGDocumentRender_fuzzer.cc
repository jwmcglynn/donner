#include <cstdlib>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/PackagedResourceTestProvider.h"
#include "donner/svg/DonnerController.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererUtils.h"

namespace donner::svg {

/**
 * Fuzzer that parses a full SVG document and then prepares it for rendering, exercising
 * reference/href resolution (<use>, <pattern>, paint servers), filter primitive parameter
 * handling, gradient stop handling, and layout across the whole document graph.
 *
 * This targets pathological reference graphs (cycles, deep chains, self-references) that can
 * cause stack overflows or excessive memory/CPU use during shadow tree instantiation, which is
 * not exercised by the per-element parser fuzzers. It then hit-tests a grid of points across the
 * canvas: every hit, including `<use>` instances, must resolve to an element of the document tree,
 * and pattern, mask and marker content must never be hit.
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

  ParseWarningSink renderSink = ParseWarningSink::Disabled();
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, renderSink);

  DonnerController controller(document);
  const auto snapshot = controller.captureHitTest();
  const Vector2d canvasSize(document.canvasSize());
  const Transform2d documentFromCanvas = document.canvasFromDocumentTransform().inverse();
  constexpr int kGridSize = 3;
  for (int row = 0; row < kGridSize; ++row) {
    for (int column = 0; column < kGridSize; ++column) {
      const Vector2d point = documentFromCanvas.transformPosition(Vector2d(
          (column + 0.5) * canvasSize.x / kGridSize, (row + 0.5) * canvasSize.y / kGridSize));
      if (const auto hit = snapshot->findIntersecting(point); hit && !hit->tryType()) {
        std::abort();
      }
    }
  }
  const Vector2d center =
      documentFromCanvas.transformPosition(Vector2d(canvasSize.x / 2.0, canvasSize.y / 2.0));
  if (const auto hit = controller.findIntersecting(center); hit && !hit->tryType()) {
    std::abort();
  }
  for (const SVGGraphicsElement& hit : controller.findAllIntersecting(center)) {
    if (!hit.tryType()) {
      std::abort();
    }
  }

  return 0;
}

}  // namespace donner::svg
