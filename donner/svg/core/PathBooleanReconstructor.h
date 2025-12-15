#pragma once
/// @file

#include "donner/svg/core/PathBooleanSegmenter.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::svg {

/**
 * Utility functions for rebuilding PathSpline instances from segmented curve spans.
 */
class PathBooleanReconstructor {
public:
  /**
   * Reconstruct a PathSpline using the spans from a segmented path.
   *
   * Each subpath is emitted with an initial MoveTo followed by the spans in order. Curve spans are
   * preserved as curves and ClosePath spans are honored when present.
   */
  static PathSpline Reconstruct(const SegmentedPath& segmented);
};

}  // namespace donner::svg
