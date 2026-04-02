#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg {

RendererBackend ActiveRendererBackend() {
  return RendererBackend::TinySkia;
}

std::string_view ActiveRendererBackendName() {
  return RendererBackendName(ActiveRendererBackend());
}

bool ActiveRendererSupportsFeature(RendererBackendFeature feature) {
  switch (feature) {
    case RendererBackendFeature::FilterEffects: return true;
    case RendererBackendFeature::Text:
    case RendererBackendFeature::AsciiSnapshot:
    case RendererBackendFeature::SkpDebug: return false;
  }

  return false;
}

RendererBitmap RenderDocumentWithActiveBackend(SVGDocument& document, bool verbose) {
  RendererTinySkia renderer(verbose);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

bool WriteActiveRendererDebugSkp(SVGDocument& document, const std::filesystem::path& outputPath) {
  (void)document;
  (void)outputPath;
  return false;
}

}  // namespace donner::svg
