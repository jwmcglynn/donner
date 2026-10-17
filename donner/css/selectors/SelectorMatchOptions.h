#pragma once
/// @file

#include <cstddef>

#include "donner/base/element/ElementLike.h"

namespace donner::css {

/** Aggregate bound on selector work: element visits and bounded chunks of text inspection. */
class SelectorTraversalBudget {
public:
  /// Maximum work steps in one aggregate cascade or standalone match.
  static constexpr std::size_t kMaximumSteps = 4 * 1024 * 1024;

  SelectorTraversalBudget() = default;
  /// Create an aggregate selector traversal budget with a caller-selected work-step ceiling.
  /// @param maximumSteps Maximum work steps to admit.
  explicit SelectorTraversalBudget(std::size_t maximumSteps) : maximumSteps_(maximumSteps) {}

  /// Consume one work step, returning false once the limit is reached.
  [[nodiscard]] bool consume() {
    if (rejected_ || steps_ >= maximumSteps_) {
      rejected_ = true;
      return false;
    }
    ++steps_;
    return true;
  }

  /// Work steps charged to this budget.
  [[nodiscard]] std::size_t steps() const { return steps_; }
  /// Whether a traversal exceeded the visit ceiling.
  [[nodiscard]] bool rejected() const { return rejected_; }
  /// Configured maximum work steps.
  [[nodiscard]] std::size_t maximumSteps() const { return maximumSteps_; }

  /// Clear accumulated work steps and rejection state while preserving the ceiling.
  void reset() {
    steps_ = 0;
    rejected_ = false;
  }

private:
  std::size_t steps_ = 0;
  bool rejected_ = false;
  std::size_t maximumSteps_ = kMaximumSteps;
};

/** Navigation that charges both visible elements and adapter-specific skipped nodes.
 * @tparam T Element-like value.
 */
template <ElementLike T>
class SelectorTraversalNavigation {
public:
  /// Bind navigation to one aggregate match budget. @param budget Shared work budget.
  explicit SelectorTraversalNavigation(SelectorTraversalBudget* budget) : budget_(budget) {}
  /// Get the first child under the shared budget. @param element Reference element.
  std::optional<T> firstChild(const T& element) const {
    if (budget_ != nullptr && !budget_->consume()) {
      return std::nullopt;
    }
    if constexpr (requires { element.firstChild(budget_); }) {
      return element.firstChild(budget_);
    }
    return element.firstChild();
  }
  /// Get the last child under the shared budget. @param element Reference element.
  std::optional<T> lastChild(const T& element) const {
    if (budget_ != nullptr && !budget_->consume()) {
      return std::nullopt;
    }
    if constexpr (requires { element.lastChild(budget_); }) {
      return element.lastChild(budget_);
    }
    return element.lastChild();
  }
  /// Get the preceding sibling under the shared budget. @param element Reference element.
  std::optional<T> previousSibling(const T& element) const {
    if (budget_ != nullptr && !budget_->consume()) {
      return std::nullopt;
    }
    if constexpr (requires { element.previousSibling(budget_); }) {
      return element.previousSibling(budget_);
    }
    return element.previousSibling();
  }
  /// Get the following sibling under the shared budget. @param element Reference element.
  std::optional<T> nextSibling(const T& element) const {
    if (budget_ != nullptr && !budget_->consume()) {
      return std::nullopt;
    }
    if constexpr (requires { element.nextSibling(budget_); }) {
      return element.nextSibling(budget_);
    }
    return element.nextSibling();
  }

private:
  SelectorTraversalBudget* budget_;
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
  bool allowScopeOnlyMatch = false;  ///< Permit scope-only ancestor and filter matching.
};

}  // namespace donner::css
