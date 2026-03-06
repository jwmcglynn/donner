#pragma once

#include "tiny_skia/PathGeometry.h"

namespace tiny_skia::pathGeometry::coreRs {

// Wrappers for functions owned by `third_party/tiny-skia/src/pathGeometry.rs`.

inline std::size_t chopQuadAtXExtrema(const std::array<Point, 3>& src, std::array<Point, 5>& dst) {
  return ::tiny_skia::pathGeometry::chopQuadAtXExtrema(src, dst);
}

inline std::size_t chopQuadAtYExtrema(const std::array<Point, 3>& src, std::array<Point, 5>& dst) {
  return ::tiny_skia::pathGeometry::chopQuadAtYExtrema(src, dst);
}

inline std::size_t chopCubicAtXExtrema(const std::array<Point, 4>& src, std::array<Point, 10>& dst) {
  return ::tiny_skia::pathGeometry::chopCubicAtXExtrema(src, dst);
}

inline std::size_t chopCubicAtYExtrema(const std::array<Point, 4>& src, std::array<Point, 10>& dst) {
  return ::tiny_skia::pathGeometry::chopCubicAtYExtrema(src, dst);
}

inline std::size_t chopCubicAtMaxCurvature(const std::array<Point, 4>& src,
                                           std::array<NormalizedF32Exclusive, 3>& tValues,
                                           std::span<Point> dst) {
  return ::tiny_skia::pathGeometry::chopCubicAtMaxCurvature(src, tValues, dst);
}

inline bool chopMonoQuadAtX(const std::array<Point, 3>& src, float x, float& t) {
  return ::tiny_skia::pathGeometry::chopMonoQuadAtX(src, x, t);
}

inline bool chopMonoQuadAtY(const std::array<Point, 3>& src, float y, float& t) {
  return ::tiny_skia::pathGeometry::chopMonoQuadAtY(src, y, t);
}

inline bool chopMonoCubicAtX(const std::array<Point, 4>& src, float x, std::array<Point, 7>& dst) {
  return ::tiny_skia::pathGeometry::chopMonoCubicAtX(src, x, dst);
}

inline bool chopMonoCubicAtY(const std::array<Point, 4>& src, float y, std::array<Point, 7>& dst) {
  return ::tiny_skia::pathGeometry::chopMonoCubicAtY(src, y, dst);
}

}  // namespace tiny_skia::pathGeometry::coreRs
