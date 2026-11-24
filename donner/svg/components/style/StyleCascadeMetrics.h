#pragma once
/// @file

#include <chrono>
#include <cstdint>
#include <ostream>

namespace donner::svg::components {

/**
 * Exact counts of the style cascade work done for one document. Counts only grow; the work of one
 * frame or edit is the difference between a snapshot taken after it and one taken before it.
 */
struct StyleCascadeCounters {
  /// Element cascades, counting every time an entity's style is computed, including repeats.
  std::uint64_t elementsCascaded = 0;
  /// Stylesheet rules tested against an element, each admitted by the rule-element budget.
  std::uint64_t rulesTested = 0;
  /// Tested rules with a selector-list entry that matched the element.
  std::uint64_t rulesMatched = 0;
  /// Declarations of matched rules admitted by the budget and handed to the property parser,
  /// including ones the parser rejects.
  std::uint64_t declarationsApplied = 0;
  /// Full style passes that discarded every computed style before recomputing the document.
  std::uint64_t fullClears = 0;
  /// Document style passes, full or dirty-only, that ended with the style budget refusing work.
  /// Refusals while computing an entity list or a single entity's style are not counted.
  std::uint64_t budgetRejections = 0;

  /// Equality operator.
  bool operator==(const StyleCascadeCounters&) const = default;

  /**
   * Work done between two snapshots.
   * @param after Snapshot taken second.
   * @param before Snapshot taken first.
   */
  friend StyleCascadeCounters operator-(const StyleCascadeCounters& after,
                                        const StyleCascadeCounters& before) {
    return StyleCascadeCounters{
        .elementsCascaded = after.elementsCascaded - before.elementsCascaded,
        .rulesTested = after.rulesTested - before.rulesTested,
        .rulesMatched = after.rulesMatched - before.rulesMatched,
        .declarationsApplied = after.declarationsApplied - before.declarationsApplied,
        .fullClears = after.fullClears - before.fullClears,
        .budgetRejections = after.budgetRejections - before.budgetRejections,
    };
  }

  /// Print every count, for test failure output.
  friend std::ostream& operator<<(std::ostream& os, const StyleCascadeCounters& counters) {
    return os << "{elementsCascaded=" << counters.elementsCascaded
              << ", rulesTested=" << counters.rulesTested
              << ", rulesMatched=" << counters.rulesMatched
              << ", declarationsApplied=" << counters.declarationsApplied
              << ", fullClears=" << counters.fullClears
              << ", budgetRejections=" << counters.budgetRejections << "}";
  }
};

/**
 * Style cascade instrumentation kept in a document registry's context by the style system.
 */
struct StyleCascadeMetrics {
  /// Exact work counts.
  StyleCascadeCounters counters;
  /// Wall-clock time inside the outermost whole-document and entity-list style calls. Unlike
  /// \ref counters it varies between runs and machines, and a single entity's style computed
  /// outside those calls is counted in \ref counters but not timed.
  std::chrono::nanoseconds styleTime{0};
  /// Style calls currently timed on this registry, so nested calls are not timed twice.
  int activeTimedCalls = 0;
};

}  // namespace donner::svg::components
