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
#ifdef DONNER_TEXT_ENABLED
    case RendererBackendFeature::Text: return true;
#else
    case RendererBackendFeature::Text: return false;
#endif
#ifdef DONNER_TEXT_FULL
    case RendererBackendFeature::TextFull: return true;
#else
    case RendererBackendFeature::TextFull: return false;
#endif
    case RendererBackendFeature::AsciiSnapshot: return true;
  }

  return false;
}

std::unique_ptr<RendererInterface> CreateActiveRendererInstance(bool verbose) {
  return std::make_unique<RendererTinySkia>(verbose);
}

RendererBitmap RenderDocumentWithActiveBackend(SVGDocument& document, bool verbose) {
  RendererTinySkia renderer(verbose);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

RendererBitmap RenderDocumentWithActiveBackendForAscii(SVGDocument& document) {
  RendererTinySkia renderer;
  renderer.setAntialias(false);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

}  // namespace donner::svg
