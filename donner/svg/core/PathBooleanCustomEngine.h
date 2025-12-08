#pragma once
/// @file

#include "donner/svg/core/PathBooleanOps.h"

namespace donner::svg {

/**
 * Simple custom Boolean engine implementation that preserves curve spans without additional
 * approximation. This placeholder combines segmented inputs according to the requested Boolean
 * operation and is intended to be replaced by the full curve-aware clipper.
 */
class PathBooleanCustomEngine : public PathBooleanEngine {
public:
  PathBooleanCustomEngine() = default;
  ~PathBooleanCustomEngine() override = default;

  [[nodiscard]] SegmentedPath compute(const PathBooleanRequest& request) override;
};

}  // namespace donner::svg

