#pragma once
/// @file

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/Specificity.h"
#include "donner/css/selectors/CompoundSelector.h"
#include "donner/css/selectors/SelectorMatchOptions.h"

namespace donner::css {

/**
 * Returned by \ref Selector::matches to indicate whether the selector matched, and if so, the
 * specificity of the match.
 *
 * The `bool` operator can be used to check if the selector matched:
 * ```
 * if (auto match = selector.matches(element); match) {
 *   // ...
 * }
 * ```
 *
 * To construct, use the static methods: \ref None() and \ref Match().
 */
struct SelectorMatchResult {
  bool matched = false;     ///< True if the selector matched.
  Specificity specificity;  ///< The specificity of the match, if matched.

  /**
   * Create a SelectorMatchResult indicating that the selector did not match.
   */
  static constexpr SelectorMatchResult None() { return SelectorMatchResult(); }

  /**
   * Create a SelectorMatchResult indicating that the selector matched, with the given specificity.
   *
   * @param specificity The specificity of the match.
   */
  static constexpr SelectorMatchResult Match(Specificity specificity) {
    return SelectorMatchResult(true, specificity);
  }

  /// Returns true if the selector matched.
  explicit operator bool() const { return matched; }

private:
  // Use the static methods above to construct.
  constexpr SelectorMatchResult() = default;

  constexpr SelectorMatchResult(bool matched, Specificity specificity)
      : matched(matched), specificity(specificity) {}
};

/**
 * Between two compound selectors, there can be a combinator, which specifies how the two elements
 * are associated in the tree.
 *
 * By default, a space between compound selectors is a descendant combinator, e.g. `div span` is a
 * \ref Combinator::Descendant combinator, while `div > span` is a
 * \ref Combinator::Child combinator.
 */
enum class Combinator {
  Descendant,         ///< Space-separated, finds descendants in the tree.
  Child,              ///< '>', finds direct children in the tree.
  NextSibling,        ///< '+', finds the next sibling in the tree.
  SubsequentSibling,  ///< '~', finds all subsequent siblings in the tree.
  Column,  ///< '||', finds the next column in the tree. Note that this is a new feature in CSS
           ///< Selectors Level 4, but isn't applicable to SVG.
};

/**
 * Ostream output operator. Outputs the combinator character, e.g. ' ', '>', '+', '~' or '||'.
 *
 * @param os The output stream.
 * @param combinator The combinator.
 */
inline std::ostream& operator<<(std::ostream& os, Combinator combinator) {
  switch (combinator) {
    case Combinator::Descendant: return os << "' '";
    case Combinator::Child: return os << "'>'";
    case Combinator::NextSibling: return os << "'+'";
    case Combinator::SubsequentSibling: return os << "'~'";
    case Combinator::Column: return os << "'||'";
  }
  UTILS_UNREACHABLE();
}

/**
 * A complex selector is a sequence of one or more compound selectors, separated by combinators.
 *
 * For example, `div > #foo` is a complex selector, with two compound selectors separated by a
 * \ref Combinator::Child.
 */
struct ComplexSelector {
  /// A single entry in a complex selector, which is a compound selector and a combinator.
  struct Entry {
    /// The combinator before this compound selector. For regular selector lists,
    /// the first entry is set to \ref Combinator::Descendant but it has no effect. For relative
    /// selector lists, the first entry is the leading combinator, for example "> div".
    Combinator combinator;
    CompoundSelector compoundSelector;  ///< The compound selector.
  };

  std::vector<Entry> entries;  ///< The entries in the complex selector.

  /**
   * Return true if this selector is valid and supported by this implementation.
   *
   * @see https://www.w3.org/TR/selectors-4/#invalid
   */
  bool isValid() const;

  /**
   * Compute specificity of the ComplexSelector, see
   * https://www.w3.org/TR/selectors-4/#specificity-rules.
   */
  Specificity::ABC computeSpecificity() const;

  /**
   * Match a selector against an element, following the rules in the spec:
   * https://www.w3.org/TR/selectors-4/#match-against-element
   *
   * @tparam T A type that fulfills the ElementLike concept, to enable traversing the tree to
   * match the selector.
   * @param targetElement Element to match against.
   * @param options Options to control matching.
   * @return true if the element matches the selector, within a SelectorMatchResult which also
   *              contains the specificity.
   */
  template <ElementLike T>
  SelectorMatchResult matches(const T& targetElement,
                              const SelectorMatchOptions<T>& options) const {
    if (entries.empty()) {
      return SelectorMatchResult::None();
    }
    return matchChain(
               entries.size() - 1, 0, false,
               CandidateCursor<T>(targetElement, std::nullopt, false, options.traversalBudget),
               options)
               ? SelectorMatchResult::Match(Specificity(computeSpecificity()))
               : SelectorMatchResult::None();
  }

  /**
   * Match this relative selector forward from its anchor, including sibling relationships.
   * @param anchor Reference element for the leading combinator.
   * @param options Scope and shared traversal budget for the match.
   * @return True if a complete selector chain matches from the anchor.
   */
  template <ElementLike T>
  bool matchesRelativeToAnchor(const T& anchor, const SelectorMatchOptions<T>& options) const {
    if (entries.empty()) {
      return false;
    }
    return matchChain(
        0, entries.size() - 1, true,
        CandidateCursor<T>(anchor, entries.front().combinator, true, options.traversalBudget),
        options);
  }

  /// Ostream output operator for \ref ComplexSelector, outputs debug strings e.g.
  /// "ComplexSelector(CompoundSelector(TypeSelector(name)))"
  friend std::ostream& operator<<(std::ostream& os, const ComplexSelector& obj);

private:
  // Parent links let a descendant cursor backtrack without allocating a coroutine or child stack.
  template <ElementLike T>
  class CandidateCursor {
  public:
    CandidateCursor(T anchor, std::optional<Combinator> relation, bool forward,
                    SelectorTraversalBudget* budget)
        : anchor_(std::move(anchor)), relation_(relation), forward_(forward), budget_(budget) {}

    bool next() {
      if (!started_) {
        started_ = true;
        current_ = first();
      } else if (current_ && relation_) {
        const SelectorTraversalNavigation<T> navigation(budget_);
        switch (*relation_) {
          case Combinator::Descendant:
            current_ = forward_ ? nextDescendant() : current_->parentElement();
            break;
          case Combinator::Child:
            current_ = forward_ ? navigation.nextSibling(*current_) : std::nullopt;
            break;
          case Combinator::SubsequentSibling:
            current_ = forward_ ? navigation.nextSibling(*current_)
                                : navigation.previousSibling(*current_);
            break;
          default: current_.reset(); break;
        }
      } else {
        current_.reset();
      }
      return current_.has_value();
    }

    T getValue() const { return *current_; }

  private:
    std::optional<T> first() const {
      if (!relation_) {
        return anchor_;
      }
      const SelectorTraversalNavigation<T> navigation(budget_);
      switch (*relation_) {
        case Combinator::Descendant:
        case Combinator::Child:
          return forward_ ? navigation.firstChild(anchor_) : anchor_.parentElement();
        case Combinator::NextSibling:
        case Combinator::SubsequentSibling:
          return forward_ ? navigation.nextSibling(anchor_) : navigation.previousSibling(anchor_);
        default: return std::nullopt;
      }
    }

    std::optional<T> nextDescendant() const {
      const SelectorTraversalNavigation<T> navigation(budget_);
      if (auto child = navigation.firstChild(*current_)) {
        return child;
      }
      T ancestor = *current_;
      while (!(ancestor == anchor_) && !(budget_ != nullptr && budget_->rejected())) {
        if (auto sibling = navigation.nextSibling(ancestor)) {
          return sibling;
        }
        const auto parent = ancestor.parentElement();
        if (!parent) {
          break;
        }
        ancestor = *parent;
      }
      return std::nullopt;
    }

    T anchor_;
    std::optional<Combinator> relation_;
    bool forward_;
    SelectorTraversalBudget* budget_;
    bool started_ = false;
    std::optional<T> current_;
  };

  template <ElementLike T>
  static bool budgetRejected(const SelectorMatchOptions<T>& options) {
    return options.traversalBudget != nullptr && options.traversalBudget->rejected();
  }

  template <ElementLike T>
  bool matchesCandidate(const T& candidate, std::size_t index,
                        const SelectorMatchOptions<T>& options) const {
    if (options.traversalBudget != nullptr && !options.traversalBudget->consume()) {
      return false;
    }
    return entries[index].compoundSelector.matches(
               candidate, index + 1 == entries.size() && !options.allowScopeOnlyMatch, options) &&
           !budgetRejected(options);
  }

  template <ElementLike T>
  bool matchesBoundary(const T& candidate, bool forward,
                       const SelectorMatchOptions<T>& options) const {
    if (forward || !options.relativeToElement) {
      return true;
    }
    return matchesRelativeTo(candidate, *options.relativeToElement, entries.front().combinator,
                             options.traversalBudget);
  }

  // An explicit search stack permits backtracking without recursive native-stack growth.
  // Every candidate shares the caller's aggregate work budget, including nested functions.
  template <ElementLike T>
  bool matchChain(std::size_t startIndex, std::size_t endIndex, bool forward,
                  CandidateCursor<T> initial, const SelectorMatchOptions<T>& options) const {
    struct Frame {
      std::size_t index;
      CandidateCursor<T> candidates;
    };
    std::vector<Frame> frames;
    frames.push_back({startIndex, std::move(initial)});
    SelectorMatchOptions<T> compoundOptions = options;
    // Nested :is/:not/nth selector arguments are ordinary selectors, not relative lists.
    compoundOptions.relativeToElement = nullptr;
    while (!frames.empty() && !budgetRejected(options)) {
      Frame& frame = frames.back();
      if (!frame.candidates.next()) {
        frames.pop_back();
        continue;
      }
      const T candidate = frame.candidates.getValue();
      if (!matchesCandidate(candidate, frame.index, compoundOptions)) {
        continue;
      }
      if (frame.index == endIndex) {
        if (matchesBoundary(candidate, forward, options)) {
          return true;
        }
        continue;
      }
      const std::size_t next = forward ? frame.index + 1 : frame.index - 1;
      const Combinator combinator =
          forward ? entries[next].combinator : entries[frame.index].combinator;
      frames.push_back(
          {next, CandidateCursor<T>(candidate, combinator, forward, options.traversalBudget)});
    }
    return false;
  }

  /**
   * For relative selectors, check if the current element matches the relativeToElement with the
   * given combinator.
   *
   * @tparam T A type that fulfills the ElementLike concept, to enable traversing the tree to match
   * the selector.
   * @param currentElement The element to check.
   * @param relativeToElement The element to check against for relative matching, for example, the
   * parent of the current element. For example, `> div` the current element will be the `div` and
   * the relativeToElement will be the parent.
   */
  template <ElementLike T>
  bool matchesRelativeTo(const T& currentElement, const T& relativeToElement, Combinator combinator,
                         SelectorTraversalBudget* traversalBudget) const {
    const SelectorTraversalNavigation<T> navigation(traversalBudget);
    switch (combinator) {
      case Combinator::Descendant: {
        CandidateCursor<T> elements(currentElement, Combinator::Descendant, false, traversalBudget);
        while (elements.next()) {
          if (traversalBudget != nullptr && !traversalBudget->consume()) {
            return false;
          }
          if (elements.getValue() == relativeToElement) {
            return true;
          }
        }
        return false;
      }
      case Combinator::Child: return (currentElement.parentElement() == relativeToElement);
      case Combinator::NextSibling:
        return (navigation.previousSibling(currentElement) == relativeToElement);
      case Combinator::SubsequentSibling: {
        CandidateCursor<T> elements(currentElement, Combinator::SubsequentSibling, false,
                                    traversalBudget);
        while (elements.next()) {
          if (traversalBudget != nullptr && !traversalBudget->consume()) {
            return false;
          }
          if (elements.getValue() == relativeToElement) {
            return true;
          }
        }
        return false;
      }
      default:
        // NOTE: Combinator::Column does not apply to SVG so it never matches.
        return false;
    }
  }
};

}  // namespace donner::css
