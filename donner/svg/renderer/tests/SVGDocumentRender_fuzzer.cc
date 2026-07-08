#include "donner/base/ParseWarningSink.h"
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
 * not exercised by the per-element parser fuzzers.
 *
 * See https://llvm.org/docs/LibFuzzer.html
 */
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

  return 0;
}

}  // namespace donner::svg
