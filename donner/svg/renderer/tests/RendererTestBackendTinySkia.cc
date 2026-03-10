#include "donner/base/Utils.h"
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
#ifdef DONNER_FILTERS_ENABLED
    case RendererBackendFeature::FilterEffects: return true;
#else
    case RendererBackendFeature::FilterEffects: return false;
#endif
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
  UTILS_UNUSED(document);
  UTILS_UNUSED(outputPath);
  return false;
}

}  // namespace donner::svg
