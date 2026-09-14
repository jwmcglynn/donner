#include "donner/svg/renderer/geode/GeodeStrokeTolerance.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string>

#include "absl/numeric/int128.h"
#include "donner/base/Box.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/base/Utils.h"
#include "donner/base/Vector2.h"

namespace donner::geode {
namespace {

const Vector2d kCenter = Vector2d(40.0, 40.0);
constexpr double kRadius = 16.0;
constexpr double kStrokeWidth = 3.0;

/// A circle built the way `PathBuilder::addCircle` builds one: four kappa
/// cubics. This is the exact shape the editor's selection chrome draws, and the
/// shape that showed as a visible polygon before stroke flattening became
/// device-aware.
Path Circle() {
  return PathBuilder().addCircle(kCenter, kRadius).build();
}

StrokeStyle CircleStroke() {
  StrokeStyle style;
  style.width = kStrokeWidth;
  return style;
}

/// One edge of a flattened outline polygon.
struct Segment {
  Vector2d from;
  Vector2d to;
};

/// Expand a `strokeToFill` result (MoveTo / LineTo / ClosePath only) into its
/// polygon edges, including the implicit closing edge of each subpath.
std::vector<Segment> OutlineSegments(const Path& outline) {
  std::vector<Segment> segments;
  Vector2d subpathStart;
  Vector2d previous;
  bool open = false;
  for (const Path::Command& command : outline.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo:
        subpathStart = outline.points()[command.pointIndex];
        previous = subpathStart;
        open = true;
        break;
      case Path::Verb::LineTo: {
        const Vector2d point = outline.points()[command.pointIndex];
        if (open) {
          segments.push_back({previous, point});
        }
        previous = point;
        break;
      }
      case Path::Verb::ClosePath:
        if (open) {
          segments.push_back({previous, subpathStart});
        }
        open = false;
        break;
      default:
        // `strokeToFill` never emits curve verbs; a curve here would mean the
        // stroker stopped flattening, which the assertions below would miss.
        ADD_FAILURE() << "Stroke outline contained an unexpected curve verb.";
        break;
    }
  }
  return segments;
}

using absl::int128;
using absl::uint128;

constexpr int kCoordinateFractionBits = 52;
constexpr double kMaximumCoordinate = 64.0;
constexpr size_t kMaximumOutlineEdges = 50000;

int Sign(int128 value) {
  return (value > 0) - (value < 0);
}

/// Continued-fraction comparison avoids a double-width cross product.
int ComparePositiveFractions(uint128 a, uint128 b, uint128 c, uint128 d) {
  bool inverted = false;
  for (int step = 0; step < 256; ++step) {
    const uint128 left = a / b;
    const uint128 right = c / d;
    if (left != right) {
      const int order = (left > right) ? 1 : -1;
      return inverted ? -order : order;
    }
    a -= left * b;
    c -= right * d;
    if (a == 0 || c == 0) {
      const int order = int(a != 0) - int(c != 0);
      return inverted ? -order : order;
    }
    std::swap(a, b);
    std::swap(c, d);
    inverted = !inverted;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "128-bit Euclidean comparison did not terminate");
  return 0;
}

int TrailingZeroBits(uint128 value) {
  UTILS_RELEASE_ASSERT(value != 0);
  const uint64_t low = absl::Uint128Low64(value);
  return low != 0 ? std::countr_zero(low) : 64 + std::countr_zero(absl::Uint128High64(value));
}

/// An exact signed ratio; denominators are always positive.
struct Fraction {
  int128 numerator = 0;
  uint128 denominator = 1;

  Fraction() = default;
  Fraction(int128 value, int128 divisor) {
    UTILS_RELEASE_ASSERT(divisor != 0);
    if (value == 0) {
      return;
    }
    const bool negative = (value < 0) != (divisor < 0);
    uint128 magnitude = uint128(value < 0 ? -value : value);
    denominator = uint128(divisor < 0 ? -divisor : divisor);
    const int shift = std::min(TrailingZeroBits(magnitude), TrailingZeroBits(denominator));
    magnitude >>= shift;
    denominator >>= shift;
    numerator = negative ? -int128(magnitude) : int128(magnitude);
  }

  std::strong_ordering operator<=>(const Fraction& other) const {
    if (denominator == other.denominator) {
      return numerator == other.numerator  ? std::strong_ordering::equal
             : numerator < other.numerator ? std::strong_ordering::less
                                           : std::strong_ordering::greater;
    }
    const int leftSign = Sign(numerator);
    const int rightSign = Sign(other.numerator);
    if (leftSign != rightSign) {
      return leftSign <=> rightSign;
    }
    const uint128 left = uint128(leftSign < 0 ? -numerator : numerator);
    const uint128 right = uint128(rightSign < 0 ? -other.numerator : other.numerator);
    const int magnitude = ComparePositiveFractions(left, denominator, right, other.denominator);
    return (leftSign < 0 ? -magnitude : magnitude) <=> 0;
  }

  bool operator==(const Fraction& other) const { return (*this <=> other) == 0; }
};

std::ostream& operator<<(std::ostream& output, const Fraction& value) {
  return output << value.numerator << '/' << value.denominator;
}

struct ExactPoint {
  int64_t x = 0;
  int64_t y = 0;

  auto operator<=>(const ExactPoint&) const = default;
  bool operator==(const ExactPoint&) const = default;
};

ExactPoint operator-(ExactPoint left, ExactPoint right) {
  return {left.x - right.x, left.y - right.y};
}

/// Reject values outside the bounded dyadic domain instead of snapping them.
std::optional<int64_t> ExactCoordinate(double value) {
  if (!std::isfinite(value) || std::abs(value) > kMaximumCoordinate) {
    return std::nullopt;
  }
  const double scaled = std::ldexp(value, kCoordinateFractionBits);
  if (std::trunc(scaled) != scaled) {
    return std::nullopt;
  }
  return static_cast<int64_t>(scaled);
}

std::optional<ExactPoint> ExactCoordinates(Vector2d value) {
  const auto x = ExactCoordinate(value.x);
  const auto y = ExactCoordinate(value.y);
  if (!x || !y) {
    return std::nullopt;
  }
  return ExactPoint{*x, *y};
}

/// Admitted coordinate differences need at most 120 determinant magnitude bits.
int128 Cross(ExactPoint left, ExactPoint right) {
  return int128(left.x) * right.y - int128(left.y) * right.x;
}

struct ExactSegment {
  ExactPoint from;
  ExactPoint to;
};

struct ExactInterval {
  Fraction low;
  Fraction high;

  bool operator==(const ExactInterval&) const = default;
};

std::ostream& operator<<(std::ostream& output, const ExactInterval& interval) {
  return output << '[' << interval.low << ", " << interval.high << ']';
}

using Boundary = std::vector<ExactInterval>;

Boundary MergeIntervals(Boundary intervals) {
  std::sort(intervals.begin(), intervals.end(), [](const auto& left, const auto& right) {
    return left.low != right.low ? left.low < right.low : left.high < right.high;
  });
  Boundary result;
  for (const ExactInterval& interval : intervals) {
    UTILS_RELEASE_ASSERT(interval.low <= interval.high);
    if (result.empty() || result.back().high < interval.low) {
      result.push_back(interval);
    } else {
      result.back().high = std::max(result.back().high, interval.high);
    }
  }
  return result;
}

struct RayEvent {
  Fraction position;
  Fraction shift;
  int delta;
};

struct RaySlice {
  Boundary filled;
  Boundary transitions;
};

/// Sort the position and infinitesimal shift before projecting tangent transitions.
RaySlice SweepEvents(std::vector<RayEvent> events) {
  std::sort(events.begin(), events.end(), [](const RayEvent& left, const RayEvent& right) {
    return left.position != right.position ? left.position < right.position
                                           : left.shift < right.shift;
  });
  RaySlice result;
  int winding = 0;
  Fraction previous;
  size_t index = 0;
  while (index < events.size()) {
    const RayEvent current = events[index];
    if (winding != 0) {
      result.filled.push_back({previous, current.position});
    }
    const bool before = winding != 0;
    do {
      winding += events[index++].delta;
    } while (index < events.size() && events[index].position == current.position &&
             events[index].shift == current.shift);
    if (before != (winding != 0)) {
      result.transitions.push_back({current.position, current.position});
    }
    previous = current.position;
  }
  UTILS_RELEASE_ASSERT_MSG(winding == 0, "Closed outline left an unbounded filled interval");
  result.filled = MergeIntervals(std::move(result.filled));
  return result;
}

struct IntervalEvent {
  Fraction position;
  int side;
  int delta;
};

Boundary SymmetricDifference(const Boundary& left, const Boundary& right) {
  std::vector<IntervalEvent> events;
  const std::array<const Boundary*, 2> inputs = {&left, &right};
  for (int side = 0; side < 2; ++side) {
    for (const ExactInterval& interval : *inputs[side]) {
      events.push_back({interval.low, side, 1});
      events.push_back({interval.high, side, -1});
    }
  }
  std::sort(events.begin(), events.end(),
            [](const auto& a, const auto& b) { return a.position < b.position; });
  Boundary result;
  std::array<int, 2> depth = {};
  Fraction previous;
  size_t index = 0;
  while (index < events.size()) {
    const Fraction position = events[index].position;
    if ((depth[0] != 0) != (depth[1] != 0)) {
      result.push_back({previous, position});
    }
    do {
      const IntervalEvent& event = events[index++];
      depth[event.side] += event.delta;
    } while (index < events.size() && events[index].position == position);
    previous = position;
  }
  UTILS_RELEASE_ASSERT(depth[0] == 0 && depth[1] == 0);
  return result;
}

struct Angle {
  ExactPoint direction{1, 0};
  bool end = false;

  std::strong_ordering operator<=>(const Angle& other) const {
    if (end != other.end) {
      return end <=> other.end;
    }
    return end ? std::strong_ordering::equal : (-Sign(Cross(direction, other.direction))) <=> 0;
  }

  bool operator==(const Angle& other) const { return (*this <=> other) == 0; }
};

bool FlipIntoUpperHalfPlane(ExactPoint& direction) {
  const bool flipped = direction.y < 0 || (direction.y == 0 && direction.x < 0);
  if (flipped) {
    direction.x = -direction.x;
    direction.y = -direction.y;
  }
  return flipped;
}

struct IndexedOutline {
  struct AngularInterval {
    Angle low;
    Angle high;
    size_t segment;
  };

  std::vector<ExactSegment> segments;
  std::vector<AngularInterval> intervals;
  std::vector<Angle> maximumEnds;
  std::vector<size_t> alwaysCandidates;
  ExactPoint center;
  bool valid = true;

  explicit IndexedOutline(const Path& path, Vector2d origin = kCenter) {
    const auto exactCenter = ExactCoordinates(origin);
    const std::vector<Segment> raw = OutlineSegments(path);
    if (!exactCenter || raw.empty() || raw.size() > kMaximumOutlineEdges) {
      valid = false;
      return;
    }
    center = *exactCenter;
    for (const Segment& segment : raw) {
      const auto from = ExactCoordinates(segment.from);
      const auto to = ExactCoordinates(segment.to);
      if (!from || !to) {
        valid = false;
        return;
      }
      segments.push_back({*from, *to});
      indexSegment(segments.size() - 1);
    }
    std::sort(intervals.begin(), intervals.end(), [](const auto& left, const auto& right) {
      return left.low != right.low ? left.low < right.low : left.high < right.high;
    });
    maximumEnds.resize(4 * intervals.size());
    if (!intervals.empty()) {
      buildMaximumEnds(0, 0, intervals.size());
    }
  }

  /// Equal endpoint flips select the intervening arc; different flips select its complement.
  void indexSegment(size_t index) {
    ExactPoint from = segments[index].from - center;
    ExactPoint to = segments[index].to - center;
    if (from == ExactPoint{} || to == ExactPoint{}) {
      alwaysCandidates.push_back(index);
      return;
    }
    const bool firstFlipped = FlipIntoUpperHalfPlane(from);
    const bool secondFlipped = FlipIntoUpperHalfPlane(to);
    if (Cross(from, to) == 0 && firstFlipped != secondFlipped) {
      alwaysCandidates.push_back(index);
      return;
    }
    const Angle low = std::min(Angle{from}, Angle{to});
    const Angle high = std::max(Angle{from}, Angle{to});
    if (firstFlipped == secondFlipped) {
      intervals.push_back({low, high, index});
    } else {
      intervals.push_back({Angle{}, low, index});
      intervals.push_back({high, Angle{{1, 0}, true}, index});
    }
  }

  void buildMaximumEnds(size_t node, size_t begin, size_t end) {
    if (end - begin == 1) {
      maximumEnds[node] = intervals[begin].high;
      return;
    }
    const size_t middle = begin + (end - begin) / 2;
    buildMaximumEnds(2 * node + 1, begin, middle);
    buildMaximumEnds(2 * node + 2, middle, end);
    maximumEnds[node] = std::max(maximumEnds[2 * node + 1], maximumEnds[2 * node + 2]);
  }

  void queryIntervals(size_t node, size_t begin, size_t end, const Angle& angle,
                      std::vector<size_t>& result) const {
    if (angle < intervals[begin].low || maximumEnds[node] < angle) {
      return;
    }
    if (end - begin == 1) {
      result.push_back(intervals[begin].segment);
      return;
    }
    const size_t middle = begin + (end - begin) / 2;
    queryIntervals(2 * node + 1, begin, middle, angle, result);
    queryIntervals(2 * node + 2, middle, end, angle, result);
  }

  bool query(ExactPoint queryCenter, ExactPoint direction, std::vector<size_t>& result) const {
    result.clear();
    if (!valid || queryCenter != center || direction == ExactPoint{}) {
      return false;
    }
    result = alwaysCandidates;
    FlipIntoUpperHalfPlane(direction);
    if (!intervals.empty()) {
      queryIntervals(0, 0, intervals.size(), Angle{direction}, result);
    }
    return true;
  }
};

/**
 * Cross C+tD+sigma*epsilon*N, where N is perpendicular to D and epsilon tends to zero.
 * The position and shift ratios retain distinct events that approach the same vertex.
 */
void AddRayEvents(const ExactSegment& segment, ExactPoint center, ExactPoint direction,
                  std::array<std::vector<RayEvent>, 2>& events) {
  const ExactPoint edge = segment.to - segment.from;
  const int128 denominator = Cross(direction, edge);
  const int first = Sign(Cross(direction, segment.from - center));
  const int second = Sign(Cross(direction, segment.to - center));
  const ExactPoint normal = {-direction.y, direction.x};
  for (int side = 0; side < 2; ++side) {
    const int sigma = side == 0 ? -1 : 1;
    const int firstSide = first == 0 ? -sigma : first;
    const int secondSide = second == 0 ? -sigma : second;
    if (firstSide == secondSide) {
      continue;
    }
    UTILS_RELEASE_ASSERT(denominator != 0);
    events[side].push_back({Fraction(Cross(segment.from - center, edge), denominator),
                            Fraction(-sigma * Cross(normal, edge), denominator),
                            Sign(denominator)});
  }
}

/**
 * Transition points plus closure(F+ xor F-) include exposed collinear intervals.
 * An internal seam has the same fill on both parallel limits and adds no boundary.
 */
Boundary UnionBoundary(const IndexedOutline& outline, ExactPoint center, ExactPoint direction,
                       bool useIndex = true) {
  UTILS_RELEASE_ASSERT(outline.valid && center == outline.center && direction != ExactPoint{});
  std::vector<size_t> candidates;
  if (useIndex) {
    UTILS_RELEASE_ASSERT(outline.query(center, direction, candidates));
  } else {
    candidates.resize(outline.segments.size());
    std::iota(candidates.begin(), candidates.end(), size_t(0));
  }
  std::array<std::vector<RayEvent>, 2> events;
  for (size_t index : candidates) {
    AddRayEvents(outline.segments[index], center, direction, events);
  }
  const RaySlice left = SweepEvents(std::move(events[0]));
  const RaySlice right = SweepEvents(std::move(events[1]));
  Boundary result = SymmetricDifference(left.filled, right.filled);
  result.insert(result.end(), left.transitions.begin(), left.transitions.end());
  result.insert(result.end(), right.transitions.begin(), right.transitions.end());
  return MergeIntervals(std::move(result));
}

using Real = long double;

Real Up(Real value) {
  return std::nextafter(value, std::numeric_limits<Real>::infinity());
}

Real Down(Real value) {
  return std::nextafter(value, -std::numeric_limits<Real>::infinity());
}

struct FloatInterval {
  Real low;
  Real high;
};

FloatInterval EncloseUint64(uint64_t value) {
  if (value == 0) {
    return {0, 0};
  }
  const Real converted = static_cast<Real>(value);
  return {Down(converted), Up(converted)};
}

/// Enclose each limb conversion and addition; the power-of-two shift is exact.
FloatInterval EncloseUint128(uint128 value) {
  const FloatInterval high = EncloseUint64(absl::Uint128High64(value));
  const FloatInterval low = EncloseUint64(absl::Uint128Low64(value));
  return {Down(std::ldexp(high.low, 64) + low.low), Up(std::ldexp(high.high, 64) + low.high)};
}

FloatInterval EncloseFraction(const Fraction& value) {
  if (value.numerator == 0) {
    return {0, 0};
  }
  const bool negative = value.numerator < 0;
  const FloatInterval numerator =
      EncloseUint128(uint128(negative ? -value.numerator : value.numerator));
  const FloatInterval denominator = EncloseUint128(value.denominator);
  UTILS_RELEASE_ASSERT(denominator.low > 0);
  const FloatInterval quotient = {Down(numerator.low / denominator.high),
                                  Up(numerator.high / denominator.low)};
  return negative ? FloatInterval{-quotient.high, -quotient.low} : quotient;
}

struct ApproximateBoundary {
  std::vector<FloatInterval> intervals;
  Real radius = 0;
};

Real ApproximateEndpoint(const Fraction& value, Real& maximumRadius) {
  const FloatInterval range = EncloseFraction(value);
  const Real center = std::clamp(range.low + (range.high - range.low) / 2, range.low, range.high);
  const Real radius = Up(std::max(center - range.low, range.high - center));
  maximumRadius = std::max(maximumRadius, radius);
  return center;
}

ApproximateBoundary Approximate(const Boundary& boundary) {
  ApproximateBoundary result;
  for (const ExactInterval& interval : boundary) {
    const Real first = ApproximateEndpoint(interval.low, result.radius);
    const Real second = ApproximateEndpoint(interval.high, result.radius);
    result.intervals.push_back({std::min(first, second), std::max(first, second)});
  }
  std::sort(result.intervals.begin(), result.intervals.end(),
            [](const auto& a, const auto& b) { return a.low < b.low; });
  std::vector<FloatInterval> merged;
  for (const FloatInterval& interval : result.intervals) {
    if (merged.empty() || merged.back().high < interval.low) {
      merged.push_back(interval);
    } else {
      merged.back().high = std::max(merged.back().high, interval.high);
    }
  }
  result.intervals = std::move(merged);
  return result;
}

Real DirectedDistance(const std::vector<FloatInterval>& source,
                      const std::vector<FloatInterval>& target) {
  Real worst = 0;
  size_t firstGap = 0;
  for (const FloatInterval& interval : source) {
    worst = std::max(worst, Up(target.front().low - interval.low));
    worst = std::max(worst, Up(interval.high - target.back().high));
    while (firstGap + 1 < target.size() && target[firstGap + 1].low < interval.low) {
      ++firstGap;
    }
    for (size_t gap = firstGap; gap + 1 < target.size(); ++gap) {
      const Real p = target[gap].high;
      const Real q = target[gap + 1].low;
      if (p > interval.high) {
        break;
      }
      const Real low = std::max(p, interval.low);
      const Real high = std::min(q, interval.high);
      if (low <= high) {
        const Real distance = std::min({Up(Up(q - p) / 2), Up(high - p), Up(q - low)});
        worst = std::max(worst, distance);
      }
    }
  }
  return worst;
}

/// Endpoint perturbation bounds the Hausdorff error of the floating interval sets.
Real BoundaryDistance(const Boundary& left, const Boundary& right) {
  if (left == right) {
    return 0;
  }
  if (left.empty() || right.empty()) {
    return std::numeric_limits<Real>::infinity();
  }
  const ApproximateBoundary a = Approximate(left);
  const ApproximateBoundary b = Approximate(right);
  const Real measured = std::max(DirectedDistance(a.intervals, b.intervals),
                                 DirectedDistance(b.intervals, a.intervals));
  return Up(Up(measured + a.radius) + b.radius);
}

/// Outward rounding covers normalization, square root, and device scaling.
Real DirectionScale(ExactPoint direction, double scale) {
  const Real x = EncloseUint64(static_cast<uint64_t>(std::abs(direction.x))).high;
  const Real y = EncloseUint64(static_cast<uint64_t>(std::abs(direction.y))).high;
  const Real norm = Up(std::sqrt(Up(Up(x * x) + Up(y * y))));
  return Up(std::ldexp(norm, -kCoordinateFractionBits) * static_cast<Real>(scale));
}

std::vector<ExactPoint> QueryDirections(const IndexedOutline& actual,
                                        const IndexedOutline& reference, ExactPoint center) {
  std::vector<ExactPoint> result;
  for (const IndexedOutline* outline : {&actual, &reference}) {
    for (const ExactSegment& edge : outline->segments) {
      ExactPoint direction = {edge.from.x + edge.to.x - 2 * center.x,
                              edge.from.y + edge.to.y - 2 * center.y};
      const int64_t divisor = std::gcd(std::abs(direction.x), std::abs(direction.y));
      if (divisor == 0) {
        continue;
      }
      direction.x /= divisor;
      direction.y /= divisor;
      if (direction.x < 0 || (direction.x == 0 && direction.y < 0)) {
        direction.x = -direction.x;
        direction.y = -direction.y;
      }
      result.push_back(direction);
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

/// Samples both union boundaries on the exact midpoint lines of all input facets.
/// The constrained line distance bounds nearest Euclidean boundary distance from above.
double MaxDeviceFacetError(const IndexedOutline& actual, const IndexedOutline& reference,
                           double scale, Vector2d center = kCenter) {
  const auto exactCenter = ExactCoordinates(center);
  if (!actual.valid || !reference.valid || !exactCenter || !std::isfinite(scale) || scale <= 0) {
    ADD_FAILURE() << "Union oracle received unsupported coordinates, geometry or scale";
    return std::numeric_limits<double>::infinity();
  }
  const std::vector<ExactPoint> directions = QueryDirections(actual, reference, *exactCenter);
  if (directions.empty()) {
    ADD_FAILURE() << "Union oracle received no nonzero facet midpoint directions";
    return std::numeric_limits<double>::infinity();
  }
  Real worst = 0;
  for (const ExactPoint direction : directions) {
    const Boundary actualBoundary = UnionBoundary(actual, *exactCenter, direction);
    const Boundary referenceBoundary = UnionBoundary(reference, *exactCenter, direction);
    const Real error = BoundaryDistance(actualBoundary, referenceBoundary);
    if (error != 0) {
      worst = std::max(worst, Up(error * DirectionScale(direction, scale)));
    }
  }
  return worst == 0
             ? 0
             : std::nextafter(static_cast<double>(worst), std::numeric_limits<double>::infinity());
}

double MaxDeviceFacetError(const Path& actual, const Path& reference, double scale,
                           Vector2d center = kCenter) {
  return MaxDeviceFacetError(IndexedOutline(actual, center), IndexedOutline(reference, center),
                             scale, center);
}

/// Converged stroke outline of `Circle()`: flattened an order of magnitude
/// finer than the tightest tolerance the derivation can produce, so it stands in
/// for the exact offset curve.
Path ConvergedCircleOutline() {
  return Circle().strokeToFill(CircleStroke(), kMinStrokeFlattenTolerance * 0.1);
}

/// Immutable process-lifetime reference; no caller mutates its geometry or index.
const IndexedOutline& ConvergedCircleIndex() {
  static const IndexedOutline reference(ConvergedCircleOutline());
  return reference;
}

// ---------------------------------------------------------------------------
// MaxAbsScaleFactor
// ---------------------------------------------------------------------------

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorOfIdentityIsOne) {
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d()), 1.0);
}

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorOfUniformScale) {
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d::Scale(8.0)), 8.0);
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d::Scale(0.25)), 0.25);
}

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorIgnoresTranslation) {
  const Transform2d transform =
      Transform2d::Scale(4.0) * Transform2d::Translate(Vector2d(1000.0, -2000.0));
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(transform), 4.0);
}

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorIsRotationInvariant) {
  const Transform2d transform = Transform2d::Scale(6.0) * Transform2d::Rotate(0.7);
  EXPECT_NEAR(MaxAbsScaleFactor(transform), 6.0, 1e-9);
}

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorTakesTheLargerAxisOfNonUniformScale) {
  // The bound must hold for every direction, so the LARGER axis wins: an error
  // vector aligned with the 32x axis is magnified 32x in device space.
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d::Scale(Vector2d(1.0, 32.0))), 32.0);
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d::Scale(Vector2d(32.0, 1.0))), 32.0);
}

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorOfDegenerateTransformIsZero) {
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d::Scale(0.0)), 0.0);
}

// ---------------------------------------------------------------------------
// StrokeFlattenScaleBucket
// ---------------------------------------------------------------------------

TEST(GeodeStrokeTolerance, ScaleBucketRoundsUpToAPowerOfTwo) {
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(1.0), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(8.0), 8.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(8.5), 16.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(15.9), 16.0);
}

/// Refine-only: minified geometry keeps the path-local default rather than
/// being tessellated more coarsely. Coarsening would stay inside the
/// device-pixel budget but would perturb every downscaled render (filter
/// sub-renders, pattern tiles, thumbnails) for no correctness gain.
TEST(GeodeStrokeTolerance, ScaleBucketFloorsAtOneForMinifiedTransforms) {
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(0.3), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(0.99), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(1e-6), 1.0);
}

TEST(GeodeStrokeTolerance, ScaleBucketIsStableWithinABucket) {
  // The point of bucketing: a continuous zoom gesture must not re-derive (and
  // therefore re-flatten) the stroke outline on every frame.
  const double bucket = StrokeFlattenScaleBucket(9.0);
  for (double scale = 8.01; scale < 16.0; scale += 0.37) {
    EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(scale), bucket) << "scale = " << scale;
  }
}

TEST(GeodeStrokeTolerance, ScaleBucketRejectsDegenerateInput) {
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(0.0), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(-4.0), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(std::numeric_limits<double>::quiet_NaN()), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(std::numeric_limits<double>::infinity()), 1.0);
}

// ---------------------------------------------------------------------------
// StrokeFlattenToleranceFor
// ---------------------------------------------------------------------------

TEST(GeodeStrokeTolerance, ToleranceAtIdentityMatchesTheDevicePixelTarget) {
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d()), kStrokeFlattenDevicePixels);
}

TEST(GeodeStrokeTolerance, ToleranceShrinksInverselyWithScale) {
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d::Scale(8.0)),
                   kStrokeFlattenDevicePixels / 8.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d::Scale(32.0)),
                   kStrokeFlattenDevicePixels / 32.0);
}

TEST(GeodeStrokeTolerance, ToleranceNeverCoarsensBelowTheLocalDefault) {
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d::Scale(0.5)), kStrokeFlattenDevicePixels);
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d::Scale(1e-12)),
                   kStrokeFlattenDevicePixels);
  EXPECT_DOUBLE_EQ(kMaxStrokeFlattenTolerance, Path::kLocalFlattenTolerance);
}

TEST(GeodeStrokeTolerance, ToleranceIsClampedForDegenerateAndExtremeTransforms) {
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d::Scale(0.0)), kStrokeFlattenDevicePixels);
  EXPECT_GE(StrokeFlattenToleranceFor(Transform2d::Scale(1e12)), kMinStrokeFlattenTolerance);
  EXPECT_LE(StrokeFlattenToleranceFor(Transform2d::Scale(1e-12)), kMaxStrokeFlattenTolerance);
}

// ---------------------------------------------------------------------------
// The invariant itself.
// ---------------------------------------------------------------------------

/// The contract every renderer-side stroke call site must satisfy: flattening a
/// curve with the derived tolerance keeps the chord error under
/// `kStrokeFlattenDevicePixels` DEVICE pixels at the transform the geometry is
/// actually drawn with. Curves rendering as visible line segments is a
/// correctness violation, so this bound is a gate, not a quality target.
class GeodeStrokeToleranceScale : public testing::TestWithParam<double> {};

TEST_P(GeodeStrokeToleranceScale, DerivedToleranceBoundsDeviceChordErrorAtEveryScale) {
  const double scale = GetParam();
  const IndexedOutline& reference = ConvergedCircleIndex();
  const double tolerance = StrokeFlattenToleranceFor(Transform2d::Scale(scale));
  const IndexedOutline stroked(Circle().strokeToFill(CircleStroke(), tolerance));
  ASSERT_TRUE(stroked.valid);
  ASSERT_TRUE(reference.valid);
  RecordProperty("reference_segments", static_cast<int>(reference.segments.size()));
  RecordProperty("actual_segments", static_cast<int>(stroked.segments.size()));
  EXPECT_LE(MaxDeviceFacetError(stroked, reference, scale), kStrokeFlattenDevicePixels)
      << "Stroke union faceting exceeded the device-pixel bound at scale " << scale;
}

std::string ScaleName(const testing::TestParamInfo<double>& info) {
  static const std::array<const char*, 6> names = {"Scale1", "Scale1Point7", "Scale4",
                                                   "Scale8", "Scale32",      "Scale512"};
  return names[info.index];
}

INSTANTIATE_TEST_SUITE_P(DeviceScale, GeodeStrokeToleranceScale,
                         testing::Values(1.0, 1.7, 4.0, 8.0, 32.0, 512.0), ScaleName);

TEST(GeodeStrokeTolerance, FacetErrorIsInvariantUnderPositiveRectanglePartition) {
  const Path rectangle = PathBuilder().addRect(Box2d({0, 0}, {4, 2})).build();
  const Path partition =
      PathBuilder().addRect(Box2d({0, 0}, {2, 2})).addRect(Box2d({2, 0}, {4, 2})).build();
  EXPECT_DOUBLE_EQ(MaxDeviceFacetError(rectangle, partition, 1.0), 0.0);
  EXPECT_DOUBLE_EQ(MaxDeviceFacetError(partition, rectangle, 1.0), 0.0);
}

/// Companion to the test above: it records WHY the derivation is needed. A
/// scale-blind path-local tolerance (the historical default) blows the same
/// device-pixel bound wide open once the view is zoomed in, which is exactly
/// how a circle turns into a visible chain of segments on screen.
TEST(GeodeStrokeTolerance, FixedLocalToleranceViolatesTheDeviceBoundWhenZoomed) {
  const IndexedOutline stroked(Circle().strokeToFill(CircleStroke(), Path::kLocalFlattenTolerance));
  ASSERT_TRUE(stroked.valid);
  const double errorAtOne = MaxDeviceFacetError(stroked, ConvergedCircleIndex(), 1.0);
  EXPECT_LE(errorAtOne, kStrokeFlattenDevicePixels);
  EXPECT_GT(errorAtOne * 32.0, kStrokeFlattenDevicePixels * 4.0)
      << "A scale-blind tolerance must still fail the device-pixel bound";
}

/// Finer flattening means more outline points. This is the property the
/// renderer-level counter assertions in `RendererGeode_tests.cc` lean on.
TEST(GeodeStrokeTolerance, DerivedToleranceProducesMoreOutlinePointsAsScaleGrows) {
  const Path circle = Circle();
  const Path atOne = circle.strokeToFill(CircleStroke(), StrokeFlattenToleranceFor(Transform2d()));
  const Path atThirtyTwo =
      circle.strokeToFill(CircleStroke(), StrokeFlattenToleranceFor(Transform2d::Scale(32.0)));

  EXPECT_GT(atThirtyTwo.points().size(), atOne.points().size() * 4u);
}

Path Rectangle(Vector2d low, Vector2d high, bool reversed = false) {
  PathBuilder builder;
  builder.moveTo(low);
  if (reversed) {
    builder.lineTo({low.x, high.y}).lineTo(high).lineTo({high.x, low.y});
  } else {
    builder.lineTo({high.x, low.y}).lineTo(high).lineTo({low.x, high.y});
  }
  return builder.closePath().build();
}

Fraction Position(double value) {
  const auto coordinate = ExactCoordinate(value);
  UTILS_RELEASE_ASSERT(coordinate.has_value());
  return Fraction(*coordinate, 1);
}

ExactInterval PointBoundary(double value) {
  return {Position(value), Position(value)};
}

Boundary HorizontalBoundary(const Path& path, double y) {
  const IndexedOutline outline(path, {0, y});
  UTILS_RELEASE_ASSERT(outline.valid);
  return UnionBoundary(outline, *ExactCoordinates({0, y}), {1, 0});
}

TEST(GeodeStrokeTolerance, ExactFractionOrderingPreservesCloseDistinctValues) {
  const int128 limit = int128(1) << 120;
  EXPECT_EQ(Fraction(2, 4), Fraction(1, 2));
  EXPECT_EQ(Fraction(-2, -4), Fraction(1, 2));
  EXPECT_LT(Fraction(-1, 3), Fraction(0, 1));
  EXPECT_GT(Fraction(limit - 1, limit), Fraction(limit - 2, limit - 1));
  EXPECT_LT(Fraction(-(limit - 1), limit), Fraction(-(limit - 2), limit - 1));
}

TEST(GeodeStrokeTolerance, ExactFractionOrderingHandlesLongEuclideanSequences) {
  int128 previous = 1;
  int128 current = 1;
  while (current < (int128(1) << 117)) {
    const int128 next = previous + current;
    previous = current;
    current = next;
  }
  EXPECT_EQ(Fraction(previous, current), Fraction(2 * previous, 2 * current));
  EXPECT_LT(Fraction(previous, current), Fraction(previous, current - 1));
}

TEST(GeodeStrokeTolerance, ExactCoordinatesRejectUnsupportedInputWithoutRounding) {
  EXPECT_EQ(ExactCoordinate(64), int64_t(1) << 58);
  EXPECT_EQ(ExactCoordinate(std::ldexp(1.0, -52)), 1);
  EXPECT_EQ(ExactCoordinate(std::ldexp(1.0, -53)), std::nullopt);
  EXPECT_EQ(ExactCoordinate(65), std::nullopt);
  EXPECT_EQ(ExactCoordinate(std::numeric_limits<double>::infinity()), std::nullopt);
  EXPECT_EQ(ExactCoordinate(std::numeric_limits<double>::quiet_NaN()), std::nullopt);
}

TEST(GeodeStrokeTolerance, CollinearExposedEdgeRemainsAnInterval) {
  const Path rectangle = Rectangle({0, 0}, {2, 2});
  EXPECT_THAT(HorizontalBoundary(rectangle, 0),
              testing::ElementsAre(ExactInterval{Position(0), Position(2)}));
  EXPECT_THAT(HorizontalBoundary(rectangle, 2),
              testing::ElementsAre(ExactInterval{Position(0), Position(2)}));
}

TEST(GeodeStrokeTolerance, CollinearInternalSeamDoesNotBecomeABoundary) {
  const Path partition =
      PathBuilder().addPath(Rectangle({0, 0}, {2, 1})).addPath(Rectangle({0, 1}, {2, 2})).build();
  EXPECT_THAT(HorizontalBoundary(partition, 1),
              testing::ElementsAre(PointBoundary(0), PointBoundary(2)));
}

TEST(GeodeStrokeTolerance, VertexAndTangentLinesRetainExactBoundaryPoints) {
  const IndexedOutline rectangle(Rectangle({0, 0}, {2, 2}), {});
  EXPECT_THAT(UnionBoundary(rectangle, {}, {1, 1}),
              testing::ElementsAre(PointBoundary(0), PointBoundary(2)));
  EXPECT_THAT(UnionBoundary(rectangle, {}, {1, -1}), testing::ElementsAre(PointBoundary(0)));
}

TEST(GeodeStrokeTolerance, ReversedContoursPreserveEveryBoundary) {
  const Path forward = Rectangle({0, 0}, {4, 2});
  const Path reverse = Rectangle({0, 0}, {4, 2}, true);
  for (const double y : {0.0, 1.0, 2.0}) {
    EXPECT_THAT(HorizontalBoundary(reverse, y),
                testing::ElementsAreArray(HorizontalBoundary(forward, y)));
  }
  EXPECT_DOUBLE_EQ(MaxDeviceFacetError(forward, reverse, 1, {2, 1}), 0);
}

TEST(GeodeStrokeTolerance, HoleBoundariesAndCollinearHoleSidesRemainVisible) {
  const Path ring = PathBuilder()
                        .addPath(Rectangle({0, 0}, {4, 4}))
                        .addPath(Rectangle({1, 1}, {3, 3}, true))
                        .build();
  EXPECT_THAT(
      HorizontalBoundary(ring, 2),
      testing::ElementsAre(PointBoundary(0), PointBoundary(1), PointBoundary(3), PointBoundary(4)));
  EXPECT_THAT(HorizontalBoundary(ring, 1),
              testing::ElementsAre(PointBoundary(0), ExactInterval{Position(1), Position(3)},
                                   PointBoundary(4)));
}

TEST(GeodeStrokeTolerance, ThinGapKeepsBothDistinctTransitions) {
  const double gap = std::ldexp(1.0, -40);
  const Path split = PathBuilder()
                         .addPath(Rectangle({0, 0}, {2, 2}))
                         .addPath(Rectangle({2 + gap, 0}, {4, 2}))
                         .build();
  EXPECT_THAT(HorizontalBoundary(split, 1),
              testing::ElementsAre(PointBoundary(0), PointBoundary(2), PointBoundary(2 + gap),
                                   PointBoundary(4)));
}

TEST(GeodeStrokeTolerance, IndexedQueriesMatchCompleteCrossingScan) {
  PathBuilder builder;
  builder.addPath(Rectangle({0, 0}, {4, 4}))
      .addPath(Rectangle({1, 1}, {3, 3}, true))
      .addPath(Rectangle({3, 1}, {6, 3}));
  for (int x = -3; x <= 3; ++x) {
    for (int y = -3; y <= 3; ++y) {
      builder.addPath(Rectangle({double(x), double(y)}, {x + 0.5, y + 0.5}));
    }
  }
  const Path path = builder.build();
  for (const Vector2d center : {Vector2d{0, 0}, Vector2d{2, 2}, Vector2d{4, 1}}) {
    const ExactPoint exactCenter = *ExactCoordinates(center);
    const IndexedOutline outline(path, center);
    for (const ExactPoint direction : {ExactPoint{1, 0}, ExactPoint{0, 1}, ExactPoint{1, 1},
                                       ExactPoint{1, -1}, ExactPoint{2, 3}, ExactPoint{-3, 1}}) {
      std::vector<size_t> candidates;
      ASSERT_TRUE(outline.query(exactCenter, direction, candidates));
      std::sort(candidates.begin(), candidates.end());
      std::vector<size_t> expected;
      for (size_t index = 0; index < outline.segments.size(); ++index) {
        const ExactSegment& segment = outline.segments[index];
        const int from = Sign(Cross(direction, segment.from - exactCenter));
        const int to = Sign(Cross(direction, segment.to - exactCenter));
        if (from == 0 || to == 0 || from != to) {
          expected.push_back(index);
        }
      }
      EXPECT_THAT(candidates, testing::ElementsAreArray(expected));
      EXPECT_THAT(UnionBoundary(outline, exactCenter, direction),
                  testing::ElementsAreArray(UnionBoundary(outline, exactCenter, direction, false)));
    }
  }
}

TEST(GeodeStrokeTolerance, AngularIndexRejectsAMismatchedCenter) {
  const IndexedOutline outline(Rectangle({0, 0}, {2, 2}), {});
  std::vector<size_t> candidates;
  EXPECT_FALSE(outline.query(*ExactCoordinates({1, 0}), {1, 0}, candidates));
  EXPECT_THAT(candidates, testing::IsEmpty());
}

TEST(GeodeStrokeTolerance, ClosedIntervalDistanceDetectsInteriorGapMaximum) {
  const Boundary interval = {{Fraction(0, 1), Fraction(4, 1)}};
  const Boundary endpoints = {{Fraction(0, 1), Fraction(0, 1)}, {Fraction(4, 1), Fraction(4, 1)}};
  const Real distance = BoundaryDistance(interval, endpoints);
  EXPECT_GE(distance, 2);
  EXPECT_LT(distance, 2 + 256 * std::numeric_limits<Real>::epsilon());
  EXPECT_EQ(BoundaryDistance(interval, interval), 0);
}

TEST(GeodeStrokeTolerance, Uint128ConversionEnclosesAllSignificantLimbs) {
  const uint128 value = (uint128(1) << 120) + (uint128(1) << 60) + 1;
  const FloatInterval range = EncloseUint128(value);
  EXPECT_LE(uint128(range.low), value);
  EXPECT_GE(uint128(range.high), value);
  const FloatInterval half = EncloseFraction(Fraction(1, 2));
  EXPECT_LE(half.low, Real(0.5));
  EXPECT_GE(half.high, Real(0.5));
}

}  // namespace
}  // namespace donner::geode
