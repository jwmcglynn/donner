#include "tiny_skia/path64/Quad64.h"

#include <algorithm>

#include "tiny_skia/path64/Scalar64.h"

namespace tiny_skia::path64 {

namespace {

std::size_t handleZero(double b, double c, std::array<double, 3>& s) {
  if (scalar64::approximatelyZero(b)) {
    s[0] = 0.0;
    return static_cast<std::size_t>(c == 0.0);
  }
  s[0] = -c / b;
  return 1;
}

}  // namespace

namespace quad64 {

std::size_t pushValidTs(const std::array<double, 3>& s, std::size_t realRoots,
                        std::array<double, 3>& t) {
  std::size_t foundRoots = 0;
  for (std::size_t i = 0; i < realRoots; ++i) {
    auto value = s[i];
    if (scalar64::approximatelyZeroOrMore(value) && scalar64::approximatelyOneOrLess(value)) {
      value = scalar64::bound(0.0, value, 1.0);
      bool duplicate = false;
      for (std::size_t j = 0; j < foundRoots; ++j) {
        if (scalar64::approximatelyEqual(t[j], value)) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        t[foundRoots++] = value;
      }
    }
  }
  return foundRoots;
}

std::size_t rootsValidT(double a, double b, double c, std::array<double, 3>& t) {
  auto s = std::array<double, 3>{};
  const auto rootCount = rootsReal(a, b, c, s);
  return pushValidTs(s, rootCount, t);
}

std::size_t rootsReal(double a, double b, double c, std::array<double, 3>& s) {
  if (a == 0.0) {
    return handleZero(b, c, s);
  }

  const auto p = b / (2.0 * a);
  const auto q = c / a;
  if (scalar64::approximatelyZero(a) &&
      (scalar64::approximatelyZeroInverse(p) || scalar64::approximatelyZeroInverse(q))) {
    return handleZero(b, c, s);
  }

  const auto p2 = p * p;
  if (!scalar64::almostDequalUlps(p2, q) && p2 < q) {
    return 0;
  }

  auto sqrtD = 0.0;
  if (p2 > q) {
    sqrtD = std::sqrt(p2 - q);
  }

  s[0] = sqrtD - p;
  s[1] = -sqrtD - p;
  return 1 + static_cast<std::size_t>(!scalar64::almostDequalUlps(s[0], s[1]));
}

}  // namespace quad64
}  // namespace tiny_skia::path64
