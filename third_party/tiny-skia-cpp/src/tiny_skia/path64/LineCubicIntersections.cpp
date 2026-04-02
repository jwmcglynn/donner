#include "tiny_skia/path64/LineCubicIntersections.h"

#include <span>

#include "tiny_skia/path64/Path64.h"

namespace tiny_skia::path64::line_cubic_intersections {

std::size_t horizontalIntersect(const cubic64::Cubic64& cubic, double axisIntercept,
                                std::array<double, 3>& roots) {
  auto coeffs = cubic.asF64Slice();
  auto coords = std::span<const double>(coeffs.data() + 1, 7);
  auto [a, b, c, d] = cubic64::coefficients(coords);
  const auto result = cubic64::rootsValidT(a, b, c, d - axisIntercept, roots);
  auto index = std::size_t{0};
  while (index < result) {
    const auto calcPt = cubic.pointAtT(roots[index]);
    if (!approximatelyEqual(calcPt.y, axisIntercept)) {
      std::array<double, 6> extremeTs{};
      const auto extrema = cubic64::findExtrema(coords, extremeTs);
      return cubic.searchRoots(extrema, axisIntercept, SearchAxis::Y, extremeTs, roots);
    }
    ++index;
  }

  return result;
}

std::size_t verticalIntersect(const cubic64::Cubic64& cubic, double axisIntercept,
                              std::array<double, 3>& roots) {
  auto coeffs = cubic.asF64Slice();
  const auto [a, b, c, d] = cubic64::coefficients(coeffs);
  const auto result = cubic64::rootsValidT(a, b, c, d - axisIntercept, roots);
  auto index = std::size_t{0};
  while (index < result) {
    const auto calcPt = cubic.pointAtT(roots[index]);
    if (!approximatelyEqual(calcPt.x, axisIntercept)) {
      std::array<double, 6> extremeTs{};
      const auto extrema = cubic64::findExtrema(coeffs, extremeTs);
      return cubic.searchRoots(extrema, axisIntercept, SearchAxis::X, extremeTs, roots);
    }
    ++index;
  }

  return result;
}

}  // namespace tiny_skia::path64::line_cubic_intersections
