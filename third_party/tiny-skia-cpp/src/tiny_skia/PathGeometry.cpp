#include "tiny_skia/PathGeometry.h"

#include <cmath>
#include <limits>
#include <span>

#include "tiny_skia/Math.h"
#include "tiny_skia/Transform.h"
#include "tiny_skia/path64/Cubic64.h"
#include "tiny_skia/path64/LineCubicIntersections.h"
#include "tiny_skia/path64/Point64.h"
#include "tiny_skia/path64/Quad64.h"

namespace tiny_skia::pathGeometry {

// Forward declarations for functions defined later in this file.
std::size_t findUnitQuadRoots(float a, float b, float c, NormalizedF32Exclusive roots[3]);
std::optional<NormalizedF32Exclusive> validUnitDivideF32(float numer, float denom);

namespace {

bool isNotMonotonic(float a, float b, float c) {
  auto ab = a - b;
  auto bc = b - c;
  if (ab < 0.0f) {
    bc = -bc;
  }
  return ab == 0.0f || bc < 0.0f;
}

// Pure f32 de Casteljau's algorithm.
// Uses interp(v0, v1, t) = v0 + (v1 - v0) * t in f32 precision.
void chopCubicAt2(std::span<const Point, 4> src, float t, std::span<Point> dst) {
  if (dst.size() < 7 || t <= 0.0f || t >= 1.0f) {
    std::fill_n(dst.data(), dst.size(), Point{});
    return;
  }

  auto interp = [](float v0, float v1, float tt) -> float { return v0 + (v1 - v0) * tt; };

  const float abx = interp(src[0].x, src[1].x, t);
  const float aby = interp(src[0].y, src[1].y, t);
  const float bcx = interp(src[1].x, src[2].x, t);
  const float bcy = interp(src[1].y, src[2].y, t);
  const float cdx = interp(src[2].x, src[3].x, t);
  const float cdy = interp(src[2].y, src[3].y, t);
  const float abcx = interp(abx, bcx, t);
  const float abcy = interp(aby, bcy, t);
  const float bcdx = interp(bcx, cdx, t);
  const float bcdy = interp(bcy, cdy, t);
  const float abcdx = interp(abcx, bcdx, t);
  const float abcdy = interp(abcy, bcdy, t);

  dst[0] = src[0];
  dst[1] = Point::fromXY(abx, aby);
  dst[2] = Point::fromXY(abcx, abcy);
  dst[3] = Point::fromXY(abcdx, abcdy);
  dst[4] = Point::fromXY(bcdx, bcdy);
  dst[5] = Point::fromXY(cdx, cdy);
  dst[6] = src[3];
}

bool chopMonoCubicAt(const std::array<Point, 4>& src, float intercept, bool isVertical,
                     std::array<Point, 7>& dst) {
  const auto cubic = tiny_skia::path64::cubic64::Cubic64::create({
      Point64::fromPoint(src[0]),
      Point64::fromPoint(src[1]),
      Point64::fromPoint(src[2]),
      Point64::fromPoint(src[3]),
  });

  auto roots = std::array<double, 3>{};
  const auto count = isVertical ? tiny_skia::path64::line_cubic_intersections::verticalIntersect(
                                      cubic, static_cast<double>(intercept), roots)
                                : tiny_skia::path64::line_cubic_intersections::horizontalIntersect(
                                      cubic, static_cast<double>(intercept), roots);
  if (count > 0) {
    chopCubicAt2(std::span<const Point, 4>(src), roots[0], std::span<Point, 7>(dst));
    return true;
  }
  return false;
}

bool chopMonoQuadAt(const std::array<Point, 3>& src, float intercept, bool isVertical, float& t) {
  // Use f32 find_unit_quad_roots (not double quad64::rootsValidT).
  const auto c0 = isVertical ? src[0].x : src[0].y;
  const auto c1 = isVertical ? src[1].x : src[1].y;
  const auto c2 = isVertical ? src[2].x : src[2].y;
  const auto a = c0 - c1 - c1 + c2;
  const auto b = 2.0f * (c1 - c0);
  const auto c = c0 - intercept;
  NormalizedF32Exclusive roots[3] = {NormalizedF32Exclusive::HALF, NormalizedF32Exclusive::HALF,
                                     NormalizedF32Exclusive::HALF};
  const auto count = findUnitQuadRoots(a, b, c, roots);
  if (count > 0) {
    t = roots[0].get();
    return true;
  }
  return false;
}

}  // namespace

std::size_t chopQuadAt(const std::array<Point, 3>& src, float t, std::array<Point, 5>& dst) {
  // Pure f32 interpolation via interp(v0, v1, t) = v0 + (v1 - v0) * t
  auto interp = [](float v0, float v1, float tt) -> float { return v0 + (v1 - v0) * tt; };
  const auto p01 = Point{interp(src[0].x, src[1].x, t), interp(src[0].y, src[1].y, t)};
  const auto p12 = Point{interp(src[1].x, src[2].x, t), interp(src[1].y, src[2].y, t)};
  const auto p012 = Point{interp(p01.x, p12.x, t), interp(p01.y, p12.y, t)};

  dst[0] = src[0];
  dst[1] = p01;
  dst[2] = p012;
  dst[3] = p12;
  dst[4] = src[2];
  return 1;
}

std::size_t chopQuadAtXExtrema(const std::array<Point, 3>& src, std::array<Point, 5>& dst) {
  auto a = src[0].x;
  auto b = src[1].x;
  const auto c = src[2].x;

  if (isNotMonotonic(a, b, c)) {
    // Use f32 validUnitDivide (not double).
    if (auto tOpt = validUnitDivideF32(a - b, a - b - b + c)) {
      chopQuadAt(src, tOpt->get(), dst);
      dst[1].x = dst[2].x;
      dst[3].x = dst[2].x;
      return 1;
    }

    b = std::abs(a - b) < std::abs(b - c) ? a : c;
  }

  dst[0] = Point{a, src[0].y};
  dst[1] = Point{b, src[1].y};
  dst[2] = Point{c, src[2].y};
  return 0;
}

std::size_t chopQuadAtYExtrema(const std::array<Point, 3>& src, std::array<Point, 5>& dst) {
  auto a = src[0].y;
  auto b = src[1].y;
  const auto c = src[2].y;

  if (isNotMonotonic(a, b, c)) {
    // Use f32 validUnitDivide (not double).
    if (auto tOpt = validUnitDivideF32(a - b, a - b - b + c)) {
      chopQuadAt(src, tOpt->get(), dst);
      dst[1].y = dst[2].y;
      dst[3].y = dst[2].y;
      return 1;
    }

    b = std::abs(a - b) < std::abs(b - c) ? a : c;
  }

  dst[0] = Point{src[0].x, a};
  dst[1] = Point{src[1].x, b};
  dst[2] = Point{src[2].x, c};
  return 0;
}

std::optional<NormalizedF32Exclusive> validUnitDivideF32(float numer, float denom) {
  if (numer < 0.0f) {
    numer = -numer;
    denom = -denom;
  }
  if (denom == 0.0f || numer == 0.0f || numer >= denom) {
    return std::nullopt;
  }
  float r = numer / denom;
  return NormalizedF32Exclusive::create(r);
}

// Uses f32 t values and pure f32 de Casteljau's.
std::size_t chopCubicAt(std::span<const Point> src, std::span<const NormalizedF32Exclusive> tValues,
                        std::span<Point> dst) {
  if (src.size() != 4 || dst.size() < 4) {
    return 0;
  }

  if (tValues.empty()) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
    return 0;
  }

  auto srcWork = std::array<Point, 4>{src[0], src[1], src[2], src[3]};
  auto t = tValues[0];
  std::size_t offset = 0;
  for (std::size_t i = 0; i < tValues.size(); ++i) {
    if (offset + 7 > dst.size()) {
      return tValues.size();
    }
    chopCubicAt2(std::span<const Point, 4>(srcWork), t.get(), dst.subspan(offset));

    if (i + 1 == tValues.size()) {
      break;
    }

    offset += 3;
    // Use output from chopCubicAt2 as next iteration's source.
    srcWork = {dst[offset + 0], dst[offset + 1], dst[offset + 2], dst[offset + 3]};

    // Renormalize t using f32 validUnitDivide.
    auto nextT =
        validUnitDivideF32(tValues[i + 1].get() - tValues[i].get(), 1.0f - tValues[i].get());
    if (!nextT.has_value()) {
      dst[offset + 4] = srcWork[3];
      dst[offset + 5] = srcWork[3];
      dst[offset + 6] = srcWork[3];
      break;
    }
    t = *nextT;
  }

  return tValues.size();
}

std::size_t chopCubicAtXExtrema(const std::array<Point, 4>& src, std::array<Point, 10>& dst) {
  auto tValues = newTValues();
  const auto rawCount = findCubicExtremaT(src[0].x, src[1].x, src[2].x, src[3].x, tValues.data());
  const auto split = chopCubicAt(std::span<const Point, 4>(src),
                                 std::span<const NormalizedF32Exclusive>(tValues.data(), rawCount),
                                 std::span<Point, 10>(dst));
  if (split > 0) {
    dst[2].x = dst[3].x;
    dst[4].x = dst[3].x;
    if (split == 2) {
      dst[5].x = dst[6].x;
      dst[7].x = dst[6].x;
    }
  }
  return split;
}

std::size_t chopCubicAtYExtrema(const std::array<Point, 4>& src, std::array<Point, 10>& dst) {
  auto tValues = newTValues();
  const auto rawCount = findCubicExtremaT(src[0].y, src[1].y, src[2].y, src[3].y, tValues.data());
  const auto split = chopCubicAt(std::span<const Point, 4>(src),
                                 std::span<const NormalizedF32Exclusive>(tValues.data(), rawCount),
                                 std::span<Point, 10>(dst));
  if (split > 0) {
    dst[2].y = dst[3].y;
    dst[4].y = dst[3].y;
    if (split == 2) {
      dst[5].y = dst[6].y;
      dst[7].y = dst[6].y;
    }
  }
  return split;
}

std::size_t chopCubicAtMaxCurvature(const std::array<Point, 4>& src,
                                    std::array<NormalizedF32Exclusive, 3>& tValues,
                                    std::span<Point> dst) {
  if (dst.size() < 4) {
    return 0;
  }

  // Use f32 root-finding (findCubicMaxCurvatureTs / solveCubicPoly)
  // NOT the f64 findMaxCurvatureRoots / cubic64::rootsValidT path.
  auto roots =
      std::array<NormalizedF32, 3>{NormalizedF32::ZERO, NormalizedF32::ZERO, NormalizedF32::ZERO};
  const auto rootCount = findCubicMaxCurvatureTs(src.data(), roots.data());
  std::size_t count = 0;
  for (std::size_t i = 0; i < rootCount; ++i) {
    if (roots[i].get() > 0.0f && roots[i].get() < 1.0f) {
      tValues[count] = NormalizedF32Exclusive::newBounded(roots[i].get());
      ++count;
    }
  }

  if (count == 0) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
    return 1;
  }

  return 1 + chopCubicAt(std::span<const Point, 4>(src),
                         std::span<const NormalizedF32Exclusive>(tValues.data(), count), dst);
}

bool chopMonoQuadAtX(const std::array<Point, 3>& src, float x, float& t) {
  return chopMonoQuadAt(src, x, true, t);
}

bool chopMonoQuadAtY(const std::array<Point, 3>& src, float y, float& t) {
  return chopMonoQuadAt(src, y, false, t);
}

bool chopMonoCubicAtX(const std::array<Point, 4>& src, float x, std::array<Point, 7>& dst) {
  return chopMonoCubicAt(src, x, true, dst);
}

bool chopMonoCubicAtY(const std::array<Point, 4>& src, float y, std::array<Point, 7>& dst) {
  return chopMonoCubicAt(src, y, false, dst);
}

// --- New functions needed by stroker / dash ---

namespace {

// float interp helper
float interpF(float a, float b, float t) { return a + (b - a) * t; }

Point interpPt(Point a, Point b, float t) {
  return Point::fromXY(interpF(a.x, b.x, t), interpF(a.y, b.y, t));
}

// Quad coefficient representation
struct QuadCoeff {
  float ax, ay;  // coefficient a
  float bx, by;  // coefficient b
  float cx, cy;  // coefficient c

  static QuadCoeff fromPoints(const Point src[3]) {
    float cx = src[0].x;
    float cy = src[0].y;
    float bx = 2.0f * (src[1].x - cx);
    float by = 2.0f * (src[1].y - cy);
    float ax = src[2].x - 2.0f * src[1].x + cx;
    float ay = src[2].y - 2.0f * src[1].y + cy;
    return {ax, ay, bx, by, cx, cy};
  }

  Point eval(float t) const {
    return Point::fromXY((ax * t + bx) * t + cx, (ay * t + by) * t + cy);
  }
};

// Cubic coefficient representation
struct CubicCoeff {
  float ax, ay, bx, by, cx, cy, dx, dy;

  static CubicCoeff fromPoints(const Point src[4]) {
    float p0x = src[0].x, p0y = src[0].y;
    float p1x = src[1].x, p1y = src[1].y;
    float p2x = src[2].x, p2y = src[2].y;
    float p3x = src[3].x, p3y = src[3].y;

    return {
        p3x + 3.0f * (p1x - p2x) - p0x,
        p3y + 3.0f * (p1y - p2y) - p0y,
        3.0f * (p2x - 2.0f * p1x + p0x),
        3.0f * (p2y - 2.0f * p1y + p0y),
        3.0f * (p1x - p0x),
        3.0f * (p1y - p0y),
        p0x,
        p0y,
    };
  }

  Point eval(float t) const {
    return Point::fromXY(((ax * t + bx) * t + cx) * t + dx, ((ay * t + by) * t + cy) * t + dy);
  }
};

Point evalCubicDerivative(const Point src[4], NormalizedF32 t) {
  float p0x = src[0].x, p0y = src[0].y;
  float p1x = src[1].x, p1y = src[1].y;
  float p2x = src[2].x, p2y = src[2].y;
  float p3x = src[3].x, p3y = src[3].y;

  float ax = p3x + 3.0f * (p1x - p2x) - p0x;
  float ay = p3y + 3.0f * (p1y - p2y) - p0y;
  float bx = 2.0f * (p2x - 2.0f * p1x + p0x);
  float by = 2.0f * (p2y - 2.0f * p1y + p0y);
  float cx = p1x - p0x;
  float cy = p1y - p0y;

  float tv = t.get();
  return Point::fromXY((ax * tv + bx) * tv + cx, (ay * tv + by) * tv + cy);
}

// formulateF1DotF2 (float variant for cubic max curvature)
std::array<float, 4> formulateF1DotF2f(const float src[4]) {
  float a = src[1] - src[0];
  float b = src[2] - 2.0f * src[1] + src[0];
  float c = src[3] + 3.0f * (src[1] - src[2]) - src[0];
  return {c * c, 3.0f * b * c, 2.0f * b * b + c * a, a * b};
}

constexpr float kFloatPi = 3.14159265f;

float scalarCubeRoot(float x) {
  return std::pow(std::abs(x), 1.0f / 3.0f) * (x < 0 ? -1.0f : 1.0f);
}

void sortArray3(NormalizedF32 arr[3]) {
  if (arr[0] > arr[1]) std::swap(arr[0], arr[1]);
  if (arr[1] > arr[2]) std::swap(arr[1], arr[2]);
  if (arr[0] > arr[1]) std::swap(arr[0], arr[1]);
}

std::size_t collapseDuplicates3(NormalizedF32 arr[3]) {
  std::size_t len = 3;
  if (arr[1] == arr[2]) len = 2;
  if (arr[0] == arr[1]) len = 1;
  return len;
}

std::size_t solveCubicPoly(const float coeff[4], NormalizedF32 tValues[3]) {
  if (isNearlyZero(coeff[0])) {
    NormalizedF32Exclusive tmpT[3] = {NormalizedF32Exclusive::HALF, NormalizedF32Exclusive::HALF,
                                      NormalizedF32Exclusive::HALF};
    auto count = findUnitQuadRoots(coeff[1], coeff[2], coeff[3], tmpT);
    for (std::size_t i = 0; i < count; ++i) {
      tValues[i] = tmpT[i].toNormalized();
    }
    return count;
  }

  float inva = 1.0f / coeff[0];
  float a = coeff[1] * inva;
  float b = coeff[2] * inva;
  float c = coeff[3] * inva;

  float q = (a * a - b * 3.0f) / 9.0f;
  float r = (2.0f * a * a * a - 9.0f * a * b + 27.0f * c) / 54.0f;

  float q3 = q * q * q;
  float r2MinusQ3 = r * r - q3;
  float adiv3 = a / 3.0f;

  if (r2MinusQ3 < 0.0f) {
    float qSqrt = std::sqrt(q);
    float div = r / (q * qSqrt);
    // clamp to [-1, 1]
    if (div < -1.0f) div = -1.0f;
    if (div > 1.0f) div = 1.0f;
    float theta = std::acos(div);
    float neg2RootQ = -2.0f * qSqrt;

    tValues[0] = NormalizedF32::newClamped(neg2RootQ * std::cos(theta / 3.0f) - adiv3);
    tValues[1] =
        NormalizedF32::newClamped(neg2RootQ * std::cos((theta + 2.0f * kFloatPi) / 3.0f) - adiv3);
    tValues[2] =
        NormalizedF32::newClamped(neg2RootQ * std::cos((theta - 2.0f * kFloatPi) / 3.0f) - adiv3);

    sortArray3(tValues);
    return collapseDuplicates3(tValues);
  } else {
    float absR = std::abs(r);
    float av = absR + std::sqrt(r2MinusQ3);
    av = scalarCubeRoot(av);
    if (r > 0.0f) av = -av;
    if (av != 0.0f) av += q / av;
    tValues[0] = NormalizedF32::newClamped(av - adiv3);
    return 1;
  }
}

bool onSameSide(const Point src[4], std::size_t testIndex, std::size_t lineIndex) {
  Point origin = src[lineIndex];
  Point line = src[lineIndex + 1] - origin;
  float crosses[2];
  for (std::size_t i = 0; i < 2; ++i) {
    Point testLine = src[testIndex + i] - origin;
    crosses[i] = line.cross(testLine);
  }
  return crosses[0] * crosses[1] >= 0.0f;
}

float calcCubicPrecision(const Point src[4]) {
  return (src[1].distanceToSquared(src[0]) + src[2].distanceToSquared(src[1]) +
          src[3].distanceToSquared(src[2])) *
         1e-8f;
}

}  // namespace

void chopQuadAtT(const Point src[3], NormalizedF32Exclusive t, Point dst[5]) {
  float tv = t.get();
  Point p01 = interpPt(src[0], src[1], tv);
  Point p12 = interpPt(src[1], src[2], tv);
  Point p012 = interpPt(p01, p12, tv);
  dst[0] = src[0];
  dst[1] = p01;
  dst[2] = p012;
  dst[3] = p12;
  dst[4] = src[2];
}

void chopCubicAt2(const Point src[4], NormalizedF32Exclusive t, Point dst[7]) {
  float tv = t.get();
  Point ab = interpPt(src[0], src[1], tv);
  Point bc = interpPt(src[1], src[2], tv);
  Point cd = interpPt(src[2], src[3], tv);
  Point abc = interpPt(ab, bc, tv);
  Point bcd = interpPt(bc, cd, tv);
  Point abcd = interpPt(abc, bcd, tv);
  dst[0] = src[0];
  dst[1] = ab;
  dst[2] = abc;
  dst[3] = abcd;
  dst[4] = bcd;
  dst[5] = cd;
  dst[6] = src[3];
}

Point evalQuadAt(const Point src[3], NormalizedF32 t) {
  return QuadCoeff::fromPoints(src).eval(t.get());
}

Point evalQuadTangentAt(const Point src[3], NormalizedF32 t) {
  if ((t == NormalizedF32::ZERO && src[0] == src[1]) ||
      (t == NormalizedF32::ONE && src[1] == src[2])) {
    return src[2] - src[0];
  }

  float bx = src[1].x - src[0].x;
  float by = src[1].y - src[0].y;
  float ax = src[2].x - src[1].x - bx;
  float ay = src[2].y - src[1].y - by;
  float tv = t.get();
  float tx = ax * tv + bx;
  float ty = ay * tv + by;
  return Point::fromXY(tx + tx, ty + ty);
}

Point evalCubicPosAt(const Point src[4], NormalizedF32 t) {
  return CubicCoeff::fromPoints(src).eval(t.get());
}

Point evalCubicTangentAt(const Point src[4], NormalizedF32 t) {
  if ((t.get() == 0.0f && src[0] == src[1]) || (t.get() == 1.0f && src[2] == src[3])) {
    Point tangent;
    if (t.get() == 0.0f) {
      tangent = src[2] - src[0];
    } else {
      tangent = src[3] - src[1];
    }
    if (tangent.x == 0.0f && tangent.y == 0.0f) {
      tangent = src[3] - src[0];
    }
    return tangent;
  }
  return evalCubicDerivative(src, t);
}

NormalizedF32 findQuadMaxCurvature(const Point src[3]) {
  float ax = src[1].x - src[0].x;
  float ay = src[1].y - src[0].y;
  float bx = src[0].x - src[1].x - src[1].x + src[2].x;
  float by = src[0].y - src[1].y - src[1].y + src[2].y;

  float numer = -(ax * bx + ay * by);
  float denom = bx * bx + by * by;
  if (denom < 0.0f) {
    numer = -numer;
    denom = -denom;
  }
  if (numer <= 0.0f) return NormalizedF32::ZERO;
  if (numer >= denom) return NormalizedF32::ONE;

  float t = numer / denom;
  auto result = NormalizedF32::create(t);
  return result.value_or(NormalizedF32::ZERO);
}

std::optional<NormalizedF32Exclusive> findQuadExtrema(float a, float b, float c) {
  return validUnitDivideF32(a - b, a - b - b + c);
}

std::size_t findCubicExtremaT(float a, float b, float c, float d,
                              NormalizedF32Exclusive tValues[3]) {
  // We divide A, B, C by 3 to simplify.
  const float aa = d - a + 3.0f * (b - c);
  const float bb = 2.0f * (a - b - b + c);
  const float cc = b - a;
  return findUnitQuadRoots(aa, bb, cc, tValues);
}

std::size_t findCubicInflections(const Point src[4], NormalizedF32Exclusive tValues[3]) {
  float ax = src[1].x - src[0].x;
  float ay = src[1].y - src[0].y;
  float bx = src[2].x - 2.0f * src[1].x + src[0].x;
  float by = src[2].y - 2.0f * src[1].y + src[0].y;
  float cx = src[3].x + 3.0f * (src[1].x - src[2].x) - src[0].x;
  float cy = src[3].y + 3.0f * (src[1].y - src[2].y) - src[0].y;

  return findUnitQuadRoots(bx * cy - by * cx, ax * cy - ay * cx, ax * by - ay * bx, tValues);
}

std::size_t findCubicMaxCurvatureTs(const Point src[4], NormalizedF32 tValues[3]) {
  float srcX[4] = {src[0].x, src[1].x, src[2].x, src[3].x};
  float srcY[4] = {src[0].y, src[1].y, src[2].y, src[3].y};
  auto coeffX = formulateF1DotF2f(srcX);
  auto coeffY = formulateF1DotF2f(srcY);
  float coeff[4];
  for (int i = 0; i < 4; ++i) {
    coeff[i] = coeffX[i] + coeffY[i];
  }
  return solveCubicPoly(coeff, tValues);
}

std::optional<NormalizedF32Exclusive> findCubicCusp(const Point src[4]) {
  if (src[0] == src[1]) return std::nullopt;
  if (src[2] == src[3]) return std::nullopt;

  if (onSameSide(src, 0, 2) || onSameSide(src, 2, 0)) {
    return std::nullopt;
  }

  NormalizedF32 tVals[3];
  auto count = findCubicMaxCurvatureTs(src, tVals);
  for (std::size_t i = 0; i < count; ++i) {
    if (tVals[i].get() <= 0.0f || tVals[i].get() >= 1.0f) continue;
    auto dPt = evalCubicDerivative(src, tVals[i]);
    float dPtMag = dPt.lengthSquared();
    float precision = calcCubicPrecision(src);
    if (dPtMag < precision) {
      return NormalizedF32Exclusive::newBounded(tVals[i].get());
    }
  }
  return std::nullopt;
}

std::size_t findUnitQuadRoots(float a, float b, float c, NormalizedF32Exclusive roots[3]) {
  if (a == 0.0f) {
    auto r = validUnitDivideF32(-c, b);
    if (r.has_value()) {
      roots[0] = *r;
      return 1;
    }
    return 0;
  }

  double dr = static_cast<double>(b) * static_cast<double>(b) -
              4.0 * static_cast<double>(a) * static_cast<double>(c);
  if (dr < 0.0) return 0;
  dr = std::sqrt(dr);
  float r = static_cast<float>(dr);
  if (!std::isfinite(r)) return 0;

  float q = (b < 0.0f) ? -(b - r) / 2.0f : -(b + r) / 2.0f;

  std::size_t count = 0;
  if (auto rv = validUnitDivideF32(q, a)) {
    roots[count++] = *rv;
  }
  if (auto rv = validUnitDivideF32(c, q)) {
    roots[count++] = *rv;
  }

  if (count == 2) {
    if (roots[0].get() > roots[1].get()) {
      std::swap(roots[0], roots[1]);
    } else if (roots[0] == roots[1]) {
      count = 1;
    }
  }
  return count;
}

// Conic implementation

Conic Conic::create(Point p0, Point p1, Point p2, float w) {
  Conic c;
  c.points[0] = p0;
  c.points[1] = p1;
  c.points[2] = p2;
  c.weight = w;
  return c;
}

Conic Conic::fromPoints(const Point pts[], float w) { return create(pts[0], pts[1], pts[2], w); }

void Conic::chop(Conic dst[2]) const {
  float scale = 1.0f / (1.0f + weight);
  float newW = std::sqrt(0.5f + weight * 0.5f);

  Point wp1 = points[1].scaled(weight);
  Point m = Point::fromXY((points[0].x + (wp1.x + wp1.x) + points[2].x) * scale * 0.5f,
                          (points[0].y + (wp1.y + wp1.y) + points[2].y) * scale * 0.5f);

  // If m is not finite, recompute using f64 for precision.
  if (!std::isfinite(m.x) || !std::isfinite(m.y)) {
    double wD = static_cast<double>(weight);
    double w2 = wD * 2.0;
    double scaleHalf = 1.0 / (1.0 + wD) * 0.5;
    m.x = static_cast<float>((static_cast<double>(points[0].x) +
                              w2 * static_cast<double>(points[1].x) +
                              static_cast<double>(points[2].x)) *
                             scaleHalf);
    m.y = static_cast<float>((static_cast<double>(points[0].y) +
                              w2 * static_cast<double>(points[1].y) +
                              static_cast<double>(points[2].y)) *
                             scaleHalf);
  }

  dst[0].points[0] = points[0];
  dst[0].points[1] = Point::fromXY((points[0].x + wp1.x) * scale, (points[0].y + wp1.y) * scale);
  dst[0].points[2] = m;
  dst[0].weight = newW;

  dst[1].points[0] = m;
  dst[1].points[1] = Point::fromXY((wp1.x + points[2].x) * scale, (wp1.y + points[2].y) * scale);
  dst[1].points[2] = points[2];
  dst[1].weight = newW;
}

std::optional<std::uint8_t> Conic::computeQuadPow2(float tolerance) const {
  if (tolerance < 0.0f || !std::isfinite(tolerance)) return std::nullopt;

  if (!std::isfinite(points[0].x) || !std::isfinite(points[0].y) || !std::isfinite(points[1].x) ||
      !std::isfinite(points[1].y) || !std::isfinite(points[2].x) || !std::isfinite(points[2].y)) {
    return std::nullopt;
  }

  float a = weight - 1.0f;
  float k = a / (4.0f * (2.0f + a));
  float x = k * (points[0].x - 2.0f * points[1].x + points[2].x);
  float y = k * (points[0].y - 2.0f * points[1].y + points[2].y);

  float error = std::sqrt(x * x + y * y);
  std::uint8_t pow2 = 0;
  for (; pow2 < 4; ++pow2) {
    if (error <= tolerance) break;
    error *= 0.25f;
  }
  return std::max(pow2, static_cast<std::uint8_t>(1));
}

namespace {

// Returns true if (a <= b <= c) || (a >= b >= c)
bool between(float a, float b, float c) { return (a - b) * (c - b) <= 0.0f; }

// Recursive conic subdivision with Y-monotonicity preservation.
Point* subdivideRecursive(const Conic& src, Point* points, std::uint8_t level) {
  if (level == 0) {
    points[0] = src.points[1];
    points[1] = src.points[2];
    return points + 2;
  }

  Conic dst[2];
  src.chop(dst);

  const float startY = src.points[0].y;
  const float endY = src.points[2].y;
  if (between(startY, src.points[1].y, endY)) {
    // If the input is monotonic and the output is not, the scan converter hangs.
    // Ensure that the chopped conics maintain their y-order.
    float midY = dst[0].points[2].y;
    if (!between(startY, midY, endY)) {
      float closerY = (std::abs(midY - startY) < std::abs(midY - endY)) ? startY : endY;
      dst[0].points[2].y = closerY;
      dst[1].points[0].y = closerY;
    }

    if (!between(startY, dst[0].points[1].y, dst[0].points[2].y)) {
      dst[0].points[1].y = startY;
    }

    if (!between(dst[1].points[0].y, dst[1].points[1].y, endY)) {
      dst[1].points[1].y = endY;
    }
  }

  --level;
  points = subdivideRecursive(dst[0], points, level);
  return subdivideRecursive(dst[1], points, level);
}

}  // namespace

std::uint8_t Conic::chopIntoQuadsPow2(std::uint8_t pow2, Point dst[]) const {
  dst[0] = points[0];
  subdivideRecursive(*this, dst + 1, pow2);

  const auto quadCount = static_cast<std::size_t>(1) << pow2;
  const auto ptCount = 2 * quadCount + 1;

  // If we generated a non-finite point, pin to the middle of the hull.
  bool hasNonFinite = false;
  for (std::size_t i = 0; i < ptCount; ++i) {
    if (!std::isfinite(dst[i].x) || !std::isfinite(dst[i].y)) {
      hasNonFinite = true;
      break;
    }
  }
  if (hasNonFinite) {
    for (std::size_t i = 1; i < ptCount - 1; ++i) {
      dst[i] = points[1];
    }
  }

  return static_cast<std::uint8_t>(1 << pow2);
}

std::optional<std::span<const Conic>> Conic::buildUnitArc(Point uStart, Point uStop,
                                                          PathDirection dir,
                                                          const Transform& userTransform,
                                                          Conic dst[5]) {
  float x = uStart.dot(uStop);
  float y = uStart.cross(uStop);
  float absY = std::abs(y);

  constexpr float kNearlyZero = kScalarNearlyZero;

  if (absY <= kNearlyZero && x > 0.0f &&
      ((y >= 0.0f && dir == PathDirection::CW) || (y <= 0.0f && dir == PathDirection::CCW))) {
    return std::nullopt;
  }

  if (dir == PathDirection::CCW) {
    y = -y;
  }

  std::size_t quadrant = 0;
  if (y == 0.0f) {
    quadrant = 2;
  } else if (x == 0.0f) {
    quadrant = (y > 0.0f) ? 1 : 3;
  } else {
    if (y < 0.0f) quadrant += 2;
    if ((x < 0.0f) != (y < 0.0f)) quadrant += 1;
  }

  const Point quadrantPoints[8] = {
      Point::fromXY(1.0f, 0.0f),  Point::fromXY(1.0f, 1.0f),  Point::fromXY(0.0f, 1.0f),
      Point::fromXY(-1.0f, 1.0f), Point::fromXY(-1.0f, 0.0f), Point::fromXY(-1.0f, -1.0f),
      Point::fromXY(0.0f, -1.0f), Point::fromXY(1.0f, -1.0f),
  };

  constexpr float kQuadrantWeight = kScalarRoot2Over2;

  std::size_t conicCount = quadrant;
  for (std::size_t i = 0; i < conicCount; ++i) {
    dst[i] = Conic::fromPoints(&quadrantPoints[i * 2], kQuadrantWeight);
  }

  Point finalPt = Point::fromXY(x, y);
  Point lastQ = quadrantPoints[quadrant * 2];
  float dotVal = lastQ.dot(finalPt);

  if (dotVal < 1.0f) {
    Point offCurve = Point::fromXY(lastQ.x + x, lastQ.y + y);
    float cosThetaOver2 = std::sqrt((1.0f + dotVal) / 2.0f);
    offCurve.setLength(1.0f / cosThetaOver2);
    // Check that lastQ and offCurve are not almost equal.
    // almostEqual returns true when the diff vector can't normalize
    // (both components zero or non-finite). We add conic when NOT almostEqual.
    Point diff = Point::fromXY(lastQ.x - offCurve.x, lastQ.y - offCurve.y);
    bool canNormalize =
        std::isfinite(diff.x) && std::isfinite(diff.y) && (diff.x != 0.0f || diff.y != 0.0f);
    if (canNormalize) {
      dst[conicCount] = Conic::create(lastQ, offCurve, finalPt, cosThetaOver2);
      conicCount++;
    }
  }

  // Compose transform: rotate by uStart, optionally pre-scale for CCW,
  // then post-concat user transform.
  auto transform = Transform::fromRow(uStart.x, uStart.y, -uStart.y, uStart.x, 0.0f, 0.0f);
  if (dir == PathDirection::CCW) {
    transform = transform.preScale(1.0f, -1.0f);
  }
  transform = transform.postConcat(userTransform);

  for (std::size_t i = 0; i < conicCount; ++i) {
    transform.mapPoints(dst[i].points);
  }

  if (conicCount == 0) return std::nullopt;
  return std::span<const Conic>(dst, conicCount);
}

std::optional<AutoConicToQuads> autoConicToQuads(Point pt0, Point pt1, Point pt2, float weight) {
  return autoConicToQuads(pt0, pt1, pt2, weight, 0.25f);
}

std::optional<AutoConicToQuads> autoConicToQuads(Point pt0, Point pt1, Point pt2, float weight,
                                                 float tolerance) {
  Conic conic = Conic::create(pt0, pt1, pt2, weight);
  auto pow2 = conic.computeQuadPow2(tolerance);
  if (!pow2.has_value()) return std::nullopt;
  AutoConicToQuads result;
  result.len = conic.chopIntoQuadsPow2(*pow2, result.points);
  return result;
}

}  // namespace tiny_skia::pathGeometry
