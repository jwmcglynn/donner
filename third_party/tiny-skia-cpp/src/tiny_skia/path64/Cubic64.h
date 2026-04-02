#pragma once

#include <array>
#include <span>

#include "tiny_skia/path64/Point64.h"

namespace tiny_skia::path64::cubic64 {

constexpr int kPointCount = 4;

struct Cubic64Pair {
  std::array<Point64, 7> points{};
};

struct Cubic64 {
  std::array<Point64, kPointCount> points{};

  static Cubic64 create(std::array<Point64, kPointCount> points);

  std::array<double, kPointCount * 2> asF64Slice() const;
  Point64 pointAtT(double t) const;
  std::size_t searchRoots(std::size_t extrema, double axisIntercept, SearchAxis axis,
                          std::array<double, 6>& extremeTs,
                          std::array<double, 3>& validRoots) const;
  std::size_t findInflections(std::span<double> tValues) const;
  Cubic64Pair chopAt(double t) const;

 private:
  double binarySearch(double min, double max, double axisIntercept, SearchAxis axis) const;
};

std::array<double, 4> coefficients(std::span<const double> src);
std::size_t rootsValidT(double a, double b, double c, double d, std::array<double, 3>& t);
std::size_t findExtrema(std::span<const double> src, std::array<double, 6>& tValues);

}  // namespace tiny_skia::path64::cubic64
