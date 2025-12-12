#include "donner/svg/core/PathBooleanCustomEngine.h"

namespace donner::svg {

namespace {

[[nodiscard]] SegmentedPath ConcatPaths(const SegmentedPath& a, const SegmentedPath& b) {
  SegmentedPath result;
  result.subpaths.reserve(a.subpaths.size() + b.subpaths.size());
  result.subpaths.insert(result.subpaths.end(), a.subpaths.begin(), a.subpaths.end());
  result.subpaths.insert(result.subpaths.end(), b.subpaths.begin(), b.subpaths.end());
  return result;
}

}  // namespace

SegmentedPath PathBooleanCustomEngine::compute(const PathBooleanRequest& request) {
  switch (request.op) {
    case PathBooleanOp::kUnion:
    case PathBooleanOp::kXor: return ConcatPaths(request.subject, request.clip);

    case PathBooleanOp::kDifference: return request.subject;

    case PathBooleanOp::kReverseDifference: return request.clip;

    case PathBooleanOp::kIntersection: return {};
  }

  return {};
}

}  // namespace donner::svg

