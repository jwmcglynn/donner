#include "tiny_skia/PathBuilder.h"

#include <algorithm>
#include <cmath>

#include "tiny_skia/Geom.h"
#include "tiny_skia/PathGeometry.h"

namespace tiny_skia {

PathBuilder& PathBuilder::moveTo(float x, float y) {
  if (!verbs_.empty() && verbs_.back() == PathVerb::Move) {
    points_.back() = Point::fromXY(x, y);
  } else {
    lastMoveToIndex_ = points_.size();
    moveToRequired_ = false;
    verbs_.push_back(PathVerb::Move);
    points_.push_back(Point::fromXY(x, y));
  }
  return *this;
}

void PathBuilder::injectMoveToIfNeeded() {
  if (moveToRequired_) {
    if (lastMoveToIndex_ < points_.size()) {
      auto p = points_[lastMoveToIndex_];
      moveTo(p.x, p.y);
    } else {
      moveTo(0.0f, 0.0f);
    }
  }
}

PathBuilder& PathBuilder::lineTo(float x, float y) {
  injectMoveToIfNeeded();
  verbs_.push_back(PathVerb::Line);
  points_.push_back(Point::fromXY(x, y));
  return *this;
}

PathBuilder& PathBuilder::quadTo(float x1, float y1, float x, float y) {
  injectMoveToIfNeeded();
  verbs_.push_back(PathVerb::Quad);
  points_.push_back(Point::fromXY(x1, y1));
  points_.push_back(Point::fromXY(x, y));
  return *this;
}

PathBuilder& PathBuilder::quadToPt(Point p1, Point p) { return quadTo(p1.x, p1.y, p.x, p.y); }

PathBuilder& PathBuilder::cubicTo(float x1, float y1, float x2, float y2, float x, float y) {
  injectMoveToIfNeeded();
  verbs_.push_back(PathVerb::Cubic);
  points_.push_back(Point::fromXY(x1, y1));
  points_.push_back(Point::fromXY(x2, y2));
  points_.push_back(Point::fromXY(x, y));
  return *this;
}

PathBuilder& PathBuilder::cubicToPt(Point p1, Point p2, Point p) {
  return cubicTo(p1.x, p1.y, p2.x, p2.y, p.x, p.y);
}

PathBuilder& PathBuilder::conicTo(float x1, float y1, float x, float y, float weight) {
  if (!(weight > 0.0f)) {
    lineTo(x, y);
  } else if (!std::isfinite(weight)) {
    lineTo(x1, y1);
    lineTo(x, y);
  } else if (weight == 1.0f) {
    quadTo(x1, y1, x, y);
  } else {
    injectMoveToIfNeeded();
    auto last = lastPoint().value_or(Point::zero());
    auto quadder =
        pathGeometry::autoConicToQuads(last, Point::fromXY(x1, y1), Point::fromXY(x, y), weight);
    if (quadder.has_value()) {
      std::size_t offset = 1;
      for (std::uint8_t i = 0; i < quadder->len; ++i) {
        auto pt1 = quadder->points[offset + 0];
        auto pt2 = quadder->points[offset + 1];
        quadTo(pt1.x, pt1.y, pt2.x, pt2.y);
        offset += 2;
      }
    }
  }
  return *this;
}

PathBuilder& PathBuilder::conicPointsTo(Point pt1, Point pt2, float weight) {
  return conicTo(pt1.x, pt1.y, pt2.x, pt2.y, weight);
}

PathBuilder& PathBuilder::close() {
  if (!verbs_.empty() && verbs_.back() != PathVerb::Close) {
    verbs_.push_back(PathVerb::Close);
  }
  moveToRequired_ = true;
  return *this;
}

std::optional<Point> PathBuilder::lastPoint() const {
  if (points_.empty()) {
    return std::nullopt;
  }
  return points_.back();
}

void PathBuilder::setLastPoint(Point pt) {
  if (!points_.empty()) {
    points_.back() = pt;
  } else {
    moveTo(pt.x, pt.y);
  }
}

bool PathBuilder::isZeroLengthSincePoint(std::size_t startPtIndex) const {
  auto count = points_.size() - startPtIndex;
  if (count < 2) {
    return true;
  }
  auto first = points_[startPtIndex];
  for (std::size_t i = 1; i < count; ++i) {
    if (first != points_[startPtIndex + i]) {
      return false;
    }
  }
  return true;
}

PathBuilder& PathBuilder::pushRect(const Rect& rect) {
  moveTo(rect.left(), rect.top());
  lineTo(rect.right(), rect.top());
  lineTo(rect.right(), rect.bottom());
  lineTo(rect.left(), rect.bottom());
  close();
  return *this;
}

PathBuilder& PathBuilder::pushOval(const Rect& oval) {
  float cx = oval.left() * 0.5f + oval.right() * 0.5f;
  float cy = oval.top() * 0.5f + oval.bottom() * 0.5f;

  Point ovalPoints[4] = {
      Point::fromXY(cx, oval.bottom()),
      Point::fromXY(oval.left(), cy),
      Point::fromXY(cx, oval.top()),
      Point::fromXY(oval.right(), cy),
  };

  Point rectPoints[4] = {
      Point::fromXY(oval.right(), oval.bottom()),
      Point::fromXY(oval.left(), oval.bottom()),
      Point::fromXY(oval.left(), oval.top()),
      Point::fromXY(oval.right(), oval.top()),
  };

  float weight = kScalarRoot2Over2;
  moveTo(ovalPoints[3].x, ovalPoints[3].y);
  for (int i = 0; i < 4; ++i) {
    conicPointsTo(rectPoints[i], ovalPoints[i], weight);
  }
  close();
  return *this;
}

PathBuilder& PathBuilder::pushCircle(float x, float y, float r) {
  auto rect = Rect::fromLTRB(x - r, y - r, x + r, y + r);
  if (rect.has_value()) {
    pushOval(*rect);
  }
  return *this;
}

PathBuilder& PathBuilder::pushPath(const Path& other) {
  lastMoveToIndex_ = points_.size();
  auto otherVerbs = other.verbs();
  auto otherPoints = other.points();
  verbs_.insert(verbs_.end(), otherVerbs.begin(), otherVerbs.end());
  points_.insert(points_.end(), otherPoints.begin(), otherPoints.end());
  return *this;
}

PathBuilder& PathBuilder::pushPathBuilder(const PathBuilder& other) {
  if (other.empty()) {
    return *this;
  }
  if (lastMoveToIndex_ != 0) {
    lastMoveToIndex_ = points_.size() + other.lastMoveToIndex_;
  }
  verbs_.insert(verbs_.end(), other.verbs_.begin(), other.verbs_.end());
  points_.insert(points_.end(), other.points_.begin(), other.points_.end());
  return *this;
}

PathBuilder& PathBuilder::reversePathTo(const PathBuilder& other) {
  if (other.empty()) {
    return *this;
  }
  // verbs_[0] should be Move
  std::size_t pointsOffset = other.points_.size() - 1;
  for (auto it = other.verbs_.rbegin(); it != other.verbs_.rend(); ++it) {
    switch (*it) {
      case PathVerb::Move:
        return *this;  // stop at the first move
      case PathVerb::Line: {
        auto pt = other.points_[pointsOffset - 1];
        pointsOffset -= 1;
        lineTo(pt.x, pt.y);
        break;
      }
      case PathVerb::Quad: {
        auto pt1 = other.points_[pointsOffset - 1];
        auto pt2 = other.points_[pointsOffset - 2];
        pointsOffset -= 2;
        quadTo(pt1.x, pt1.y, pt2.x, pt2.y);
        break;
      }
      case PathVerb::Cubic: {
        auto pt1 = other.points_[pointsOffset - 1];
        auto pt2 = other.points_[pointsOffset - 2];
        auto pt3 = other.points_[pointsOffset - 3];
        pointsOffset -= 3;
        cubicTo(pt1.x, pt1.y, pt2.x, pt2.y, pt3.x, pt3.y);
        break;
      }
      case PathVerb::Close:
        break;
    }
  }
  return *this;
}

void PathBuilder::clear() {
  verbs_.clear();
  points_.clear();
  lastMoveToIndex_ = 0;
  moveToRequired_ = true;
}

std::optional<Path> PathBuilder::finish() {
  if (empty()) {
    return std::nullopt;
  }
  if (verbs_.size() == 1) {
    return std::nullopt;
  }

  // Construct the Path (which recomputes bounds).
  auto path = Path(std::move(verbs_), std::move(points_));

  // Reset builder state.
  verbs_ = {};
  points_ = {};
  lastMoveToIndex_ = 0;
  moveToRequired_ = true;

  return path;
}

std::optional<Path> Path::fromCircle(float cx, float cy, float r) {
  PathBuilder b;
  b.pushCircle(cx, cy, r);
  return b.finish();
}

}  // namespace tiny_skia
