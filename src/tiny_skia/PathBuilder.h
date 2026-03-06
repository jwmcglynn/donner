#pragma once

/// @file PathBuilder.h
/// @brief Builder for constructing immutable Path objects.

#include <cstddef>
#include <optional>
#include <vector>

#include "tiny_skia/Path.h"
#include "tiny_skia/PathGeometry.h"
#include "tiny_skia/Scalar.h"

namespace tiny_skia {

/// Incrementally builds a Path from move/line/quad/cubic/close operations.
///
/// @par Example
/// @snippet fill.cpp fill_example
class PathBuilder {
 public:
  PathBuilder() = default;

  /// Pre-allocates capacity for verbs and points.
  PathBuilder(std::size_t verbsCapacity, std::size_t pointsCapacity) {
    verbs_.reserve(verbsCapacity);
    points_.reserve(pointsCapacity);
  }

  /// Reserves additional capacity.
  void reserve(std::size_t additionalVerbs, std::size_t additionalPoints) {
    verbs_.reserve(verbs_.size() + additionalVerbs);
    points_.reserve(points_.size() + additionalPoints);
  }

  [[nodiscard]] std::size_t size() const { return verbs_.size(); }
  [[nodiscard]] bool empty() const { return verbs_.empty(); }

  /// Begins a new sub-path at (x, y).
  PathBuilder& moveTo(float x, float y);
  /// Adds a line segment to (x, y).
  PathBuilder& lineTo(float x, float y);
  /// Adds a quadratic Bezier to (x, y) with control point (x1, y1).
  PathBuilder& quadTo(float x1, float y1, float x, float y);
  /// Adds a quadratic Bezier using Point arguments.
  PathBuilder& quadToPt(Point p1, Point p);
  /// Adds a cubic Bezier to (x, y) with control points (x1,y1) and (x2,y2).
  PathBuilder& cubicTo(float x1, float y1, float x2, float y2, float x, float y);
  /// Adds a cubic Bezier using Point arguments.
  PathBuilder& cubicToPt(Point p1, Point p2, Point p);
  /// Adds a conic (rational quadratic) segment, approximated as quadratics.
  PathBuilder& conicTo(float x1, float y1, float x, float y, float weight);
  /// Adds a conic segment using Point arguments.
  PathBuilder& conicPointsTo(Point pt1, Point pt2, float weight);
  /// Closes the current sub-path.
  PathBuilder& close();

  [[nodiscard]] std::optional<Point> lastPoint() const;
  void setLastPoint(Point pt);

  /// @internal
  [[nodiscard]] bool isZeroLengthSincePoint(std::size_t startPtIndex) const;

  /// Creates a Path from a circle. Returns nullopt for non-positive radius.
  [[nodiscard]] static std::optional<Path> fromCircle(float cx, float cy, float radius) {
    PathBuilder b;
    b.pushCircle(cx, cy, radius);
    return b.finish();
  }

  /// Appends a closed rectangle sub-path.
  PathBuilder& pushRect(const Rect& rect);
  /// Appends a closed oval sub-path inscribed in the given rect.
  PathBuilder& pushOval(const Rect& oval);
  /// Appends a closed circle sub-path.
  PathBuilder& pushCircle(float x, float y, float r);
  /// Appends all segments from another path.
  PathBuilder& pushPath(const Path& other);
  /// Appends all segments from another builder.
  PathBuilder& pushPathBuilder(const PathBuilder& other);
  /// @internal
  PathBuilder& reversePathTo(const PathBuilder& other);

  /// Resets the builder, discarding all segments.
  void clear();

  /// Builds and returns the immutable Path. Returns nullopt if empty or invalid.
  [[nodiscard]] std::optional<Path> finish();

  /// @internal
  [[nodiscard]] const std::vector<Point>& points() const { return points_; }
  /// @internal
  [[nodiscard]] const std::vector<PathVerb>& verbs() const { return verbs_; }

 private:
  void injectMoveToIfNeeded();

  std::vector<PathVerb> verbs_;
  std::vector<Point> points_;
  std::size_t lastMoveToIndex_ = 0;
  bool moveToRequired_ = true;
};

}  // namespace tiny_skia
