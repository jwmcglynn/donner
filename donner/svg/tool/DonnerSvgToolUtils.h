#pragma once
/// @file

#include <string>

#include "donner/base/Box.h"
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
void CompositeAABBRect(RendererBitmap& bitmap, const Boxd& bounds,
                       const SampledImageInfo& imageInfo);

}  // namespace donner::svg
