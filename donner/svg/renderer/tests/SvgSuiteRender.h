#pragma once
/// @file
/// Native rendering setup shared by suite runners.

#include <filesystem>

#include "tools/svg_suite/ImageCompare.h"

namespace donner::svg {

/// Explicit offline inputs for one corpus render.
struct SvgSuiteRenderSettings {
  std::filesystem::path input;
  std::filesystem::path resourceRoot;
  std::filesystem::path fontRoot;
  int width = 0;
  int height = 0;
  /// Optional separate root for an explicitly staged input. Relative resources still resolve
  /// from the input's directory, and fetched resources remain confined to resourceRoot.
  std::filesystem::path inputRoot;
};

/// Render one SVG through the production software backend and the corpus font configuration.
/// @param settings Bounded output size and explicit input/resource/font roots.
svg_suite::ImageResult RenderSvgSuiteCase(const SvgSuiteRenderSettings& settings);

}  // namespace donner::svg
