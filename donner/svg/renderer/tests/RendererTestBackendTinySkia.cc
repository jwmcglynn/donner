#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg {

namespace {

bool TinySkiaSupportsFeature(RendererBackendFeature feature) {
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

std::unique_ptr<RendererInterface> TinySkiaCreateInstance(bool verbose) {
  return std::make_unique<RendererTinySkia>(verbose);
}

RendererBitmap TinySkiaRender(SVGDocument& document, bool verbose) {
  RendererTinySkia renderer(verbose);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

RendererBitmap TinySkiaRenderForAscii(SVGDocument& document) {
  RendererTinySkia renderer;
  renderer.setAntialias(false);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

}  // namespace

void RegisterTinySkiaBackend() {
  RegisterBackendOps(RendererBackend::TinySkia, BackendOps{
                                                    .render = &TinySkiaRender,
                                                    .renderForAscii = &TinySkiaRenderForAscii,
                                                    .supportsFeature = &TinySkiaSupportsFeature,
                                                    .createInstance = &TinySkiaCreateInstance,
                                                });
}

}  // namespace donner::svg
