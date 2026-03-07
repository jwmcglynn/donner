#pragma once

/// @file Path.h
/// @brief Immutable vector path and related types.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "tiny_skia/Edge.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Transform.h"

namespace tiny_skia {

/// Path segment verb (moveTo, lineTo, quadTo, cubicTo, close).
enum class PathVerb : std::uint8_t {
  Move,
  Close,
  Line,
  Quad,
  Cubic,
};

/// Line cap style for stroke endpoints.
enum class LineCap : std::uint8_t {
  Butt,   ///< Flat cap, no extension.
  Round,  ///< Semicircle cap.
  Square, ///< Extends by half the stroke width.
};

// Forward declarations for stroke/dash.
class PathBuilder;
struct Stroke;
struct StrokeDash;

/// @internal
/// A path segment for iteration.
struct PathSegment {
  enum class Kind : std::uint8_t { MoveTo, LineTo, QuadTo, CubicTo, Close };
  Kind kind;
  Point pts[3] = {};  // up to 3 points depending on kind
};

/// Immutable vector path — a sequence of lines, quadratics, and cubics.
///
/// Build with PathBuilder or use the static factories (fromRect, fromCircle).
/// Paths are immutable once constructed; use Path::clear() to recycle into a
/// new PathBuilder.
class Path {
 public:
  Path() = default;
  Path(std::vector<PathVerb> verbs, std::vector<Point> points)
      : verbs_(std::move(verbs)), points_(std::move(points)) {
    recomputeBounds();
  }

  /// Creates a rectangular path.
  static Path fromRect(const Rect& rect) {
    std::vector<PathVerb> verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line,
                                   PathVerb::Close};
    std::vector<Point> points = {Point{rect.left(), rect.top()}, Point{rect.right(), rect.top()},
                                 Point{rect.right(), rect.bottom()},
                                 Point{rect.left(), rect.bottom()}};
    return Path(std::move(verbs), std::move(points));
  }

  /// Creates a circular path. Returns nullopt for non-positive radius.
  [[nodiscard]] static std::optional<Path> fromCircle(float cx, float cy, float r);

  [[nodiscard]] std::size_t size() const { return verbs_.size(); }
  [[nodiscard]] bool empty() const { return verbs_.empty(); }

  [[nodiscard]] std::span<const PathVerb> verbs() const { return verbs_; }
  [[nodiscard]] std::span<const Point> points() const { return points_; }

  /// @internal
  void addVerb(PathVerb verb) { verbs_.push_back(verb); }

  /// @internal
  void addPoint(Point point) {
    if (bounds_.has_value()) {
      const auto current = bounds_.value();
      bounds_ =
          Rect::fromLTRB(std::min(current.left(), point.x), std::min(current.top(), point.y),
                         std::max(current.right(), point.x), std::max(current.bottom(), point.y));
    } else {
      bounds_ = Rect::fromLTRB(point.x, point.y, point.x, point.y);
    }
    points_.push_back(point);
  }

  /// @internal
  /// Returns true if the path is convex. Checks the control polygon: all cross products of
  /// consecutive edge vectors must have the same sign, AND no edges self-intersect.
  [[nodiscard]] bool isConvex() const {
    if (points_.size() < 3) return false;

    // Must be a single closed contour.
    int moveCount = 0;
    bool hasClosed = false;
    for (auto v : verbs_) {
      if (v == PathVerb::Move) moveCount++;
      if (v == PathVerb::Close) hasClosed = true;
    }
    if (moveCount != 1 || !hasClosed) return false;

    const auto n = points_.size();

    // Check cross products of consecutive direction vectors on the control polygon.
    int sign = 0;  // 0 = unknown, 1 = positive, -1 = negative
    for (std::size_t i = 0; i < n; i++) {
      const auto& p0 = points_[i];
      const auto& p1 = points_[(i + 1) % n];
      const auto& p2 = points_[(i + 2) % n];

      float dx1 = p1.x - p0.x;
      float dy1 = p1.y - p0.y;
      float dx2 = p2.x - p1.x;
      float dy2 = p2.y - p1.y;

      float cross = dx1 * dy2 - dy1 * dx2;
      if (cross > 0.0f) {
        if (sign < 0) return false;
        sign = 1;
      } else if (cross < 0.0f) {
        if (sign > 0) return false;
        sign = -1;
      }
    }
    if (sign == 0) return false;

    // Check no non-adjacent edges intersect (simple polygon check).
    // Stroked path outlines can be self-intersecting single contours that pass the
    // cross-product check above; this O(n²) check catches them.
    for (std::size_t i = 0; i < n; i++) {
      const auto& a = points_[i];
      const auto& b = points_[(i + 1) % n];
      for (std::size_t j = i + 2; j < n; j++) {
        if (i == 0 && j == n - 1) continue;  // Adjacent via wrap-around.
        const auto& c = points_[j];
        const auto& d = points_[(j + 1) % n];
        // Check proper intersection using orientation tests.
        float d1 = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        float d2 = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
        float d3 = (d.x - c.x) * (a.y - c.y) - (d.y - c.y) * (a.x - c.x);
        float d4 = (d.x - c.x) * (b.y - c.y) - (d.y - c.y) * (b.x - c.x);
        if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
            ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
          return false;
        }
      }
    }
    return true;
  }

  /// Axis-aligned bounding box (control-point bounds).
  [[nodiscard]] Rect bounds() const {
    return bounds_.value_or(Rect::fromLTRB(0.0f, 0.0f, 0.0f, 0.0f).value());
  }

  /// Returns a transformed copy. Nullopt if the transform produces non-finite values.
  [[nodiscard]] std::optional<Path> transform(const Transform& ts) const {
    auto pts = points_;
    ts.mapPoints(pts);
    for (const auto& p : pts) {
      if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
        return std::nullopt;
      }
    }
    return Path(verbs_, std::move(pts));
  }

  /// Computes tight bounds by finding curve extrema (more precise than bounds()).
  [[nodiscard]] std::optional<Rect> computeTightBounds() const;

  /// Clears the path and returns a PathBuilder reusing the allocations.
  [[nodiscard]] PathBuilder clear();

  /// Generates a filled path representing the stroke outline.
  [[nodiscard]] std::optional<Path> stroke(const Stroke& stroke, float resScale) const;

  /// Applies a dash pattern, returning a new dashed path.
  [[nodiscard]] std::optional<Path> dash(const StrokeDash& dash, float resScale) const;

 private:
  void recomputeBounds() {
    if (points_.empty()) {
      bounds_.reset();
      return;
    }

    auto left = points_[0].x;
    auto top = points_[0].y;
    auto right = points_[0].x;
    auto bottom = points_[0].y;

    for (const auto& point : points_) {
      left = std::min(left, point.x);
      right = std::max(right, point.x);
      top = std::min(top, point.y);
      bottom = std::max(bottom, point.y);
    }

    bounds_ = Rect::fromLTRB(left, top, right, bottom);
  }

  std::vector<PathVerb> verbs_;
  std::vector<Point> points_;
  std::optional<Rect> bounds_;
};

/// Fill rule for path filling.
enum class FillRule : std::uint8_t {
  Winding = 0,  ///< Non-zero winding rule.
  EvenOdd = 1,  ///< Even-odd (parity) rule.
};

/// @internal
/// Path segments iterator.
class PathSegmentsIter {
 public:
  explicit PathSegmentsIter(const Path& path) : path_(&path) {}

  void setAutoClose(bool flag) { isAutoClose_ = flag; }

  std::optional<PathSegment> next();

  [[nodiscard]] Point lastPoint() const { return lastPoint_; }
  [[nodiscard]] Point lastMoveTo() const { return lastMoveTo_; }

  [[nodiscard]] PathVerb currVerb() const { return path_->verbs()[verbIndex_ - 1]; }

  [[nodiscard]] std::optional<PathVerb> nextVerb() const {
    if (verbIndex_ < path_->verbs().size()) {
      return path_->verbs()[verbIndex_];
    }
    return std::nullopt;
  }

  [[nodiscard]] bool hasValidTangent() const;

 private:
  PathSegment autoClose();

  const Path* path_;
  std::size_t verbIndex_ = 0;
  std::size_t pointsIndex_ = 0;
  bool isAutoClose_ = false;
  Point lastMoveTo_ = Point::zero();
  Point lastPoint_ = Point::zero();
};

}  // namespace tiny_skia
