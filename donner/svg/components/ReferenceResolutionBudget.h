#pragma once
/// @file

#include <array>
#include <cstddef>

namespace donner::svg::components {

/** Bounds document-wide href inheritance traversal for gradients and patterns. */
class ReferenceResolutionBudget {
public:
  enum class Kind : std::size_t { Gradient, Pattern, Count };

  static constexpr std::size_t kMaximumReferenceDepth = 64;
  static constexpr std::size_t kMaximumHopsPerKind = 64 * 1024;

  struct Stats {
    std::size_t hops = 0;
    bool rejected = false;
  };

  bool reserve(Kind kind, std::size_t depth) {
    (void)depth;
    Stats& current = stats_[static_cast<std::size_t>(kind)];
    ++current.hops;
    return true;
  }

  void reset(Kind kind) { stats_[static_cast<std::size_t>(kind)] = {}; }

  const Stats& stats(Kind kind) const { return stats_[static_cast<std::size_t>(kind)]; }

private:
  std::array<Stats, static_cast<std::size_t>(Kind::Count)> stats_{};
};

}  // namespace donner::svg::components
