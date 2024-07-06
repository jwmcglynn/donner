#pragma once
/// @file

#include <vector>

#include "donner/base/Utils.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/SelectorTraversal.h"
#include "donner/css/Specificity.h"
#include "donner/css/selectors/CompoundSelector.h"

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
 * \ref Descendant combinator, while `div > span` is a \ref Child combinator.
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
    /// The combinator between this compound selector and the next. For regular selector lists,
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
   * @return true if the element matches the selector, within a SelectorMatchResult which also
   *              contains the specificity.
   */
  template <traversal::ElementLike T>
  SelectorMatchResult matches(const T& targetElement) const {
    // TODO: Accept :scope elements.
    using GeneratorCreator = std::function<traversal::SelectorTraversalGenerator<T>()>;
    GeneratorCreator elementsGenerator =
        std::bind(&traversal::singleElementGenerator<T>, targetElement);

    // "To match a complex selector against an element, process it compound selector at a time, in
    // right-to-left order."
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      const Entry& entry = *it;

      // "If any simple selectors in the rightmost compound selector does not match the element,
      // return failure."
      std::optional<T> currentElement;
      traversal::SelectorTraversalGenerator<T> elements = elementsGenerator();
      while (elements.next()) {
        const T element = elements.getValue();
        if (entry.compoundSelector.matches(element)) {
          currentElement = element;
          break;
        }
      }

      if (!currentElement) {
        return SelectorMatchResult::None();
      }

      // "Otherwise, if there is only one compound selector in the complex selector, return
      // success."
      // In this case, return success once we've reached the leftmost compound selector.
      if (it + 1 == entries.rend()) {
        return SelectorMatchResult::Match(Specificity(computeSpecificity()));
      }

      // "Otherwise, consider all possible elements that could be related to this element by the
      // rightmost combinator. If the operation of matching the selector consisting of this
      // selector with the rightmost compound selector and rightmost combinator removed against
      // any one of these elements returns success, then return success. Otherwise, return
      // failure."
      if (entry.combinator == Combinator::Descendant) {
        elementsGenerator = std::bind(&traversal::parentsGenerator<T>, currentElement.value());
      } else if (entry.combinator == Combinator::Child) {
        if (auto parent = currentElement->parentElement()) {
          elementsGenerator = std::bind(&traversal::singleElementGenerator<T>, parent.value());
        } else {
          return SelectorMatchResult::None();
        }
      } else if (entry.combinator == Combinator::NextSibling) {
        if (auto previous = currentElement->previousSibling()) {
          elementsGenerator = std::bind(&traversal::singleElementGenerator<T>, previous.value());
        } else {
          return SelectorMatchResult::None();
        }
      } else if (entry.combinator == Combinator::SubsequentSibling) {
        elementsGenerator =
            std::bind(&traversal::previousSiblingsGenerator<T>, currentElement.value());
      } else {
        // NOTE: Combinator::Column does not apply to SVG so it never matches.
        return SelectorMatchResult::None();
      }
    }

    // Default to not match.
    return SelectorMatchResult::None();
  }

  /// Output a human-readable representation of the selector.
  friend std::ostream& operator<<(std::ostream& os, const ComplexSelector& obj);
};

}  // namespace donner::css
