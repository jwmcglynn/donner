#include "donner/svg/renderer/geode/GeodeAnchoredSampling.h"

#include <cmath>
#include <limits>

namespace donner::geode {

namespace {

/// Distance from a whole coordinate below which a position counts as on it. Two renders that differ
/// by a whole-pixel move compute the same position with different double-precision rounding, so a
/// position exactly on a pixel or texel boundary must not fall to either side of it.
constexpr double kWholeCoordinateSnap = 1e-9;

/// Splits \p coordinate into a whole part and a fraction in [0, 1), or returns false when the whole
/// part does not fit an int.
bool SplitTexelCoordinate(double coordinate, int& whole, float& fraction) {
  double floored = std::floor(coordinate);
  double remainder = coordinate - floored;
  if (remainder > 1.0 - kWholeCoordinateSnap) {
    floored += 1.0;
    remainder = 0.0;
  } else if (remainder < kWholeCoordinateSnap) {
    remainder = 0.0;
  }
  if (!(floored >= static_cast<double>(std::numeric_limits<int>::min()) &&
        floored <= static_cast<double>(std::numeric_limits<int>::max()))) {
    return false;
  }
  whole = static_cast<int>(floored);
  fraction = static_cast<float>(remainder);
  if (fraction >= 1.0f) {
    // A remainder just below one can round up to one in single precision.
    whole += 1;
    fraction = 0.0f;
  }
  return true;
}

}  // namespace

Vector2i AnchorPixelFor(const Vector2d& position) {
  const auto pixel = [](double coordinate) {
    const double floored = std::floor(coordinate + kWholeCoordinateSnap);
    if (!(floored >= static_cast<double>(std::numeric_limits<int>::min()) &&
          floored <= static_cast<double>(std::numeric_limits<int>::max()))) {
      return 0;
    }
    return static_cast<int>(floored);
  };
  return Vector2i(pixel(position.x), pixel(position.y));
}

std::optional<AnchoredTexelRows> AnchoredTexelRowsFor(const Transform2d& targetFromTexel,
                                                      const Vector2i& anchor) {
  const double det = targetFromTexel.determinant();
  if (!std::isfinite(det) || det == 0.0) {
    return std::nullopt;
  }
  const Transform2d texelFromTarget = targetFromTexel.inverse();
  const Vector2d anchorTexel = texelFromTarget.transformPosition(Vector2d(anchor.x, anchor.y));

  AnchoredTexelRows rows;
  // Transform2d stores [a b c d e f] with x' = a*x + c*y + e and y' = b*x + d*y + f.
  rows.linear = {
      static_cast<float>(texelFromTarget.data[0]), static_cast<float>(texelFromTarget.data[2]),
      static_cast<float>(texelFromTarget.data[1]), static_cast<float>(texelFromTarget.data[3])};
  for (const float term : rows.linear) {
    if (!std::isfinite(term)) {
      return std::nullopt;
    }
  }
  if (!SplitTexelCoordinate(anchorTexel.x, rows.whole.x, rows.fraction[0]) ||
      !SplitTexelCoordinate(anchorTexel.y, rows.whole.y, rows.fraction[1])) {
    return std::nullopt;
  }
  rows.anchor = anchor;
  return rows;
}

}  // namespace donner::geode
