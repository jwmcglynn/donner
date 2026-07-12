#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"

namespace donner::svg {

/// Maximum accepted raster dimension for one pattern-tile axis.
inline constexpr int kMaxPatternTileRasterDimension = 4096;

/**
 * Validated dimensions and scale for a rasterized pattern tile.
 */
struct PatternTileRasterMetrics {
  int pixelWidth;
  int pixelHeight;
  Vector2d rasterFromPatternScale;
};

/**
 * Computes bounded raster dimensions for a logical pattern tile.
 *
 * @param tileRect Logical pattern tile rectangle.
 * @param deviceFromPattern Transform from logical pattern coordinates to device pixels.
 * @return Raster metrics, or nullopt when an input is non-finite, non-positive, singular, or would
 *     exceed \ref kMaxPatternTileRasterDimension.
 */
std::optional<PatternTileRasterMetrics> ComputePatternTileRasterMetrics(
    const Box2d& tileRect, const Transform2d& deviceFromPattern);

/**
 * Converts a logical pattern-to-target transform into a raster-tile-to-target transform.
 *
 * @param targetFromPattern Transform from logical pattern coordinates to target coordinates.
 * @param rasterFromPatternScale Raster pixels per logical pattern unit.
 * @return Transform from raster-tile pixel coordinates to target coordinates.
 */
Transform2d TargetFromPatternRaster(const Transform2d& targetFromPattern,
                                    const Vector2d& rasterFromPatternScale);

}  // namespace donner::svg
