#pragma once
/// @file

#include <gmock/gmock.h>

#include <cstddef>

#include "donner/svg/renderer/geode/GeodePathEncoder.h"

namespace donner::geode::test {

/// Prints the encoded-path shape that matters when diagnosing encoder failures.
inline void DescribeEncodedPath(const EncodedPath& encoded,
                                testing::MatchResultListener* resultListener) {
  *resultListener << "curves=" << encoded.curves.size() << ", bands=" << encoded.bands.size()
                  << ", vertices=" << encoded.vertices.size()
                  << ", quadVertices=" << encoded.quadVertices.size()
                  << ", vCurves=" << encoded.vCurves.size() << ", vBands=" << encoded.vBands.size()
                  << ", hBandGrid=" << encoded.hBandGrid.size()
                  << ", vBandGrid=" << encoded.vBandGrid.size()
                  << ", hBandCount=" << encoded.hBandCount << ", vBandCount=" << encoded.vBandCount
                  << ", bounds=(" << encoded.pathBounds.topLeft.x << ", "
                  << encoded.pathBounds.topLeft.y << ")-(" << encoded.pathBounds.bottomRight.x
                  << ", " << encoded.pathBounds.bottomRight.y << ")";
}

/// Matches an encoded path with no generated data and prints all encoded counts on failure.
MATCHER(EmptyEncodedPath, "an empty encoded path") {
  if (arg.empty() && arg.curves.empty() && arg.vertices.empty() && arg.quadVertices.empty() &&
      arg.vCurves.empty() && arg.vBands.empty() && arg.hBandGrid.empty() && arg.vBandGrid.empty()) {
    return true;
  }

  DescribeEncodedPath(arg, result_listener);
  return false;
}

/// Matches an encoded path with at least one horizontal band and prints all counts on failure.
MATCHER(NonEmptyEncodedPath, "a non-empty encoded path") {
  if (!arg.empty()) {
    return true;
  }

  DescribeEncodedPath(arg, result_listener);
  return false;
}

/// Matches an encoded path whose legacy band vertices contain two triangles per band.
MATCHER(HasBandVertexQuads, "band vertices sized as two triangles per band") {
  const size_t expectedVertices = arg.bands.size() * 6u;
  if (arg.vertices.size() == expectedVertices) {
    return true;
  }

  *result_listener << "vertices=" << arg.vertices.size()
                   << ", expected vertices=" << expectedVertices << ", bands=" << arg.bands.size();
  return false;
}

/// Matches an encoded path whose dense band-grid arrays match their advertised counts.
MATCHER(HasBandGridsSizedToCounts, "band grids sized to their advertised counts") {
  const bool hMatches = arg.hBandGrid.size() == arg.hBandCount;
  const bool vMatches = arg.vBandGrid.size() == arg.vBandCount;
  if (hMatches && vMatches) {
    return true;
  }

  *result_listener << "hBandGrid=" << arg.hBandGrid.size() << ", hBandCount=" << arg.hBandCount
                   << ", vBandGrid=" << arg.vBandGrid.size() << ", vBandCount=" << arg.vBandCount;
  return false;
}

/// Matches an object that explicitly converts to true as a WebGPU-style handle.
MATCHER(TruthyWgpuHandle, "a truthy WebGPU handle") {
  if (static_cast<bool>(arg)) {
    return true;
  }

  *result_listener << "handle converts to false";
  return false;
}

/// Matches an object that explicitly converts to false as a WebGPU-style handle.
MATCHER(FalsyWgpuHandle, "a falsy WebGPU handle") {
  if (!static_cast<bool>(arg)) {
    return true;
  }

  *result_listener << "handle converts to true";
  return false;
}

}  // namespace donner::geode::test
