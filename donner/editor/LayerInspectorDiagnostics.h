#pragma once
/// @file

#include <string_view>

#include "donner/base/Vector2.h"

namespace donner::editor {

/// Canvas freshness state shown by the layer inspector.
enum class CanvasFreshness {
  Current,
  CommitStalled,
  CompositorBehind,
};

/// Classify desired, document, and compositor canvas-size agreement.
///
/// @param viewportDesiredCanvas Canvas size implied by the current viewport.
/// @param documentCanvas Canvas size committed to the SVG document.
/// @param compositorCanvas Canvas size last rasterized by the compositor.
[[nodiscard]] CanvasFreshness ClassifyCanvasFreshness(const Vector2i& viewportDesiredCanvas,
                                                      const Vector2i& documentCanvas,
                                                      const Vector2i& compositorCanvas);

/// User-visible status suffix for a canvas freshness state.
///
/// @param freshness State returned by \ref ClassifyCanvasFreshness.
[[nodiscard]] std::string_view CanvasFreshnessStatusSuffix(CanvasFreshness freshness);

}  // namespace donner::editor
