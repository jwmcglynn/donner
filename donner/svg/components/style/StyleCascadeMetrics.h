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
  /// Times every computed style was discarded to restyle the whole document.
  std::uint64_t fullClears = 0;
  /// Document style passes, full or dirty-only, that ended with the style budget refusing work.
  /// Refusals while computing an entity list or a single entity's style are not counted.
  std::uint64_t budgetRejections = 0;
  /// Whole \ref PropertyRegistry copies and moves made while cascading elements, each of which
  /// writes every property. This physical count depends on worker cache placement. Building an
  /// element's computed style copies its local declarations once.
  std::uint64_t registryCopies = 0;
  /// Actual cache hits; stealing can change this count.
  std::uint64_t matchedPropertiesCacheHits = 0;
  /// Actual cache misses; stealing can change this count.
  std::uint64_t matchedPropertiesCacheMisses = 0;
  /// Element cascades excluded from matched-properties caching.
  std::uint64_t matchedPropertiesCacheUncacheable = 0;
  /// Actual parses avoided by cache hits; stealing can change this count.
  std::uint64_t declarationParsesSkipped = 0;
  /// Tested rules skipped without matching because, for every selector-list entry, the ancestor
  /// filter showed that no ancestor of the element has a key the entry needs.
  std::uint64_t ancestorFilterRejections = 0;
  /// Tested rules with ancestor keys that the enabled ancestor filter let through and that then
  /// did not match. A match can also fail in the subject compound, so this is an upper bound on
  /// the filter's false positives.
  std::uint64_t ancestorFilterFalsePositives = 0;
  /// Times a style pass's ancestor filter was disabled below an ancestor because inserting its
  /// keys saturated a counter.
  std::uint64_t ancestorFilterSaturations = 0;
  /// Times a style pass's ancestor filter was disabled below an ancestor because its keys
  /// exceeded the count or input-byte limit.
  std::uint64_t ancestorFilterKeyLimits = 0;

  /// Compare work that is independent of physical cache placement and stealing.
  bool sameLogicalWork(const StyleCascadeCounters& other) const {
    return elementsCascaded == other.elementsCascaded && rulesTested == other.rulesTested &&
           rulesMatched == other.rulesMatched && declarationsApplied == other.declarationsApplied &&
           fullClears == other.fullClears && budgetRejections == other.budgetRejections &&
           matchedPropertiesCacheUncacheable == other.matchedPropertiesCacheUncacheable &&
           ancestorFilterRejections == other.ancestorFilterRejections &&
           ancestorFilterFalsePositives == other.ancestorFilterFalsePositives &&
           ancestorFilterSaturations == other.ancestorFilterSaturations &&
           ancestorFilterKeyLimits == other.ancestorFilterKeyLimits;
  }

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
        .registryCopies = after.registryCopies - before.registryCopies,
        .matchedPropertiesCacheHits =
            after.matchedPropertiesCacheHits - before.matchedPropertiesCacheHits,
        .matchedPropertiesCacheMisses =
            after.matchedPropertiesCacheMisses - before.matchedPropertiesCacheMisses,
        .matchedPropertiesCacheUncacheable =
            after.matchedPropertiesCacheUncacheable - before.matchedPropertiesCacheUncacheable,
        .declarationParsesSkipped =
            after.declarationParsesSkipped - before.declarationParsesSkipped,
        .ancestorFilterRejections =
            after.ancestorFilterRejections - before.ancestorFilterRejections,
        .ancestorFilterFalsePositives =
            after.ancestorFilterFalsePositives - before.ancestorFilterFalsePositives,
        .ancestorFilterSaturations =
            after.ancestorFilterSaturations - before.ancestorFilterSaturations,
        .ancestorFilterKeyLimits = after.ancestorFilterKeyLimits - before.ancestorFilterKeyLimits,
    };
  }

  /// Print every count, for test failure output.
  friend std::ostream& operator<<(std::ostream& os, const StyleCascadeCounters& counters) {
    return os << "{elementsCascaded=" << counters.elementsCascaded
              << ", rulesTested=" << counters.rulesTested
              << ", rulesMatched=" << counters.rulesMatched
              << ", declarationsApplied=" << counters.declarationsApplied
              << ", fullClears=" << counters.fullClears
              << ", budgetRejections=" << counters.budgetRejections
              << ", registryCopies=" << counters.registryCopies
              << ", matchedPropertiesCacheHits=" << counters.matchedPropertiesCacheHits
              << ", matchedPropertiesCacheMisses=" << counters.matchedPropertiesCacheMisses
              << ", matchedPropertiesCacheUncacheable="
              << counters.matchedPropertiesCacheUncacheable
              << ", declarationParsesSkipped=" << counters.declarationParsesSkipped
              << ", ancestorFilterRejections=" << counters.ancestorFilterRejections
              << ", ancestorFilterFalsePositives=" << counters.ancestorFilterFalsePositives
              << ", ancestorFilterSaturations=" << counters.ancestorFilterSaturations
              << ", ancestorFilterKeyLimits=" << counters.ancestorFilterKeyLimits << "}";
  }
};

/**
 * Style cascade instrumentation kept in a document registry's context by the style system.
 */
struct StyleCascadeMetrics {
  /// Large static passes dispatched to a scheduler.
  std::uint64_t scheduledPasses = 0;
  /// Tasks admitted to the flat style scope.
  std::uint64_t admittedStyleTasks = 0;
  /// Ready ranges processed inline after the scheduler declined a spawn.
  std::uint64_t declinedStyleTasks = 0;
  /// Attempted preparations whose owner fell back to serial matching.
  std::uint64_t abandonedStylePasses = 0;
  /// Canonical hits served by an unchanged, already-referenced clock entry.
  std::uint64_t reusedCanonicalHits = 0;
  /// Nonempty per-element warning phases emitted from matching worker buffers.
  std::uint64_t preparedWarningPhases = 0;
  /// Accepted worker selector traces over scheduled passes.
  std::uint64_t replayedSelectorTraces = 0;
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
