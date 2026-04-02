#include "tiny_skia/path64/Cubic64.h"

#include <algorithm>
#include <cassert>

#include "tiny_skia/path64/Path64.h"
#include "tiny_skia/path64/Quad64.h"

namespace tiny_skia::path64::cubic64 {

namespace {

constexpr double kPi = 3.141592653589793;

std::size_t rootsReal(double a, double b, double c, double d, std::array<double, 3>& s) {
  if (approximatelyZero(a) && approximatelyZeroWhenComparedTo(a, b) &&
      approximatelyZeroWhenComparedTo(a, c) && approximatelyZeroWhenComparedTo(a, d)) {
    // we're just a quadratic
    return quad64::rootsReal(b, c, d, s);
  }

  if (approximatelyZeroWhenComparedTo(d, a) && approximatelyZeroWhenComparedTo(d, b) &&
      approximatelyZeroWhenComparedTo(d, c)) {
    // 0 is one root
    auto count = quad64::rootsReal(a, b, c, s);
    for (std::size_t i = 0; i < count; ++i) {
      if (approximatelyZero(s[i])) {
        return count;
      }
    }

    s[count] = 0.0;
    return count + 1;
  }

  if (approximatelyZero(a + b + c + d)) {
    // 1 is one root
    auto count = quad64::rootsReal(a, a + b, -d, s);
    for (std::size_t i = 0; i < count; ++i) {
      if (almostDequalUlps(s[i], 1.0)) {
        return count;
      }
    }
    s[count] = 1.0;
    return count + 1;
  }

  const auto invA = 1.0 / a;
  const auto aa = b * invA;
  const auto bb = c * invA;
  const auto cc = d * invA;
  const auto aa2 = aa * aa;
  const auto q = (aa2 - bb * 3.0) / 9.0;
  const auto r = (2.0 * aa2 * aa - 9.0 * aa * bb + 27.0 * cc) / 54.0;
  const auto r2 = r * r;
  const auto q3 = q * q * q;
  const auto r2MinusQ3 = r2 - q3;
  const auto aDiv3 = aa / 3.0;
  std::size_t offset = 0;

  if (r2MinusQ3 < 0.0) {
    // we have 3 real roots
    const auto theta = std::acos(bound(-1.0, r / std::sqrt(q3), 1.0));
    const auto neg2RootQ = -2.0 * std::sqrt(q);

    auto root = neg2RootQ * std::cos(theta / 3.0) - aDiv3;
    s[offset] = root;
    ++offset;

    root = neg2RootQ * std::cos((theta + 2.0 * kPi) / 3.0) - aDiv3;
    if (!almostDequalUlps(s[0], root)) {
      s[offset] = root;
      ++offset;
    }

    root = neg2RootQ * std::cos((theta - 2.0 * kPi) / 3.0) - aDiv3;
    if (!almostDequalUlps(s[0], root) && (offset == 1 || !almostDequalUlps(s[1], root))) {
      s[offset] = root;
      ++offset;
    }
  } else {
    // we have 1 real root
    auto aRoot = std::sqrt(r2MinusQ3) + std::abs(r);
    aRoot = cubeRoot(aRoot);
    if (r > 0.0) {
      aRoot = -aRoot;
    }

    if (aRoot != 0.0) {
      aRoot += q / aRoot;
    }

    auto root = aRoot - aDiv3;
    s[offset] = root;
    ++offset;
    if (almostDequalUlps(root, q3)) {
      root = -aRoot / 2.0 - aDiv3;
      if (!almostDequalUlps(s[0], root)) {
        s[offset] = root;
        ++offset;
      }
    }
  }

  return offset;
}

void interpCubicCoordsX(const std::array<Point64, 4>& src, double t, std::array<Point64, 7>& dst) {
  const auto ab = interp(src[0].x, src[1].x, t);
  const auto bc = interp(src[1].x, src[2].x, t);
  const auto cd = interp(src[2].x, src[3].x, t);
  const auto abc = interp(ab, bc, t);
  const auto bcd = interp(bc, cd, t);
  const auto abcd = interp(abc, bcd, t);

  dst[0].x = src[0].x;
  dst[1].x = ab;
  dst[2].x = abc;
  dst[3].x = abcd;
  dst[4].x = bcd;
  dst[5].x = cd;
  dst[6].x = src[3].x;
}

void interpCubicCoordsY(const std::array<Point64, 4>& src, double t, std::array<Point64, 7>& dst) {
  const auto ab = interp(src[0].y, src[1].y, t);
  const auto bc = interp(src[1].y, src[2].y, t);
  const auto cd = interp(src[2].y, src[3].y, t);
  const auto abc = interp(ab, bc, t);
  const auto bcd = interp(bc, cd, t);
  const auto abcd = interp(abc, bcd, t);

  dst[0].y = src[0].y;
  dst[1].y = ab;
  dst[2].y = abc;
  dst[3].y = abcd;
  dst[4].y = bcd;
  dst[5].y = cd;
  dst[6].y = src[3].y;
}

}  // namespace

Cubic64 Cubic64::create(std::array<Point64, kPointCount> points) { return Cubic64{points}; }

std::array<double, kPointCount * 2> Cubic64::asF64Slice() const {
  return {
      points[0].x, points[0].y, points[1].x, points[1].y,
      points[2].x, points[2].y, points[3].x, points[3].y,
  };
}

Point64 Cubic64::pointAtT(double t) const {
  if (t == 0.0) {
    return points[0];
  }
  if (t == 1.0) {
    return points[3];
  }

  const auto oneT = 1.0 - t;
  const auto oneT2 = oneT * oneT;
  const auto a = oneT2 * oneT;
  const auto b = 3.0 * oneT2 * t;
  const auto t2 = t * t;
  const auto c = 3.0 * oneT * t2;
  const auto d = t2 * t;
  return Point64::fromXY(a * points[0].x + b * points[1].x + c * points[2].x + d * points[3].x,
                         a * points[0].y + b * points[1].y + c * points[2].y + d * points[3].y);
}

std::size_t Cubic64::searchRoots(std::size_t extrema, double axisIntercept, SearchAxis axis,
                                 std::array<double, 6>& extremeTs,
                                 std::array<double, 3>& validRoots) const {
  extrema += findInflections(
      std::span<double>(extremeTs.begin() + static_cast<long>(extrema), extremeTs.end()));
  extremeTs[extrema] = 0.0;
  ++extrema;
  extremeTs[extrema] = 1.0;
  ++extrema;
  assert(extrema < extremeTs.size());

  std::sort(extremeTs.begin(), extremeTs.begin() + static_cast<long>(extrema),
            [](double a, double b) { return a < b; });

  std::size_t validCount = 0;
  std::size_t index = 0;
  while (index + 1 < extrema) {
    const auto min = extremeTs[index];
    ++index;
    const auto max = extremeTs[index];
    if (min == max) {
      continue;
    }

    const auto newT = binarySearch(min, max, axisIntercept, axis);
    if (newT >= 0.0) {
      if (validCount >= 3) {
        return 0;
      }
      validRoots[validCount] = newT;
      ++validCount;
    }
  }

  return validCount;
}

std::size_t Cubic64::findInflections(std::span<double> tValues) const {
  const auto ax = points[1].x - points[0].x;
  const auto ay = points[1].y - points[0].y;
  const auto bx = points[2].x - 2.0 * points[1].x + points[0].x;
  const auto by = points[2].y - 2.0 * points[1].y + points[0].y;
  const auto cx = points[3].x + 3.0 * (points[1].x - points[2].x) - points[0].x;
  const auto cy = points[3].y + 3.0 * (points[1].y - points[2].y) - points[0].y;

  auto values = std::array<double, 3>{};
  const auto count =
      quad64::rootsValidT(bx * cy - by * cx, ax * cy - ay * cx, ax * by - ay * bx, values);
  std::copy(values.begin(), values.begin() + static_cast<long>(count), tValues.begin());
  return count;
}

double Cubic64::binarySearch(double min, double max, double axisIntercept, SearchAxis axis) const {
  auto t = (min + max) / 2.0;
  auto step = (t - min) / 2.0;
  auto cubicAtT = pointAtT(t);
  auto calcPos = cubicAtT.axisCoord(axis);
  auto calcDist = calcPos - axisIntercept;
  if (approximatelyEqual(calcPos, axisIntercept)) {
    return t;
  }
  while (true) {
    const auto priorT = std::max(min, t - step);
    const auto lessPt = pointAtT(priorT);
    if (approximatelyEqualHalf(lessPt.x, cubicAtT.x) &&
        approximatelyEqualHalf(lessPt.y, cubicAtT.y)) {
      return -1.0;
    }

    const auto lessDist = lessPt.axisCoord(axis) - axisIntercept;
    const auto lastStep = step;
    step /= 2.0;
    auto ok = calcDist > 0.0 ? calcDist > lessDist : calcDist < lessDist;
    if (ok) {
      t = priorT;
    } else {
      const auto nextT = t + lastStep;
      if (nextT > max) {
        return -1.0;
      }

      const auto morePt = pointAtT(nextT);
      if (approximatelyEqualHalf(morePt.x, cubicAtT.x) &&
          approximatelyEqualHalf(morePt.y, cubicAtT.y)) {
        return -1.0;
      }

      const auto moreDist = morePt.axisCoord(axis) - axisIntercept;
      ok = calcDist > 0.0 ? calcDist <= moreDist : calcDist >= moreDist;
      if (ok) {
        continue;
      }
      t = nextT;
    }

    const auto testAtT = pointAtT(t);
    cubicAtT = testAtT;
    calcPos = cubicAtT.axisCoord(axis);
    calcDist = calcPos - axisIntercept;
    if (approximatelyEqual(calcPos, axisIntercept)) {
      break;
    }
  }

  return t;
}

Cubic64Pair Cubic64::chopAt(double t) const {
  auto dst = std::array<Point64, 7>{};
  if (t == 0.5) {
    dst[0] = points[0];
    dst[1].x = (points[0].x + points[1].x) / 2.0;
    dst[1].y = (points[0].y + points[1].y) / 2.0;
    dst[2].x = (points[0].x + 2.0 * points[1].x + points[2].x) / 4.0;
    dst[2].y = (points[0].y + 2.0 * points[1].y + points[2].y) / 4.0;
    dst[3].x = (points[0].x + 3.0 * (points[1].x + points[2].x) + points[3].x) / 8.0;
    dst[3].y = (points[0].y + 3.0 * (points[1].y + points[2].y) + points[3].y) / 8.0;
    dst[4].x = (points[1].x + 2.0 * points[2].x + points[3].x) / 4.0;
    dst[4].y = (points[1].y + 2.0 * points[2].y + points[3].y) / 4.0;
    dst[5].x = (points[2].x + points[3].x) / 2.0;
    dst[5].y = (points[2].y + points[3].y) / 2.0;
    dst[6] = points[3];
    return Cubic64Pair{dst};
  }

  interpCubicCoordsX(points, t, dst);
  interpCubicCoordsY(points, t, dst);
  return Cubic64Pair{dst};
}

std::array<double, 4> coefficients(std::span<const double> src) {
  auto a = src[6];
  auto b = src[4] * 3.0;
  auto c = src[2] * 3.0;
  const auto d = src[0];
  a -= d - c + b;
  b += 3.0 * d - 2.0 * c;
  c -= 3.0 * d;
  return {a, b, c, d};
}

std::size_t rootsValidT(double a, double b, double c, double d, std::array<double, 3>& t) {
  auto s = std::array<double, 3>{};
  const auto realRoots = rootsReal(a, b, c, d, s);
  auto foundRoots = quad64::pushValidTs(s, realRoots, t);

  for (std::size_t index = 0; index < realRoots; ++index) {
    const auto value = s[index];
    if (!approximatelyOneOrLess(value) && between(value, 1.0, 1.00005)) {
      for (std::size_t idx = 0; idx < foundRoots; ++idx) {
        if (approximatelyEqual(t[idx], 1.0)) {
          goto outer_continue;
        }
      }
      assert(foundRoots < 3);
      t[foundRoots] = 1.0;
      ++foundRoots;
    } else if (!approximatelyZeroOrMore(value) && between(value, -0.00005, 0.0)) {
      for (std::size_t idx = 0; idx < foundRoots; ++idx) {
        if (approximatelyEqual(t[idx], 0.0)) {
          goto outer_continue;
        }
      }
      assert(foundRoots < 3);
      t[foundRoots] = 0.0;
      ++foundRoots;
    }
  outer_continue:;
  }

  return foundRoots;
}

std::size_t findExtrema(std::span<const double> src, std::array<double, 6>& tValues) {
  const auto a = src[0];
  const auto b = src[2];
  const auto c = src[4];
  const auto d = src[6];
  const auto a2 = d - a + 3.0 * (b - c);
  const auto b2 = 2.0 * (a - b - b + c);
  const auto c2 = b - a;
  auto values = std::array<double, 3>{};
  const auto count = quad64::rootsValidT(a2, b2, c2, values);
  std::copy(values.begin(), values.begin() + static_cast<long>(count), tValues.begin());
  return count;
}

}  // namespace tiny_skia::path64::cubic64
