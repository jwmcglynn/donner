#pragma once
/// @file

#include "donner/svg/core/FillRule.h"
#include "donner/svg/core/PathBooleanReconstructor.h"
#include "donner/svg/core/PathBooleanSegmenter.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::svg {

/**
 * Supported Boolean operations between two paths.
 */
enum class PathBooleanOp {
  kUnion,
  kIntersection,
  kDifference,
  kReverseDifference,
  kXor,
};

/**
 * Aggregated request data passed to a PathBooleanEngine implementation.
 */
struct PathBooleanRequest {
  PathBooleanOp op;          ///< Operation to perform.
  FillRule subjectFillRule;  ///< Fill rule for the subject path.
  FillRule clipFillRule;     ///< Fill rule for the clip path.
  double tolerance;          ///< Segmentation and predicate tolerance.
  SegmentedPath subject;     ///< Segmented subject path.
  SegmentedPath clip;        ///< Segmented clip path.
};

/**
 * Mock-friendly interface for the custom Boolean kernel.
 */
class PathBooleanEngine {
public:
  virtual ~PathBooleanEngine() = default;

  /**
   * Execute a Boolean operation using the provided segmented paths.
   */
  virtual SegmentedPath compute(const PathBooleanRequest& request) = 0;
};

/**
 * Adapter responsible for preparing PathSpline inputs for Boolean processing and delegating to
 * the injected engine implementation.
 */
class PathBooleanOps {
public:
  static PathSpline Compute(const PathSpline& subject, const PathSpline& clip, PathBooleanOp op,
                            FillRule subjectFillRule, FillRule clipFillRule,
                            PathBooleanEngine& engine,
                            double tolerance = kDefaultSegmentationTolerance);
};

}  // namespace donner::svg
