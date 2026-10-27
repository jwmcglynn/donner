#include "donner/editor/EditorSampleCatalog.h"

#include <array>

#include "donner/editor/EditorSamples.h"
#include "donner/editor/EditorSplash.h"

namespace donner::editor {
namespace {
const std::array<EditorSample, 6>& CatalogStorage() {
  static const std::array<EditorSample, 6> samples = {{
      {"donner-splash", "Donner Splash", {embedded::kEditorSplashSvg}},
      {"geode-splash", "Geode Splash", {embedded::kEditorGeodeSplashSvg}},
      {"basic-shapes", "Basic Shapes", {embedded::kBasicShapesSvg}},
      {"text-style", "Text and Style", {embedded::kTextStyleSvg}},
      {"gradients-clip", "Gradients and Clip", {embedded::kGradientsClipSvg}},
      {"animation", "Animation", {embedded::kAnimationSvg}},
  }};
  return samples;
}
}  // namespace

std::string_view EditorSampleSource::view() const {
  const auto bytes = resource.view();
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

bool EditorSampleSource::request() const {
#ifdef __EMSCRIPTEN__
  if (!resource.ready() && !resource.failed()) {
    RequestBrowserPackagedResource(resource.sha256);
    return false;
  }
  return resource.ready();
#else
  return static_cast<bool>(resource.load());
#endif
}

std::span<const EditorSample> GetEditorSampleCatalog() noexcept {
  return CatalogStorage();
}

const EditorSample* FindEditorSample(std::string_view id) noexcept {
  for (const EditorSample& sample : CatalogStorage()) {
    if (sample.id == id) {
      return &sample;
    }
  }
  return nullptr;
}
}  // namespace donner::editor
