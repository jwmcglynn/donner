#pragma once
/// @file
/// Device-aware cubic-to-quadratic approximation for filled paths.

#include <algorithm>
#include <cmath>

#include "donner/svg/renderer/geode/GeodeStrokeTolerance.h"

namespace donner::geode {

/// Maximum cubic approximation error in device pixels and at identity scale.
inline constexpr double kFillCubicDevicePixels = 0.1;

/// Bounds approximation work; the device-pixel bound holds through 100,000x scale.
inline constexpr double kMinFillCubicTolerance = 1e-6;

/**
 * Path-local cubic approximation tolerance for the draw-time transform.
 *
 * Power-of-two scale buckets allow reuse during zoom. Minified paths retain
 * the historical 0.1 tolerance; magnified paths are refined conservatively.
 *
 * @param deviceFromLocal Transform applied to the encoded path at draw time.
 */
inline double FillCubicToleranceFor(const Transform2d& deviceFromLocal) {
  double largestCoefficient = 0.0;
  for (double coefficient : {deviceFromLocal.data[0], deviceFromLocal.data[1],
                             deviceFromLocal.data[2], deviceFromLocal.data[3]}) {
    if (!std::isfinite(coefficient)) {
      return kMinFillCubicTolerance;
    }
    largestCoefficient = std::max(largestCoefficient, std::abs(coefficient));
  }
  // Clamp before squaring large coefficients in the singular-value calculation.
  if (largestCoefficient >= kFillCubicDevicePixels / kMinFillCubicTolerance) {
    return kMinFillCubicTolerance;
  }
  const double scale = MaxAbsScaleFactor(deviceFromLocal);
  if (!std::isfinite(scale)) {
    return kMinFillCubicTolerance;
  }
  return std::clamp(kFillCubicDevicePixels / StrokeFlattenScaleBucket(scale),
                    kMinFillCubicTolerance, kFillCubicDevicePixels);
}

}  // namespace donner::geode
