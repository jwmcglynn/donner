#pragma once
/// @file
/// Internal preflight accounting for text-decoration geometry.

#include <concepts>

namespace donner::svg::detail {

/// Counts geometry before allocation without allowing accumulated costs to exceed the allowance.
/// @tparam Count Unsigned work-counter type.
template <std::unsigned_integral Count>
class DecorationWorkBudget {
public:
  /// Creates a preflight with the draw's remaining allowance.
  /// @param allowance Maximum geometry work admitted by this preflight.
  explicit DecorationWorkBudget(Count allowance) : allowance_(allowance), remaining_(allowance) {}

  /// Charges one segment, permanently exhausting the allowance if it cannot fit.
  /// @param points Number of points the segment would generate.
  void add(Count points) {
    if (exceeded_) {
      return;
    }
    if (points > remaining_) {
      remaining_ = 0;
      exceeded_ = true;
      return;
    }
    remaining_ -= points;
  }

  /// Whether any segment exceeded the allowance.
  [[nodiscard]] bool exceeded() const { return exceeded_; }
  /// Work left after all admitted segments, or zero after rejection.
  [[nodiscard]] Count remaining() const { return remaining_; }
  /// Number of points admitted, valid for geometry construction only when not exceeded.
  [[nodiscard]] Count points() const { return allowance_ - remaining_; }

private:
  Count allowance_;
  Count remaining_;
  bool exceeded_ = false;
};

}  // namespace donner::svg::detail
