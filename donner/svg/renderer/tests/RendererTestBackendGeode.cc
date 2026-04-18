#include "donner/base/Utils.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg {

namespace {

/// Returns a process-wide shared GeodeDevice, created on first access.
///
/// Sharing a single device across all test-constructed renderers avoids the
/// Mesa llvmpipe (and Intel ANV) hang caused by accumulating hundreds of
/// WebGPU device creations in a single process — the driver state doesn't
/// reclaim cleanly and the process eventually deadlocks.
std::shared_ptr<geode::GeodeDevice> SharedTestDevice() {
  static auto device = [] {
    auto d = geode::GeodeDevice::CreateHeadless();
    // Wrap the unique_ptr in a shared_ptr for lifetime sharing.
    return std::shared_ptr<geode::GeodeDevice>(std::move(d));
  }();
  return device;
}

}  // namespace

RendererBackend ActiveRendererBackend() {
  return RendererBackend::Geode;
}

std::string_view ActiveRendererBackendName() {
  return RendererBackendName(ActiveRendererBackend());
}

bool ActiveRendererSupportsFeature(RendererBackendFeature feature) {
  switch (feature) {
    // Text and filter effects are stubbed in the Geode skeleton. They will
    // be filled in during later phases (see docs/design_docs/0017-geode_renderer.md
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
  return std::make_unique<RendererGeode>(SharedTestDevice(), verbose);
}

RendererBitmap RenderDocumentWithActiveBackend(SVGDocument& document, bool verbose) {
  RendererGeode renderer(SharedTestDevice(), verbose);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

RendererBitmap RenderDocumentWithActiveBackendForAscii(SVGDocument& document) {
  // Geode has no anti-aliasing toggle — ASCII snapshots aren't supported.
  RendererGeode renderer(SharedTestDevice());
  renderer.draw(document);
  return renderer.takeSnapshot();
}

bool WriteActiveRendererDebugSkp(SVGDocument& document, const std::filesystem::path& outputPath) {
  UTILS_UNUSED(document);
  UTILS_UNUSED(outputPath);
  return false;
}

}  // namespace donner::svg
