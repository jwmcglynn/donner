#include "tiny_skia/Edge.h"

#include <bit>
#include <cassert>

namespace tiny_skia {

namespace {

constexpr int kMaxCoeffShift = 6;

FDot6 computeDy(FDot6 top, FDot6 y0) { return leftShift(top, 6) + 32 - y0; }

FDot16 fdot6ToFixedDiv2(FDot6 value) { return leftShift(value, 9); }

FDot16 fdot6UpShift(FDot6 x, int32_t upShift) {
  assert((leftShift(x, upShift) >> upShift) == x);
  return leftShift(x, upShift);
}

FDot16 cheapDistance(FDot6 dx, FDot6 dy) {
  dx = std::abs(dx);
  dy = std::abs(dy);
  return dx > dy ? dx + (dy >> 1) : dy + (dx >> 1);
}

int32_t diffToShift(FDot6 dx, FDot6 dy, int32_t shiftAA) {
  auto dist = cheapDistance(dx, dy);
  dist = (dist + (1 << (2 + shiftAA))) >> (3 + shiftAA);

  if (dist == 0) {
    return 0;
  }
  return (32 - std::countl_zero(static_cast<uint32_t>(dist))) >> 1;
}

FDot6 cubicDeltaFromLine(FDot6 a, FDot6 b, FDot6 c, FDot6 d) {
  const auto oneThird = ((a * 8 - b * 15 + c * 6 + d) * 19) >> 9;
  const auto twoThird = ((a + b * 6 - c * 15 + d * 8) * 19) >> 9;
  return std::max(std::abs(oneThird), std::abs(twoThird));
}

std::optional<QuadraticEdge> makeQuadraticEdge(std::span<const Point> points, std::int32_t shift) {
  if (points.size() != 3) {
    return std::nullopt;
  }

  auto scale = static_cast<float>(1 << (shift + 6));
  auto x0 = static_cast<std::int32_t>(points[0].x * scale);
  auto y0 = static_cast<std::int32_t>(points[0].y * scale);
  const auto x1 = static_cast<std::int32_t>(points[1].x * scale);
  const auto y1 = static_cast<std::int32_t>(points[1].y * scale);
  auto x2 = static_cast<std::int32_t>(points[2].x * scale);
  auto y2 = static_cast<std::int32_t>(points[2].y * scale);

  auto winding = std::int8_t{1};
  if (y0 > y2) {
    std::swap(x0, x2);
    std::swap(y0, y2);
    winding = -1;
  }

  assert(y0 <= y1 && y1 <= y2);

  const auto top = fdot6::round(y0);
  const auto bottom = fdot6::round(y2);
  if (top == bottom) {
    return std::nullopt;
  }

  auto dx = (leftShift(x1, 1) - x0 - x2) >> 2;
  auto dy = (leftShift(y1, 1) - y0 - y2) >> 2;
  shift = diffToShift(dx, dy, shift);
  assert(shift >= 0);

  if (shift == 0) {
    shift = 1;
  } else if (shift > kMaxCoeffShift) {
    shift = kMaxCoeffShift;
  }

  const auto curveCount = static_cast<int8_t>(1 << shift);
  const auto curveShift = static_cast<uint8_t>(shift - 1);
  const auto a = fdot6ToFixedDiv2(x0 - x1 - x1 + x2);
  const auto b = fdot6::toFdot16(x1 - x0);

  const auto qx = fdot6::toFdot16(x0);
  const auto qdx = b + (a >> shift);
  const auto qddx = a >> (shift - 1);

  const auto aY = fdot6ToFixedDiv2(y0 - y1 - y1 + y2);
  const auto bY = fdot6::toFdot16(y1 - y0);
  const auto qy = fdot6::toFdot16(y0);
  const auto qdy = bY + (aY >> shift);
  const auto qddy = aY >> (shift - 1);

  const auto qLastX = fdot6::toFdot16(x2);
  const auto qLastY = fdot6::toFdot16(y2);

  return QuadraticEdge{.line =
                           {
                               .prev = std::nullopt,
                               .next = std::nullopt,
                               .x = 0,
                               .dx = 0,
                               .firstY = 0,
                               .lastY = 0,
                               .winding = winding,
                           },
                       .curveCount = curveCount,
                       .curveShift = curveShift,
                       .qx = qx,
                       .qy = qy,
                       .qdx = qdx,
                       .qdy = qdy,
                       .qddx = qddx,
                       .qddy = qddy,
                       .qLastX = qLastX,
                       .qLastY = qLastY};
}

std::optional<CubicEdge> makeCubicEdge(std::span<const Point> points, std::int32_t shift,
                                       bool sortY) {
  if (points.size() != 4) {
    return std::nullopt;
  }

  auto scale = static_cast<float>(1 << (shift + 6));
  auto x0 = static_cast<std::int32_t>(points[0].x * scale);
  auto y0 = static_cast<std::int32_t>(points[0].y * scale);
  auto x1 = static_cast<std::int32_t>(points[1].x * scale);
  auto y1 = static_cast<std::int32_t>(points[1].y * scale);
  auto x2 = static_cast<std::int32_t>(points[2].x * scale);
  auto y2 = static_cast<std::int32_t>(points[2].y * scale);
  auto x3 = static_cast<std::int32_t>(points[3].x * scale);
  auto y3 = static_cast<std::int32_t>(points[3].y * scale);

  auto winding = std::int8_t{1};
  if (sortY && y0 > y3) {
    std::swap(x0, x3);
    std::swap(x1, x2);
    std::swap(y0, y3);
    std::swap(y1, y2);
    winding = -1;
  }

  const auto top = fdot6::round(y0);
  const auto bottom = fdot6::round(y3);
  if (sortY && top == bottom) {
    return std::nullopt;
  }

  {
    const auto dx = cubicDeltaFromLine(x0, x1, x2, x3);
    const auto dy = cubicDeltaFromLine(y0, y1, y2, y3);
    shift = diffToShift(dx, dy, 2) + 1;
  }

  assert(shift > 0);
  if (shift > kMaxCoeffShift) {
    shift = kMaxCoeffShift;
  }

  auto upShift = 6;
  auto downShift = shift + upShift - 10;
  if (downShift < 0) {
    downShift = 0;
    upShift = 10 - shift;
  }

  const auto curveCount = leftShift(-1, shift);
  const auto curveShift = static_cast<uint8_t>(shift);
  const auto dshift = static_cast<uint8_t>(downShift);

  const auto b = fdot6UpShift(3 * (x1 - x0), upShift);
  const auto c = fdot6UpShift(3 * (x0 - x1 - x1 + x2), upShift);
  const auto d = fdot6UpShift(x3 + 3 * (x1 - x2) - x0, upShift);
  const auto cx = fdot6::toFdot16(x0);
  const auto cdx = b + (c >> shift) + (d >> (2 * shift));
  const auto cddx = 2 * c + ((3 * d) >> (shift - 1));
  const auto cdddx = (3 * d) >> (shift - 1);

  const auto bY = fdot6UpShift(3 * (y1 - y0), upShift);
  const auto cY = fdot6UpShift(3 * (y0 - y1 - y1 + y2), upShift);
  const auto dY = fdot6UpShift(y3 + 3 * (y1 - y2) - y0, upShift);
  const auto cy = fdot6::toFdot16(y0);
  const auto cdy = bY + (cY >> shift) + (dY >> (2 * shift));
  const auto cddy = 2 * cY + ((3 * dY) >> (shift - 1));
  const auto cdddy = (3 * dY) >> (shift - 1);

  const auto cLastX = fdot6::toFdot16(x3);
  const auto cLastY = fdot6::toFdot16(y3);

  return CubicEdge{.line =
                       {
                           .prev = std::nullopt,
                           .next = std::nullopt,
                           .x = 0,
                           .dx = 0,
                           .firstY = 0,
                           .lastY = 0,
                           .winding = winding,
                       },
                   .curveCount = static_cast<int8_t>(curveCount),
                   .curveShift = curveShift,
                   .dshift = dshift,
                   .cx = cx,
                   .cy = cy,
                   .cdx = cdx,
                   .cdy = cdy,
                   .cddx = cddx,
                   .cddy = cddy,
                   .cdddx = cdddx,
                   .cdddy = cdddy,
                   .cLastX = cLastX,
                   .cLastY = cLastY};
}

}  // namespace

Edge::Edge(const LineEdge& line) : asVariant_(line) {}
Edge::Edge(const QuadraticEdge& quad) : asVariant_(quad) {}
Edge::Edge(const CubicEdge& cubic) : asVariant_(cubic) {}

bool Edge::isLine() const { return std::holds_alternative<LineEdge>(asVariant_); }

bool Edge::isQuadratic() const { return std::holds_alternative<QuadraticEdge>(asVariant_); }

bool Edge::isCubic() const { return std::holds_alternative<CubicEdge>(asVariant_); }

const LineEdge& Edge::asLine() const {
  if (std::holds_alternative<LineEdge>(asVariant_)) {
    return std::get<LineEdge>(asVariant_);
  }
  if (std::holds_alternative<QuadraticEdge>(asVariant_)) {
    return std::get<QuadraticEdge>(asVariant_).line;
  }
  return std::get<CubicEdge>(asVariant_).line;
}

LineEdge& Edge::asLine() {
  if (std::holds_alternative<LineEdge>(asVariant_)) {
    return std::get<LineEdge>(asVariant_);
  }
  if (std::holds_alternative<QuadraticEdge>(asVariant_)) {
    return std::get<QuadraticEdge>(asVariant_).line;
  }
  return std::get<CubicEdge>(asVariant_).line;
}

const QuadraticEdge& Edge::asQuadratic() const { return std::get<QuadraticEdge>(asVariant_); }

QuadraticEdge& Edge::asQuadratic() { return std::get<QuadraticEdge>(asVariant_); }

const CubicEdge& Edge::asCubic() const { return std::get<CubicEdge>(asVariant_); }

CubicEdge& Edge::asCubic() { return std::get<CubicEdge>(asVariant_); }

std::optional<LineEdge> LineEdge::create(Point p0, Point p1, std::int32_t shift) {
  const auto scaleShift = shift + 6;
  if (scaleShift < 0 || scaleShift >= 31) {
    return std::nullopt;
  }

  const auto scale = static_cast<float>(1 << scaleShift);
  auto x0 = static_cast<std::int32_t>(p0.x * scale);
  auto y0 = static_cast<std::int32_t>(p0.y * scale);
  auto x1 = static_cast<std::int32_t>(p1.x * scale);
  auto y1 = static_cast<std::int32_t>(p1.y * scale);
  auto winding = std::int8_t{1};

  if (y0 > y1) {
    std::swap(x0, x1);
    std::swap(y0, y1);
    winding = -1;
  }

  const auto top = fdot6::round(y0);
  const auto bottom = fdot6::round(y1);
  if (top == bottom) {
    return std::nullopt;
  }

  const auto slope = fdot6::div(x1 - x0, y1 - y0);
  const auto dy = computeDy(top, y0);

  return LineEdge{.prev = std::nullopt,
                  .next = std::nullopt,
                  .x = fdot6::toFdot16(x0 + fdot16::mul(slope, dy)),
                  .dx = slope,
                  .firstY = top,
                  .lastY = bottom - 1,
                  .winding = winding};
}

bool LineEdge::isVertical() const { return dx == 0; }

bool LineEdge::update(FDot16 x0, FDot16 y0, FDot16 x1, FDot16 y1) {
  assert(winding == 1 || winding == -1);

  y0 >>= 10;
  y1 >>= 10;
  assert(y0 <= y1);

  const auto top = fdot6::round(y0);
  const auto bottom = fdot6::round(y1);
  if (top == bottom) {
    return false;
  }

  x0 >>= 10;
  x1 >>= 10;
  const auto slope = fdot6::div(x1 - x0, y1 - y0);
  const auto dy = computeDy(top, y0);
  x = fdot6::toFdot16(x0 + fdot16::mul(slope, dy));
  dx = slope;
  firstY = top;
  lastY = bottom - 1;

  return true;
}

std::optional<QuadraticEdge> QuadraticEdge::create(std::span<const Point> points,
                                                   std::int32_t shift) {
  auto edgeOpt = makeQuadraticEdge(points, shift);
  if (!edgeOpt) {
    return std::nullopt;
  }
  auto edge = std::move(edgeOpt.value());
  if (edge.update()) {
    return edge;
  }
  return std::nullopt;
}

bool QuadraticEdge::update() {
  const auto shift = curveShift;
  auto count = curveCount;
  auto oldX = qx;
  auto oldY = qy;
  auto dx = qdx;
  auto dy = qdy;
  FDot16 newX;
  FDot16 newY;

  bool success;
  while (true) {
    count -= 1;
    if (count > 0) {
      newX = oldX + (dx >> shift);
      dx += qddx;
      newY = oldY + (dy >> shift);
      dy += qddy;
    } else {
      newX = qLastX;
      newY = qLastY;
    }

    success = line.update(oldX, oldY, newX, newY);
    oldX = newX;
    oldY = newY;

    if (count == 0 || success) {
      break;
    }
  }

  qx = newX;
  qy = newY;
  qdx = dx;
  qdy = dy;
  curveCount = count;
  return success;
}

std::optional<CubicEdge> CubicEdge::create(std::span<const Point> points, std::int32_t shift) {
  auto edgeOpt = makeCubicEdge(points, shift, true);
  if (!edgeOpt) {
    return std::nullopt;
  }
  auto edge = std::move(edgeOpt.value());
  if (edge.update()) {
    return edge;
  }
  return std::nullopt;
}

bool CubicEdge::update() {
  auto count = curveCount;
  auto oldX = cx;
  auto oldY = cy;
  const auto ddshift = curveShift;
  const auto shift = dshift;
  bool success;
  FDot16 newX;
  FDot16 newY;

  assert(count < 0);
  while (true) {
    count += 1;
    if (count < 0) {
      newX = oldX + (cdx >> shift);
      cdx += cddx >> ddshift;
      cddx += cdddx;
      newY = oldY + (cdy >> shift);
      cdy += cddy >> ddshift;
      cddy += cdddy;
    } else {
      newX = cLastX;
      newY = cLastY;
    }
    if (newY < oldY) {
      newY = oldY;
    }

    success = line.update(oldX, oldY, newX, newY);
    oldX = newX;
    oldY = newY;

    if (count == 0 || success) {
      break;
    }
  }

  cx = newX;
  cy = newY;
  curveCount = count;
  return success;
}

}  // namespace tiny_skia
