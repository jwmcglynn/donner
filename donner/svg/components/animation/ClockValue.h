#pragma once
/// @file

#include <cmath>
#include <limits>
#include <ostream>

namespace donner::svg::components {

/**
 * Represents a SMIL clock value, which can be a concrete time in seconds or `indefinite`.
 *
 * Clock values are used in SVG animation timing attributes like `begin`, `dur`, and `end`.
 * Supported formats: `HH:MM:SS.frac`, `MM:SS.frac`, `<number><metric>` (h/min/s/ms).
 * Default metric is seconds.
 */
class ClockValue {
public:
  /// Create a ClockValue representing a specific time in seconds.
  static constexpr ClockValue Seconds(double seconds) { return ClockValue(seconds); }

  /// Create a ClockValue representing `indefinite`.
  static constexpr ClockValue Indefinite() {
    return ClockValue(std::numeric_limits<double>::infinity());
  }

  /// Default constructor creates a zero clock value.
  constexpr ClockValue() = default;

  /// Returns the time in seconds. Returns infinity for `indefinite`.
  [[nodiscard]] constexpr double seconds() const { return seconds_; }

  /// Returns true if this clock value is `indefinite`.
  [[nodiscard]] constexpr bool isIndefinite() const { return std::isinf(seconds_); }

  /// Equality comparison.
  constexpr bool operator==(const ClockValue&) const = default;

  /// Ordering comparison.
  constexpr auto operator<=>(const ClockValue&) const = default;

  /// Stream output for debugging.
  friend std::ostream& operator<<(std::ostream& os, const ClockValue& cv) {
    if (cv.isIndefinite()) {
      return os << "indefinite";
    }
    return os << cv.seconds_ << "s";
  }

private:
  explicit constexpr ClockValue(double seconds) : seconds_(seconds) {}

  double seconds_ = 0.0;
};

}  // namespace donner::svg::components
