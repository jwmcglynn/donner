#include "tiny_skia/scan/Hairline.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "tiny_skia/FixedPoint.h"
#include "tiny_skia/PathGeometry.h"

namespace tiny_skia {

constexpr std::uint8_t kMaxCubicSubdivideLevel = 9;
constexpr std::uint8_t kMaxQuadSubdivideLevel = 5;
constexpr float kFloatPi = 3.14159265f;
constexpr std::int32_t kMaxCoord = 32767;
constexpr std::int32_t kHalfPixelMask = 0x3f;

Point addPoints(const Point& left, const Point& right) {
  return {left.x + right.x, left.y + right.y};
}

Point subtract(const Point& left, const Point& right) {
  return {left.x - right.x, left.y - right.y};
}

bool isZero(const Point& point) { return point.x == 0.0f && point.y == 0.0f; }

void normalize(Point& point) {
  const auto distance = std::sqrt(point.x * point.x + point.y * point.y);
  point.x /= distance;
  point.y /= distance;
}

template <std::size_t N>
void extendPts(LineCap lineCap, PathVerb prevVerb, const std::optional<PathVerb>& nextVerb,
               std::array<Point, N>& points) {
  const auto capOutset = lineCap == LineCap::Square ? 0.5f : kFloatPi / 8.0f;

  if (prevVerb == PathVerb::Move) {
    const auto first = points[0];
    std::size_t offset = 0;
    std::size_t controls = N - 1;
    Point tangent{};

    do {
      ++offset;
      tangent = subtract(first, points[offset]);
      if (!isZero(tangent)) {
        break;
      }
      if (controls > 0) {
        --controls;
      }
    } while (controls > 0);

    if (isZero(tangent)) {
      tangent = {1.0f, 0.0f};
      controls = N - 1;
    } else {
      normalize(tangent);
    }

    offset = 0;
    do {
      points[offset] = addPoints(points[offset], {tangent.x * capOutset, tangent.y * capOutset});
      ++offset;
      ++controls;
    } while (controls < N);
  }

  if (!nextVerb.has_value() || *nextVerb == PathVerb::Move || *nextVerb == PathVerb::Close) {
    const auto last = points[N - 1];
    std::size_t offset = N - 1;
    std::size_t controls = N - 1;
    Point tangent{};

    do {
      --offset;
      tangent = subtract(last, points[offset]);
      if (!isZero(tangent)) {
        break;
      }
      if (controls > 0) {
        --controls;
      }
    } while (controls > 0);

    if (isZero(tangent)) {
      tangent = {-1.0f, 0.0f};
      controls = N - 1;
    } else {
      normalize(tangent);
    }

    offset = N - 1;
    while (true) {
      points[offset] = addPoints(points[offset], {tangent.x * capOutset, tangent.y * capOutset});
      ++controls;
      if (controls >= N) {
        break;
      }
      --offset;
    }
  }
}

bool isIntRectContaining(const IntRect& outer, const IntRect& inner) {
  return outer.left() <= inner.left() && outer.top() <= inner.top() &&
         outer.right() >= inner.right() && outer.bottom() >= inner.bottom();
}

std::optional<IntRect> makeInset(const IntRect& rect, std::uint32_t insetX, std::uint32_t insetY) {
  if (insetX > rect.width() || insetY > rect.height()) {
    return std::nullopt;
  }
  return IntRect::fromXYWH(rect.x() + static_cast<std::int32_t>(insetX),
                           rect.y() + static_cast<std::int32_t>(insetY), rect.width() - 2u * insetX,
                           rect.height() - 2u * insetY);
}

std::optional<IntRect> makeOutset(const IntRect& rect, std::uint32_t outsetX,
                                  std::uint32_t outsetY) {
  return IntRect::fromXYWH(rect.x() - static_cast<std::int32_t>(outsetX),
                           rect.y() - static_cast<std::int32_t>(outsetY),
                           rect.width() + 2u * outsetX, rect.height() + 2u * outsetY);
}

std::optional<Rect> makeOutset(const Rect& rect, float outsetX, float outsetY) {
  return Rect::fromLTRB(rect.left() - outsetX, rect.top() - outsetY, rect.right() + outsetX,
                        rect.bottom() + outsetY);
}

bool geometricOverlap(const Rect& left, const Rect& right) {
  return left.left() < right.right() && right.left() < left.right() &&
         left.top() < right.bottom() && right.top() < left.bottom();
}

bool geometricContains(const Rect& outer, const Rect& inner) {
  return inner.right() <= outer.right() && inner.left() >= outer.left() &&
         inner.bottom() <= outer.bottom() && inner.top() >= outer.top();
}

std::uint32_t saturatingCeilToU32(float value) {
  if (!std::isfinite(value)) {
    return value > 0.0f ? std::numeric_limits<std::uint32_t>::max() : 0u;
  }
  if (value >= static_cast<float>(std::numeric_limits<std::uint32_t>::max())) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  if (value <= 0.0f) {
    return 0u;
  }
  return static_cast<std::uint32_t>(std::ceil(value));
}

std::uint32_t computeIntQuadDist(const std::array<Point, 3>& points) {
  const auto dx = std::fabs((points[0].x + points[2].x) * 0.5f - points[1].x);
  const auto dy = std::fabs((points[0].y + points[2].y) * 0.5f - points[1].y);

  const auto idx = saturatingCeilToU32(dx);
  const auto idy = saturatingCeilToU32(dy);
  return (dx > dy) ? (idx + (idy >> 1)) : (idy + (idx >> 1));
}

std::uint32_t computeQuadLevel(const std::array<Point, 3>& points) {
  const auto dist = computeIntQuadDist(points);
  const auto leading = std::countl_zero(dist);
  auto level = (33u - leading) >> 1;
  if (level > kMaxQuadSubdivideLevel) {
    level = kMaxQuadSubdivideLevel;
  }
  return level;
}

std::uint32_t computeCubicSegments(const std::array<Point, 4>& points) {
  const auto oneThird = 1.0f / 3.0f;
  const auto twoThird = 2.0f / 3.0f;

  const auto p13 = Point{oneThird * points[3].x + twoThird * points[0].x,
                         oneThird * points[3].y + twoThird * points[0].y};
  const auto p23 = Point{oneThird * points[0].x + twoThird * points[3].x,
                         oneThird * points[0].y + twoThird * points[3].y};

  const auto diff =
      std::max(std::max(std::fabs(points[1].x - p13.x), std::fabs(points[1].y - p13.y)),
               std::max(std::fabs(points[2].x - p23.x), std::fabs(points[2].y - p23.y)));

  float tol = 1.0f / 8.0f;
  for (std::uint32_t i = 0; i < kMaxCubicSubdivideLevel; ++i) {
    if (diff < tol) {
      return 1u << i;
    }
    tol *= 4.0f;
  }
  return 1u << kMaxCubicSubdivideLevel;
}

// Quad coefficient representation.
struct QuadCoeffLocal {
  float ax, ay, bx, by, cx, cy;

  static QuadCoeffLocal fromPoints(const std::array<Point, 3>& pts) {
    return {
        pts[2].x - 2.0f * pts[1].x + pts[0].x,
        pts[2].y - 2.0f * pts[1].y + pts[0].y,
        2.0f * (pts[1].x - pts[0].x),
        2.0f * (pts[1].y - pts[0].y),
        pts[0].x,
        pts[0].y,
    };
  }

  Point eval(float t) const {
    return Point::fromXY((ax * t + bx) * t + cx, (ay * t + by) * t + cy);
  }
};

std::optional<Rect> intRectToRect(const IntRect& rect) {
  return Rect::fromLTRB(static_cast<float>(rect.x()), static_cast<float>(rect.y()),
                        static_cast<float>(rect.x()) + rect.width(),
                        static_cast<float>(rect.y()) + rect.height());
}

// Cubic coefficient representation.
struct CubicCoeffLocal {
  float ax, ay, bx, by, cx, cy, dx, dy;

  static CubicCoeffLocal fromPoints(const std::array<Point, 4>& pts) {
    return {
        pts[3].x + 3.0f * (pts[1].x - pts[2].x) - pts[0].x,
        pts[3].y + 3.0f * (pts[1].y - pts[2].y) - pts[0].y,
        3.0f * (pts[2].x - 2.0f * pts[1].x + pts[0].x),
        3.0f * (pts[2].y - 2.0f * pts[1].y + pts[0].y),
        3.0f * (pts[1].x - pts[0].x),
        3.0f * (pts[1].y - pts[0].y),
        pts[0].x,
        pts[0].y,
    };
  }

  Point eval(float t) const {
    return Point::fromXY(((ax * t + bx) * t + cx) * t + dx, ((ay * t + by) * t + cy) * t + dy);
  }
};

bool lt90(const Point& left, const Point& pivot, const Point& right) {
  const auto leftVector = subtract(left, pivot);
  const auto rightVector = subtract(right, pivot);
  return (leftVector.x * rightVector.x + leftVector.y * rightVector.y) >= 0.0f;
}

bool quickCubicNicenessCheck(const std::array<Point, 4>& points) {
  return lt90(points[1], points[0], points[3]) && lt90(points[2], points[0], points[3]) &&
         lt90(points[1], points[3], points[0]) && lt90(points[2], points[3], points[0]);
}

bool pointsFinite(std::span<const Point> points) {
  return std::all_of(points.begin(), points.end(), [](const Point& point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
  });
}

std::optional<Rect> computeNoCheckQuadBounds(const std::array<Point, 3>& points) {
  const auto minX = std::min({points[0].x, points[1].x, points[2].x});
  const auto minY = std::min({points[0].y, points[1].y, points[2].y});
  const auto maxX = std::max({points[0].x, points[1].x, points[2].x});
  const auto maxY = std::max({points[0].y, points[1].y, points[2].y});
  return Rect::fromLTRB(minX, minY, maxX, maxY);
}

std::optional<Rect> computeNoCheckCubicBounds(const std::array<Point, 4>& points) {
  const auto minX = std::min({points[0].x, points[1].x, points[2].x, points[3].x});
  const auto minY = std::min({points[0].y, points[1].y, points[2].y, points[3].y});
  const auto maxX = std::max({points[0].x, points[1].x, points[2].x, points[3].x});
  const auto maxY = std::max({points[0].y, points[1].y, points[2].y, points[3].y});
  return Rect::fromLTRB(minX, minY, maxX, maxY);
}

void hairQuad2(const std::array<Point, 3>& points, const ScreenIntRect* clip, LineProc lineProc,
               Blitter& blitter) {
  const auto lines = 1u << computeQuadLevel(points);
  if (lines == 1u) {
    lineProc(std::span<const Point>{points.data(), 2}, clip, blitter);
    return;
  }

  constexpr std::size_t kMaxPoints = (1u << kMaxQuadSubdivideLevel) + 1;
  if (lines >= kMaxPoints) {
    return;
  }

  // Use coefficient-based evaluation with incremental t.
  const auto coeff = QuadCoeffLocal::fromPoints(points);
  const float dt = 1.0f / static_cast<float>(lines);
  float t = 0.0f;

  auto output = std::array<Point, kMaxPoints>{};
  output[0] = points[0];
  for (std::uint32_t i = 1; i < lines; ++i) {
    t += dt;
    output[i] = coeff.eval(t);
  }
  output[lines] = points[2];

  lineProc(std::span<const Point>{output.data(), lines + 1}, clip, blitter);
}

void hairCubic2(const std::array<Point, 4>& points, const ScreenIntRect* clip, LineProc lineProc,
                Blitter& blitter) {
  const auto lines = computeCubicSegments(points);
  if (lines == 1u) {
    // Draw line from start to END point (not control point 1).
    const auto line = std::array<Point, 2>{points[0], points[3]};
    lineProc(std::span<const Point>{line.data(), 2}, clip, blitter);
    return;
  }

  constexpr std::size_t kMaxPoints = (1u << kMaxCubicSubdivideLevel) + 1;
  if (lines >= kMaxPoints) {
    return;
  }

  // Use coefficient-based evaluation with incremental t.
  const auto coeff = CubicCoeffLocal::fromPoints(points);
  const float dt = 1.0f / static_cast<float>(lines);
  float t = 0.0f;

  auto output = std::array<Point, kMaxPoints>{};
  output[0] = points[0];
  for (std::uint32_t i = 1; i < lines; ++i) {
    t += dt;
    output[i] = coeff.eval(t);
  }

  if (!pointsFinite(std::span<const Point>{output.data(), lines})) {
    return;
  }

  output[lines] = points[3];
  lineProc(std::span<const Point>{output.data(), lines + 1}, clip, blitter);
}

void hairQuad(std::array<Point, 3> points, const ScreenIntRect* clip,
              const std::optional<Rect>& insetClip, const std::optional<Rect>& outsetClip,
              LineProc lineProc, Blitter& blitter) {
  if (insetClip.has_value() && outsetClip.has_value()) {
    const auto bounds = computeNoCheckQuadBounds(points);
    if (!bounds.has_value()) {
      return;
    }
    if (!geometricOverlap(outsetClip.value(), bounds.value())) {
      return;
    }
    if (geometricContains(insetClip.value(), bounds.value())) {
      clip = nullptr;
    }
  }

  hairQuad2(points, clip, lineProc, blitter);
}

void hairCubic(std::array<Point, 4> points, const ScreenIntRect* clip,
               const std::optional<Rect>& insetClip, const std::optional<Rect>& outsetClip,
               LineProc lineProc, Blitter& blitter) {
  if (insetClip.has_value() && outsetClip.has_value()) {
    const auto bounds = computeNoCheckCubicBounds(points);
    if (!bounds.has_value()) {
      return;
    }
    if (!geometricOverlap(outsetClip.value(), bounds.value())) {
      return;
    }
    if (geometricContains(insetClip.value(), bounds.value())) {
      clip = nullptr;
    }
  }

  if (quickCubicNicenessCheck(points)) {
    hairCubic2(points, clip, lineProc, blitter);
    return;
  }

  auto split = std::array<Point, 13>{};
  auto tValues = std::array<NormalizedF32Exclusive, 3>{
      NormalizedF32Exclusive::HALF, NormalizedF32Exclusive::HALF, NormalizedF32Exclusive::HALF};
  const auto count =
      pathGeometry::chopCubicAtMaxCurvature(points, tValues, std::span<Point>{split});
  for (std::size_t i = 0; i < count; ++i) {
    const auto offset = i * 3;
    const auto segment = std::array<Point, 4>{split[offset], split[offset + 1], split[offset + 2],
                                              split[offset + 3]};
    hairCubic2(segment, clip, lineProc, blitter);
  }
}

void hairLineRgn(std::span<const Point> points, const ScreenIntRect* clip, Blitter& blitter) {
  const auto fixedBounds = Rect::fromLTRB(-kMaxCoord, -kMaxCoord, kMaxCoord, kMaxCoord);
  if (!fixedBounds.has_value()) {
    return;
  }

  std::optional<Rect> clipBounds;
  if (clip != nullptr) {
    clipBounds = clip->toRect();
  }

  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    const auto segment = std::array<Point, 2>{points[i], points[i + 1]};
    auto clipped = std::array<Point, 2>{};
    if (!lineClipper::intersect(std::span<const Point, 2>{segment}, fixedBounds.value(),
                                 std::span<Point, 2>{clipped})) {
      continue;
    }

    auto working = clipped;
    if (clipBounds.has_value()) {
      auto clipped2 = std::array<Point, 2>{};
      if (!lineClipper::intersect(std::span<const Point, 2>{working}, clipBounds.value(),
                                   std::span<Point, 2>{clipped2})) {
        continue;
      }
      working = clipped2;
    }

    auto x0 = fdot6::fromF32(working[0].x);
    auto y0 = fdot6::fromF32(working[0].y);
    auto x1 = fdot6::fromF32(working[1].x);
    auto y1 = fdot6::fromF32(working[1].y);

    if (!fdot6::canConvertToFdot16(x0) || !fdot6::canConvertToFdot16(y0) ||
        !fdot6::canConvertToFdot16(x1) || !fdot6::canConvertToFdot16(y1)) {
      continue;
    }

    const auto dx = x1 - x0;
    const auto dy = y1 - y0;

    if (std::abs(dx) > std::abs(dy)) {
      if (x0 > x1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
      }

      auto ix0 = fdot6::round(x0);
      const auto ix1 = fdot6::round(x1);
      if (ix0 == ix1) {
        continue;
      }

      const auto slope = fdot16::divide(dy, dx);
      auto startY = fdot6::toFdot16(y0) +
                    (static_cast<std::int64_t>(slope) * ((32 - x0) & kHalfPixelMask) >> 6);
      auto maxY = std::numeric_limits<std::int32_t>::max();
      if (clipBounds.has_value()) {
        maxY = fdot16::fromF32(clipBounds.value().bottom());
      }

      while (true) {
        if (ix0 >= 0 && startY >= 0 && startY < maxY) {
          blitter.blitH(static_cast<std::uint32_t>(ix0), static_cast<std::uint32_t>(startY >> 16),
                        kLengthU32One);
        }
        startY += slope;
        ++ix0;
        if (ix0 >= ix1) {
          break;
        }
      }
      continue;
    }

    if (y0 > y1) {
      std::swap(x0, x1);
      std::swap(y0, y1);
    }

    auto iy0 = fdot6::round(y0);
    const auto iy1 = fdot6::round(y1);
    if (iy0 == iy1) {
      continue;
    }

    const auto slope = fdot16::divide(dx, dy);
    auto startX = fdot6::toFdot16(x0) +
                  (static_cast<std::int64_t>(slope) * ((32 - y0) & kHalfPixelMask) >> 6);

    while (true) {
      if (startX >= 0 && iy0 >= 0) {
        blitter.blitH(static_cast<std::uint32_t>(startX >> 16), static_cast<std::uint32_t>(iy0),
                      kLengthU32One);
      }
      startX += slope;
      ++iy0;
      if (iy0 >= iy1) {
        break;
      }
    }
  }
}

void strokePathSegments(const Path& path, LineCap lineCap, const ScreenIntRect& clip,
                        LineProc lineProc, const std::optional<Rect>& insetClip,
                        const std::optional<Rect>& outsetClip, Blitter& blitter) {
  const auto verbs = path.verbs();
  const auto points = path.points();

  PathVerb prevVerb = PathVerb::Move;
  Point firstPoint{};
  Point lastPoint{};

  std::size_t pointIndex = 0;

  for (std::size_t verbIndex = 0; verbIndex < verbs.size(); ++verbIndex) {
    const auto verb = verbs[verbIndex];
    const auto nextVerb =
        (verbIndex + 1 < verbs.size()) ? std::optional{verbs[verbIndex + 1]} : std::nullopt;
    const auto* clipPtr = &clip;

    if (verb == PathVerb::Move) {
      if (pointIndex >= points.size()) {
        return;
      }
      firstPoint = points[pointIndex];
      lastPoint = points[pointIndex];
      prevVerb = verb;
      ++pointIndex;
      continue;
    }

    if (verb == PathVerb::Close) {
      auto close = std::array<Point, 2>{lastPoint, firstPoint};
      if (lineCap != LineCap::Butt && prevVerb == PathVerb::Move) {
        extendPts(lineCap, prevVerb, nextVerb, close);
      }
      lineProc(std::span<const Point>{close.data(), close.size()}, clipPtr, blitter);
      if (lineCap != LineCap::Butt) {
        prevVerb = verb;
      }
      continue;
    }

    if (verb == PathVerb::Line) {
      if (pointIndex >= points.size()) {
        return;
      }

      auto line = std::array<Point, 2>{lastPoint, points[pointIndex]};
      if (lineCap != LineCap::Butt) {
        extendPts(lineCap, prevVerb, nextVerb, line);
      }
      lineProc(std::span<const Point>{line.data(), line.size()}, clipPtr, blitter);
      const auto lineStart = line[0];

      lastPoint = points[pointIndex];
      if (prevVerb == PathVerb::Move && lineCap != LineCap::Butt) {
        firstPoint = lineStart;
      }
      prevVerb = verb;
      ++pointIndex;
      continue;
    }

    if (verb == PathVerb::Quad) {
      if (pointIndex + 1 >= points.size()) {
        return;
      }
      auto quad = std::array<Point, 3>{lastPoint, points[pointIndex], points[pointIndex + 1]};
      if (lineCap != LineCap::Butt) {
        extendPts(lineCap, prevVerb, nextVerb, quad);
      }
      hairQuad(quad, clipPtr, insetClip, outsetClip, lineProc, blitter);
      const auto lineStart = quad[0];

      lastPoint = points[pointIndex + 1];
      if (prevVerb == PathVerb::Move && lineCap != LineCap::Butt) {
        firstPoint = lineStart;
      }
      prevVerb = verb;
      pointIndex += 2;
      continue;
    }

    if (pointIndex + 2 >= points.size()) {
      return;
    }
    auto cubic = std::array<Point, 4>{lastPoint, points[pointIndex], points[pointIndex + 1],
                                      points[pointIndex + 2]};
    if (lineCap != LineCap::Butt) {
      extendPts(lineCap, prevVerb, nextVerb, cubic);
    }
    hairCubic(cubic, clipPtr, insetClip, outsetClip, lineProc, blitter);
    const auto lineStart = cubic[0];

    lastPoint = points[pointIndex + 2];
    if (prevVerb == PathVerb::Move && lineCap != LineCap::Butt) {
      firstPoint = lineStart;
    }
    prevVerb = verb;
    pointIndex += 3;
  }
}

namespace scan {

void strokePath(const Path& path, LineCap lineCap, const ScreenIntRect& clip, Blitter& blitter) {
  strokePathImpl(path, lineCap, clip, hairLineRgn, blitter);
}

void strokePathImpl(const Path& path, LineCap lineCap, const ScreenIntRect& clip, LineProc lineProc,
                    Blitter& blitter) {
  const auto capOutset = lineCap == LineCap::Butt ? 1.0f : 2.0f;
  const auto pathBounds = makeOutset(path.bounds(), capOutset, capOutset);
  if (!pathBounds.has_value()) {
    return;
  }
  const auto pathBoundsInt = pathBounds.value().roundOut();
  if (!pathBoundsInt.has_value()) {
    return;
  }

  const auto clipInt = clip.toIntRect();
  if (!clipInt.intersect(pathBoundsInt.value()).has_value()) {
    return;
  }

  std::optional<Rect> insetClip;
  std::optional<Rect> outsetClip;
  if (!isIntRectContaining(clipInt, pathBoundsInt.value())) {
    const auto inset = makeInset(clipInt, 1, 1);
    const auto outset = makeOutset(clipInt, 1, 1);
    if (!inset.has_value() || !outset.has_value()) {
      return;
    }
    insetClip = intRectToRect(inset.value());
    outsetClip = intRectToRect(outset.value());
  }

  strokePathSegments(path, lineCap, clip, lineProc, insetClip, outsetClip, blitter);
}

}  // namespace scan

}  // namespace tiny_skia
