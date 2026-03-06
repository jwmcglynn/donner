#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "tiny_skia/FixedPoint.h"
#include "tiny_skia/Math.h"
#include "tiny_skia/Point.h"

namespace tiny_skia {

class LineEdge {
 public:
  std::optional<std::uint32_t> prev;
  std::optional<std::uint32_t> next;

  FDot16 x = 0;
  FDot16 dx = 0;
  std::int32_t firstY = 0;
  std::int32_t lastY = 0;
  std::int8_t winding = 0;

  static std::optional<LineEdge> create(Point p0, Point p1, std::int32_t shift);
  [[nodiscard]] bool isVertical() const;

  friend class QuadraticEdge;
  friend class CubicEdge;

 private:
  bool update(FDot16 x0, FDot16 y0, FDot16 x1, FDot16 y1);
};

class QuadraticEdge {
 public:
  LineEdge line;
  std::int8_t curveCount = 0;
  std::uint8_t curveShift = 0;
  FDot16 qx = 0;
  FDot16 qy = 0;
  FDot16 qdx = 0;
  FDot16 qdy = 0;
  FDot16 qddx = 0;
  FDot16 qddy = 0;
  FDot16 qLastX = 0;
  FDot16 qLastY = 0;

  static std::optional<QuadraticEdge> create(std::span<const Point> points, std::int32_t shift);
  [[nodiscard]] bool update();
};

class CubicEdge {
 public:
  LineEdge line;
  std::int8_t curveCount = 0;
  std::uint8_t curveShift = 0;
  std::uint8_t dshift = 0;
  FDot16 cx = 0;
  FDot16 cy = 0;
  FDot16 cdx = 0;
  FDot16 cdy = 0;
  FDot16 cddx = 0;
  FDot16 cddy = 0;
  FDot16 cdddx = 0;
  FDot16 cdddy = 0;
  FDot16 cLastX = 0;
  FDot16 cLastY = 0;

  static std::optional<CubicEdge> create(std::span<const Point> points, std::int32_t shift);
  [[nodiscard]] bool update();
};

class Edge {
 public:
  Edge() = delete;
  explicit Edge(const LineEdge& line);
  explicit Edge(const QuadraticEdge& quad);
  explicit Edge(const CubicEdge& cubic);

  [[nodiscard]] bool isLine() const;
  [[nodiscard]] bool isQuadratic() const;
  [[nodiscard]] bool isCubic() const;
  [[nodiscard]] const LineEdge& asLine() const;
  [[nodiscard]] LineEdge& asLine();
  [[nodiscard]] const QuadraticEdge& asQuadratic() const;
  [[nodiscard]] QuadraticEdge& asQuadratic();
  [[nodiscard]] const CubicEdge& asCubic() const;
  [[nodiscard]] CubicEdge& asCubic();

 private:
  std::variant<LineEdge, QuadraticEdge, CubicEdge> asVariant_;
};

}  // namespace tiny_skia
