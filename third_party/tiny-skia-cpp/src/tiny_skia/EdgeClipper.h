#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>

#include "tiny_skia/EdgeBuilder.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Path.h"

namespace tiny_skia {

constexpr std::size_t kMaxClippedEdges = 18;

class ClippedEdges {
 public:
  using Container = std::array<PathEdge, kMaxClippedEdges>;

  using const_iterator = Container::const_iterator;

  [[nodiscard]] bool empty() const { return size_ == 0; }

  [[nodiscard]] std::size_t size() const { return size_; }

  [[nodiscard]] const_iterator begin() const { return edges_.cbegin(); }

  [[nodiscard]] const_iterator end() const {
    return edges_.cbegin() + static_cast<std::ptrdiff_t>(size_);
  }

  [[nodiscard]] std::span<const PathEdge> span() const {
    return std::span<const PathEdge>{edges_.data(), size_};
  }

  [[nodiscard]] bool pushLine(Point p0, Point p1) {
    if (size_ >= kMaxClippedEdges) {
      return false;
    }
    auto& edge = edges_[size_];
    ++size_;
    edge.type = PathEdgeType::LineTo;
    edge.points[0] = p0;
    edge.points[1] = p1;
    return true;
  }

  [[nodiscard]] bool pushQuad(std::array<Point, 3> pts, bool reverse) {
    if (size_ >= kMaxClippedEdges) {
      return false;
    }
    auto& edge = edges_[size_];
    ++size_;
    edge.type = PathEdgeType::QuadTo;
    edge.points[0] = reverse ? pts[2] : pts[0];
    edge.points[1] = pts[1];
    edge.points[2] = reverse ? pts[0] : pts[2];
    return true;
  }

  [[nodiscard]] bool pushCubic(std::array<Point, 4> pts, bool reverse) {
    if (size_ >= kMaxClippedEdges) {
      return false;
    }
    auto& edge = edges_[size_];
    ++size_;
    edge.type = PathEdgeType::CubicTo;
    edge.points[0] = reverse ? pts[3] : pts[0];
    edge.points[1] = reverse ? pts[2] : pts[1];
    edge.points[2] = reverse ? pts[1] : pts[2];
    edge.points[3] = reverse ? pts[0] : pts[3];
    return true;
  }

 private:
  Container edges_{};
  std::size_t size_ = 0;
};

class EdgeClipper {
 public:
  explicit EdgeClipper(Rect clip, bool canCullToTheRight);

  [[nodiscard]] std::optional<ClippedEdges> clipLine(Point p0, Point p1);

 private:
  [[nodiscard]] std::optional<ClippedEdges> clipQuad(Point p0, Point p1, Point p2);
  [[nodiscard]] std::optional<ClippedEdges> clipCubic(Point p0, Point p1, Point p2, Point p3);

  void pushLine(Point p0, Point p1);
  void pushVerticalLine(float x, float y0, float y1, bool reverse);
  void clipMonoQuad(const std::array<Point, 3>& src);
  void clipMonoCubic(const std::array<Point, 4>& src);
  void pushQuad(const std::array<Point, 3>& pts, bool reverse);
  void pushCubic(const std::array<Point, 4>& pts, bool reverse);

  Rect clip_;
  bool canCullToTheRight_ = false;
  ClippedEdges edges_;

  friend class EdgeClipperIter;
};

class EdgeClipperIter {
 public:
  EdgeClipperIter(const Path& path, Rect clip, bool canCullToTheRight);

  [[nodiscard]] std::optional<ClippedEdges> next();

 private:
  PathEdgeIter edgeIter_;
  Rect clip_;
  bool canCullToTheRight_ = false;
};

}  // namespace tiny_skia
