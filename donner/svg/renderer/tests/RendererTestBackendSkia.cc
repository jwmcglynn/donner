#include <fstream>

#include "donner/svg/renderer/RendererSkia.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "include/core/SkData.h"
#include "include/core/SkPicture.h"

namespace donner::svg {

RendererBackend ActiveRendererBackend() {
  return RendererBackend::Skia;
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
    case RendererBackendFeature::AsciiSnapshot:
    case RendererBackendFeature::SkpDebug: return true;
  }

  return false;
}

std::unique_ptr<RendererInterface> CreateActiveRendererInstance(bool verbose) {
  return std::make_unique<RendererSkia>(verbose);
}

RendererBitmap RenderDocumentWithActiveBackend(SVGDocument& document, bool verbose) {
  RendererSkia renderer(verbose);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

RendererBitmap RenderDocumentWithActiveBackendForAscii(SVGDocument& document) {
  RendererSkia renderer;
  renderer.setAntialias(false);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

bool WriteActiveRendererDebugSkp(SVGDocument& document, const std::filesystem::path& outputPath) {
  RendererSkia renderer(/*verbose=*/true);
  sk_sp<SkPicture> picture = renderer.drawIntoSkPicture(document);
  sk_sp<SkData> pictureData = picture->serialize();

  std::ofstream file(outputPath, std::ios::binary);
  if (!file) {
    return false;
  }

  file.write(reinterpret_cast<const char*>(pictureData->data()),  // NOLINT: Intentional cast.
             static_cast<std::streamsize>(pictureData->size()));
  return file.good();
}

}  // namespace donner::svg
