#include "donner/base/Utils.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg {

RendererBackend ActiveRendererBackend() {
  return RendererBackend::Geode;
}

std::string_view ActiveRendererBackendName() {
  return RendererBackendName(ActiveRendererBackend());
}

bool ActiveRendererSupportsFeature(RendererBackendFeature feature) {
  switch (feature) {
    // Text and filter effects are stubbed in the Geode skeleton. They will
    // be filled in during later phases (see docs/design_docs/geode_renderer.md
    // — Phase 4 for text, Phase 7 for filters).
    case RendererBackendFeature::FilterEffects: return false;
    case RendererBackendFeature::Text: return false;
    case RendererBackendFeature::TextFull: return false;
    case RendererBackendFeature::AsciiSnapshot: return false;
    case RendererBackendFeature::SkpDebug: return false;
  }

  return false;
}

std::unique_ptr<RendererInterface> CreateActiveRendererInstance(bool verbose) {
  return std::make_unique<RendererGeode>(verbose);
}

RendererBitmap RenderDocumentWithActiveBackend(SVGDocument& document, bool verbose) {
  RendererGeode renderer(verbose);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

RendererBitmap RenderDocumentWithActiveBackendForAscii(SVGDocument& document) {
  // Geode has no anti-aliasing toggle — ASCII snapshots aren't supported.
  RendererGeode renderer;
  renderer.draw(document);
  return renderer.takeSnapshot();
}

bool WriteActiveRendererDebugSkp(SVGDocument& document, const std::filesystem::path& outputPath) {
  UTILS_UNUSED(document);
  UTILS_UNUSED(outputPath);
  return false;
}

}  // namespace donner::svg
