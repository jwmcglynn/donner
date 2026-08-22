#pragma once
/// @file

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/Box.h"
#include "donner/base/TerminalEscape.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {

/**
 * Build a CSS-like selector path for an element.
 *
 * @param element Element to describe.
 * @return Selector path from the root to the element.
 */
std::string BuildCssSelectorPath(const SVGElement& element);

/**
 * Return the narrowest sandbox root for an absolute input document path.
 *
 * A document grants access only to its containing directory, never to an ambient process working
 * directory.
 */
std::optional<std::filesystem::path> ResourceSandboxRootForAbsoluteInput(
    const std::filesystem::path& absoluteInputPath);

using ::donner::EscapeTerminalText;

/** Sampled image dimensions and scaling for coordinate mapping. */
struct SampledImageInfo {
  int columns = 0;
  int rows = 0;
  /// Inverse scale: maps sub-pixel index to image X pixel via int(subPixel * xScale).
  double xScale = 1.0;
  /// Inverse scale: maps sub-pixel index to image Y pixel via int(subPixel * yScale).
  double yScale = 1.0;
};

/**
 * Draw a 1-sub-pixel blue AABB outline directly into the bitmap, aligned to the terminal
 * sub-pixel grid.
 *
 * @param bitmap Bitmap to draw into (modified in-place).
 * @param bounds AABB in image coordinates.
 * @param imageInfo Terminal sampling info for sub-pixel alignment.
 */
void CompositeAABBRect(RendererBitmap& bitmap, const Box2d& bounds,
                       const SampledImageInfo& imageInfo);

}  // namespace donner::svg
