#include <gtest/gtest.h>

#include <cstdlib>
#include <string_view>

#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg {

namespace {

/// How the environment asked retained rasterization to be exercised.
///
/// A suite that draws each document once only exercises the recording half of retained
/// rasterization, so the mode is selected per run rather than compiled in:
///
/// - `off`: the default. One frame, no retention.
/// - `replay` (`DONNER_TINYSKIA_RETAINED=1`): draw a second frame through the warmed cache and
///   return that one, so the whole corpus is checked against its goldens with replayed
///   coverage.
/// - `compare` (`DONNER_TINYSKIA_RETAINED=compare`): additionally render the same document on a
///   renderer that retains nothing and fail unless the two frames are byte-equal, which turns
///   every corpus document into a retained-versus-fresh identity case.
enum class RetainedMode { Off, Replay, Compare };

RetainedMode RequestedRetainedMode() {
  static const RetainedMode mode = [] {
    const char* value = std::getenv("DONNER_TINYSKIA_RETAINED");
    if (value == nullptr) {
      return RetainedMode::Off;
    }

    const std::string_view text(value);
    if (text == "compare") {
      return RetainedMode::Compare;
    }
    return text == "1" ? RetainedMode::Replay : RetainedMode::Off;
  }();
  return mode;
}

/// Reports the first differing byte, so a corpus-wide failure names a pixel instead of a file.
void ExpectBitmapsEqual(const RendererBitmap& fresh, const RendererBitmap& retained) {
  ASSERT_EQ(fresh.dimensions, retained.dimensions);
  ASSERT_EQ(fresh.rowBytes, retained.rowBytes);
  ASSERT_EQ(fresh.pixels.size(), retained.pixels.size());

  for (std::size_t i = 0; i < fresh.pixels.size(); ++i) {
    if (fresh.pixels[i] != retained.pixels[i]) {
      const std::size_t pixel = i / 4;
      const int x = static_cast<int>(pixel % static_cast<std::size_t>(fresh.dimensions.x));
      const int y = static_cast<int>(pixel / static_cast<std::size_t>(fresh.dimensions.x));
      FAIL() << "retained rendering differs from fresh rendering at pixel (" << x << ", " << y
             << ") channel " << (i % 4) << ": " << static_cast<int>(fresh.pixels[i]) << " vs "
             << static_cast<int>(retained.pixels[i]);
    }
  }
}

/// Draws `document` and returns the frame the current mode is meant to check.
RendererBitmap RenderSettled(RendererTinySkia& renderer, SVGDocument& document) {
  const RetainedMode mode = RequestedRetainedMode();
  if (mode == RetainedMode::Off) {
    renderer.draw(document);
    return renderer.takeSnapshot();
  }

  RendererBitmap fresh;
  if (mode == RetainedMode::Compare) {
    RendererTinySkia freshRenderer;
    freshRenderer.setAntialias(renderer.antialias());
    freshRenderer.draw(document);
    fresh = freshRenderer.takeSnapshot();
  }

  renderer.setRetainedSpansEnabled(true);
  renderer.draw(document);
  renderer.draw(document);
  RendererBitmap retained = renderer.takeSnapshot();

  if (mode == RetainedMode::Compare) {
    ExpectBitmapsEqual(fresh, retained);
  }
  return retained;
}

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
  auto renderer = std::make_unique<RendererTinySkia>(verbose);
  renderer->setRetainedSpansEnabled(RequestedRetainedMode() != RetainedMode::Off);
  return renderer;
}

RendererBitmap TinySkiaRender(SVGDocument& document, bool verbose) {
  RendererTinySkia renderer(verbose);
  return RenderSettled(renderer, document);
}

RendererBitmap TinySkiaRenderForAscii(SVGDocument& document) {
  RendererTinySkia renderer;
  renderer.setAntialias(false);
  return RenderSettled(renderer, document);
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
