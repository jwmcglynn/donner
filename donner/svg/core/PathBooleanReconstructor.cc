#include "donner/svg/core/PathBooleanReconstructor.h"

namespace donner::svg {
namespace {

void AppendSpan(PathSpline* path, const PathCurveSpan& span) {
  switch (span.type) {
    case PathSpline::CommandType::LineTo:
      path->lineTo(span.endPoint);
      return;
    case PathSpline::CommandType::ClosePath:
      path->closePath();
      return;
    case PathSpline::CommandType::CurveTo: {
      path->curveTo(span.controlPoint1, span.controlPoint2, span.endPoint);
      return;
    }
    case PathSpline::CommandType::MoveTo: {
      // MoveTo spans are not expected in segmented paths; these are represented by subpath metadata.
      return;
    }
  }
}

}  // namespace

PathSpline PathBooleanReconstructor::Reconstruct(const SegmentedPath& segmented) {
  PathSpline rebuilt;

  for (const PathSubpathView& subpath : segmented.subpaths) {
    rebuilt.moveTo(subpath.moveTo);

    for (const PathCurveSpan& span : subpath.spans) {
      AppendSpan(&rebuilt, span);
      if (span.type == PathSpline::CommandType::ClosePath) {
        // ClosePath spans already return to moveTo; no additional commands needed.
        break;
      }
    }

    if (subpath.closed && (subpath.spans.empty() ||
                           subpath.spans.back().type != PathSpline::CommandType::ClosePath)) {
      rebuilt.closePath();
    }
  }

  return rebuilt;
}

}  // namespace donner::svg
