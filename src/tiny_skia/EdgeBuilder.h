#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "tiny_skia/Edge.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Path.h"

namespace tiny_skia {

struct ShiftedIntRect {
  ScreenIntRect shiftedRect;
  std::int32_t shift = 0;

  [[nodiscard]] static std::optional<ShiftedIntRect> create(ScreenIntRect rect, std::int32_t shift);

  [[nodiscard]] const ScreenIntRect& shifted() const;
  [[nodiscard]] ScreenIntRect recover() const;
};

class BasicEdgeBuilder {
 public:
  static BasicEdgeBuilder newBuilder(std::int32_t clipShift);

  static std::optional<std::vector<Edge>> buildEdges(const Path& path, const ShiftedIntRect* clip,
                                                     std::int32_t clipShift);

  bool build(const Path& path, const ShiftedIntRect* clip, bool canCullToTheRight);

  [[nodiscard]] std::size_t edgesCount() const;
  void clearEdges();
  [[nodiscard]] std::span<const Edge> edges() const;

 private:
  void pushLine(std::span<const Point, 2> points);
  void pushQuad(std::span<const Point> points);
  void pushCubic(std::span<const Point> points);

  std::vector<Edge> edges_;
  std::int32_t clipShift_ = 0;

  enum class Combine {
    None,
    Partial,
    Total,
  };

  [[nodiscard]] static Combine combineVertical(const LineEdge& edge, LineEdge& last);
};

enum class PathEdgeType {
  LineTo,
  QuadTo,
  CubicTo,
};

struct PathEdge {
  PathEdgeType type = PathEdgeType::LineTo;
  std::array<Point, 4> points{};
};

class PathEdgeIter {
 public:
  explicit PathEdgeIter(const Path& path);
  [[nodiscard]] std::optional<PathEdge> next();

 private:
  const Path* path_ = nullptr;
  std::size_t verbIndex_ = 0;
  std::size_t pointsIndex_ = 0;
  Point moveTo_{};
  bool needsCloseLine_ = false;

  [[nodiscard]] std::optional<PathEdge> closeLine();
};

[[nodiscard]] PathEdgeIter pathIter(const Path& path);

}  // namespace tiny_skia
