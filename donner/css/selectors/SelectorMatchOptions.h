#pragma once
/// @file

#include <cstddef>

#include "donner/base/element/ElementLike.h"

namespace donner::css {

/** Aggregate bound on element visits performed while matching selectors. */
class SelectorTraversalBudget {
public:
  /// Maximum element visits in one aggregate cascade or standalone match.
  static constexpr std::size_t kMaximumSteps = 4 * 1024 * 1024;

  SelectorTraversalBudget() = default;
  /// Create an aggregate selector traversal budget with a caller-selected visit ceiling.
  /// @param maximumSteps Maximum element visits to admit.
  explicit SelectorTraversalBudget(std::size_t maximumSteps) : maximumSteps_(maximumSteps) {}

  /// Consume one element visit, returning false once the limit is reached.
  [[nodiscard]] bool consume() {
    if (rejected_ || steps_ >= maximumSteps_) {
      rejected_ = true;
      return false;
    }
    ++steps_;
    return true;
  }

  /// Element visits charged to this budget.
  [[nodiscard]] std::size_t steps() const { return steps_; }
  /// Whether a traversal exceeded the visit ceiling.
  [[nodiscard]] bool rejected() const { return rejected_; }
  /// Configured maximum element visits.
  [[nodiscard]] std::size_t maximumSteps() const { return maximumSteps_; }

  /// Clear accumulated visits and rejection state while preserving the ceiling.
  void reset() {
    steps_ = 0;
    rejected_ = false;
  }

private:
  std::size_t steps_ = 0;
  bool rejected_ = false;
  std::size_t maximumSteps_ = kMaximumSteps;
};

/**
 * Options for matching a selector against an element.
 *
 * This is used to pass additional information to the matching algorithm, such as the element to
 * match against for relative queries.
 *
 * @tparam T A type that fulfills the ElementLike concept, matching the \ref Selector::matches
 * method.
 */
template <ElementLike T>
struct SelectorMatchOptions {
  const T* relativeToElement =
      nullptr;  ///< Enables relative querying and uses this element as the reference point. For
                ///< example, `> div` will match `div` that is a child of `relativeToElement`.
  const T* scopeElement = nullptr;  ///< Element to match against `:scope` queries. Cannot be
                                    ///< matched directly, but can be used for relative matching.
  SelectorTraversalBudget* traversalBudget = nullptr;  ///< Shared selector-work budget.
};

}  // namespace donner::css
