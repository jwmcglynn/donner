#pragma once
/// @file
/// GPU-independent resource fixtures for the sealed presentation contract.

#include "donner/editor/FramePresentation.h"

namespace donner::editor {

struct FramePresentationTestAccess {
  static void installResources(
      GlTextureCache& cache,
      std::shared_ptr<const GlTextureCache::PresentationResources> resources) {
    cache.overviewTiles_ = resources->overviewTiles();
    cache.presentationResources_ = std::move(resources);
  }

  static std::shared_ptr<const GlTextureCache::PresentationResources> resources(
      std::shared_ptr<const CapturedPresentation> capture,
      std::vector<GlTextureCache::TileView> tiles, PresentationCoverageDiagnostics coverage = {},
      std::shared_ptr<const CapturedPresentation> overview = nullptr,
      std::vector<GlTextureCache::TileView> overviewTiles = {}) {
    auto result = std::shared_ptr<GlTextureCache::PresentationResources>(
        new GlTextureCache::PresentationResources());
    result->capture_ = std::move(capture);
    result->tiles_ = std::move(tiles);
    result->coverage_ = coverage;
    result->overviewCapture_ = std::move(overview);
    result->overviewTiles_ = std::move(overviewTiles);
    result->indexCoverage();
    return result;
  }
};

}  // namespace donner::editor
