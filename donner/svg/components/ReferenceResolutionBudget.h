#pragma once
/// @file

#include <array>
#include <cstddef>

namespace donner::svg::components {

/** Bounds document-wide href inheritance traversal for filters, gradients, and patterns. */
class ReferenceResolutionBudget {
public:
  enum class Kind : std::size_t { Filter, Gradient, Pattern, Count };

  static constexpr std::size_t kMaximumReferenceDepth = 64;
  static constexpr std::size_t kMaximumHopsPerKind = 64 * 1024;

  struct Limits {
    std::size_t maximumReferenceDepth = kMaximumReferenceDepth;
    std::size_t maximumHopsPerKind = kMaximumHopsPerKind;
  };

  struct Stats {
    std::size_t hops = 0;
    bool rejected = false;
  };

  ReferenceResolutionBudget() = default;
  explicit ReferenceResolutionBudget(Limits limits) : limits_(limits) {}

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

  void reset(Kind kind) { stats_[static_cast<std::size_t>(kind)] = {}; }

  const Stats& stats(Kind kind) const { return stats_[static_cast<std::size_t>(kind)]; }

private:
  Limits limits_;
  std::array<Stats, static_cast<std::size_t>(Kind::Count)> stats_{};
};

}  // namespace donner::svg::components
