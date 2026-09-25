#pragma once
/// @file

#include <array>
#include <cstddef>

namespace donner::svg::components {

/** Bounds document-wide href inheritance traversal for filters, gradients, and patterns. */
class ReferenceResolutionBudget {
public:
  /// Independent reference chains charged against this document budget.
  enum class Kind : std::size_t {
    Filter,    ///< Filter inheritance references.
    Gradient,  ///< Gradient inheritance references.
    Pattern,   ///< Pattern inheritance references.
    Count,     ///< Number of tracked reference kinds; not a reservable kind.
  };

  /// Maximum inheritance depth accepted for one reference chain.
  static constexpr std::size_t kMaximumReferenceDepth = 64;
  /// Maximum reference hops accepted per kind across the document.
  static constexpr std::size_t kMaximumHopsPerKind = 64 * 1024;

  /// Caller-selected depth and per-kind hop limits.
  struct Limits {
    /// Maximum depth of one inheritance chain.
    std::size_t maximumReferenceDepth = kMaximumReferenceDepth;
    /// Maximum aggregate hops charged to each reference kind.
    std::size_t maximumHopsPerKind = kMaximumHopsPerKind;
  };

  /// Accumulated reservation state for one reference kind.
  struct Stats {
    /// Number of successful reference hops charged.
    std::size_t hops = 0;
    /// True after this kind exceeds either configured limit.
    bool rejected = false;
  };

  ReferenceResolutionBudget() = default;
  /// Create a budget with caller-selected depth and hop limits.
  /// @param limits Limits applied independently to each reference kind.
  explicit ReferenceResolutionBudget(Limits limits) : limits_(limits) {}

  /// Charge a reference hop at the given inheritance depth.
  /// @param kind Reference kind being traversed.
  /// @param depth Depth of this hop in the current inheritance chain.
  /// @return False if this kind has already rejected work or either limit is exceeded.
  bool reserve(Kind kind, std::size_t depth) {
    Stats& current = stats_[static_cast<std::size_t>(kind)];
    if (current.rejected || depth > limits_.maximumReferenceDepth ||
        current.hops >= limits_.maximumHopsPerKind) {
      current.rejected = true;
      return false;
    }
    ++current.hops;
    return true;
  }

  /// Clear the hop count and rejection state for one reference kind.
  /// @param kind Reference kind to reset.
  void reset(Kind kind) { stats_[static_cast<std::size_t>(kind)] = {}; }

  /// Return accumulated hop count and rejection state for one reference kind.
  /// @param kind Reference kind to inspect.
  const Stats& stats(Kind kind) const { return stats_[static_cast<std::size_t>(kind)]; }

private:
  Limits limits_;
  std::array<Stats, static_cast<std::size_t>(Kind::Count)> stats_{};
};

}  // namespace donner::svg::components
