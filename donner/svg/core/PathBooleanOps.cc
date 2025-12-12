#include "donner/svg/core/PathBooleanOps.h"

#include "donner/svg/core/PathBooleanReconstructor.h"

namespace donner::svg {
namespace {

[[nodiscard]] bool PathIsEmpty(const PathSpline& path) {
  return path.commands().empty();
}

}  // namespace

PathSpline PathBooleanOps::Compute(const PathSpline& subject, const PathSpline& clip,
                                   PathBooleanOp op, FillRule subjectFillRule,
                                   FillRule clipFillRule, PathBooleanEngine& engine,
                                   double tolerance) {
  const bool subjectEmpty = PathIsEmpty(subject);
  const bool clipEmpty = PathIsEmpty(clip);

  if (subjectEmpty || clipEmpty) {
    switch (op) {
      case PathBooleanOp::kUnion:
      case PathBooleanOp::kXor: return subjectEmpty ? clip : subject;
      case PathBooleanOp::kDifference: return subjectEmpty ? PathSpline{} : subject;
      case PathBooleanOp::kReverseDifference: return clipEmpty ? PathSpline{} : clip;
      case PathBooleanOp::kIntersection: return PathSpline{};
    }
  }

  const double effectiveTolerance = tolerance > 0.0 ? tolerance : kDefaultSegmentationTolerance;

  PathBooleanRequest request{.op = op,
                             .subjectFillRule = subjectFillRule,
                             .clipFillRule = clipFillRule,
                             .tolerance = effectiveTolerance,
                             .subject = SegmentPathForBoolean(subject, effectiveTolerance),
                             .clip = SegmentPathForBoolean(clip, effectiveTolerance)};

  const SegmentedPath result = engine.compute(request);
  if (result.subpaths.empty()) {
    return PathSpline{};
  }

  return PathBooleanReconstructor::Reconstruct(result);
}

}  // namespace donner::svg
