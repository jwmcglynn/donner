#include <algorithm>

#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/PathGeometry.h"

namespace tiny_skia {

namespace {

std::size_t computeQuadExtremas(Point p0, Point p1, Point p2, Point extremas[5]) {
  std::size_t idx = 0;
  if (auto t = pathGeometry::findQuadExtrema(p0.x, p1.x, p2.x)) {
    const Point src[3] = {p0, p1, p2};
    extremas[idx++] = pathGeometry::evalQuadAt(src, t->toNormalized());
  }
  if (auto t = pathGeometry::findQuadExtrema(p0.y, p1.y, p2.y)) {
    const Point src[3] = {p0, p1, p2};
    extremas[idx++] = pathGeometry::evalQuadAt(src, t->toNormalized());
  }
  extremas[idx] = p2;
  return idx + 1;
}

std::size_t computeCubicExtremas(Point p0, Point p1, Point p2, Point p3, Point extremas[5]) {
  auto ts0 = pathGeometry::newTValues();
  auto ts1 = pathGeometry::newTValues();
  const auto n0 = pathGeometry::findCubicExtremaT(p0.x, p1.x, p2.x, p3.x, ts0.data());
  const auto n1 = pathGeometry::findCubicExtremaT(p0.y, p1.y, p2.y, p3.y, ts1.data());
  const auto totalLen = n0 + n1;

  const Point src[4] = {p0, p1, p2, p3};
  std::size_t idx = 0;
  for (std::size_t i = 0; i < n0; ++i) {
    extremas[idx++] = pathGeometry::evalCubicPosAt(src, ts0[i].toNormalized());
  }
  for (std::size_t i = 0; i < n1; ++i) {
    extremas[idx++] = pathGeometry::evalCubicPosAt(src, ts1[i].toNormalized());
  }
  extremas[totalLen] = p3;
  return totalLen + 1;
}

}  // namespace

std::optional<Rect> Path::computeTightBounds() const {
  if (points_.empty()) {
    return std::nullopt;
  }

  Point extremas[5] = {};
  auto minPt = points_[0];
  auto maxPt = points_[0];

  PathSegmentsIter iter(*this);
  Point lastPt = Point::zero();
  while (auto seg = iter.next()) {
    std::size_t count = 0;
    switch (seg->kind) {
      case PathSegment::Kind::MoveTo:
        extremas[0] = seg->pts[0];
        count = 1;
        break;
      case PathSegment::Kind::LineTo:
        extremas[0] = seg->pts[0];
        count = 1;
        break;
      case PathSegment::Kind::QuadTo:
        count = computeQuadExtremas(lastPt, seg->pts[0], seg->pts[1], extremas);
        break;
      case PathSegment::Kind::CubicTo:
        count = computeCubicExtremas(lastPt, seg->pts[0], seg->pts[1], seg->pts[2], extremas);
        break;
      case PathSegment::Kind::Close:
        break;
    }

    lastPt = iter.lastPoint();
    for (std::size_t i = 0; i < count; ++i) {
      minPt.x = std::min(minPt.x, extremas[i].x);
      minPt.y = std::min(minPt.y, extremas[i].y);
      maxPt.x = std::max(maxPt.x, extremas[i].x);
      maxPt.y = std::max(maxPt.y, extremas[i].y);
    }
  }

  return Rect::fromLTRB(minPt.x, minPt.y, maxPt.x, maxPt.y);
}

PathBuilder Path::clear() {
  verbs_.clear();
  points_.clear();
  bounds_.reset();
  PathBuilder builder(verbs_.capacity(), points_.capacity());
  return builder;
}

PathSegment PathSegmentsIter::autoClose() {
  if (isAutoClose_ && lastPoint_ != lastMoveTo_) {
    verbIndex_--;
    PathSegment seg;
    seg.kind = PathSegment::Kind::LineTo;
    seg.pts[0] = lastMoveTo_;
    return seg;
  }
  PathSegment seg;
  seg.kind = PathSegment::Kind::Close;
  return seg;
}

std::optional<PathSegment> PathSegmentsIter::next() {
  auto verbs = path_->verbs();
  auto pts = path_->points();

  if (verbIndex_ >= verbs.size()) {
    return std::nullopt;
  }

  PathVerb verb = verbs[verbIndex_];
  verbIndex_++;

  PathSegment seg;
  switch (verb) {
    case PathVerb::Move: {
      pointsIndex_++;
      lastMoveTo_ = pts[pointsIndex_ - 1];
      lastPoint_ = lastMoveTo_;
      seg.kind = PathSegment::Kind::MoveTo;
      seg.pts[0] = lastMoveTo_;
      break;
    }
    case PathVerb::Line: {
      pointsIndex_++;
      lastPoint_ = pts[pointsIndex_ - 1];
      seg.kind = PathSegment::Kind::LineTo;
      seg.pts[0] = lastPoint_;
      break;
    }
    case PathVerb::Quad: {
      pointsIndex_ += 2;
      lastPoint_ = pts[pointsIndex_ - 1];
      seg.kind = PathSegment::Kind::QuadTo;
      seg.pts[0] = pts[pointsIndex_ - 2];
      seg.pts[1] = lastPoint_;
      break;
    }
    case PathVerb::Cubic: {
      pointsIndex_ += 3;
      lastPoint_ = pts[pointsIndex_ - 1];
      seg.kind = PathSegment::Kind::CubicTo;
      seg.pts[0] = pts[pointsIndex_ - 3];
      seg.pts[1] = pts[pointsIndex_ - 2];
      seg.pts[2] = lastPoint_;
      break;
    }
    case PathVerb::Close: {
      seg = autoClose();
      lastPoint_ = lastMoveTo_;
      break;
    }
  }

  return seg;
}

bool PathSegmentsIter::hasValidTangent() const {
  // Clone iterator state.
  PathSegmentsIter iter = *this;
  while (auto seg = iter.next()) {
    switch (seg->kind) {
      case PathSegment::Kind::MoveTo:
        return false;
      case PathSegment::Kind::LineTo:
        if (iter.lastPoint_ == seg->pts[0]) continue;
        return true;
      case PathSegment::Kind::QuadTo:
        if (iter.lastPoint_ == seg->pts[0] && iter.lastPoint_ == seg->pts[1]) continue;
        return true;
      case PathSegment::Kind::CubicTo:
        if (iter.lastPoint_ == seg->pts[0] && iter.lastPoint_ == seg->pts[1] &&
            iter.lastPoint_ == seg->pts[2])
          continue;
        return true;
      case PathSegment::Kind::Close:
        return false;
    }
  }
  return false;
}

}  // namespace tiny_skia
