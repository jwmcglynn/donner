#pragma once
/// @file

#include <cassert>
#include <concepts>
#include <coroutine>
#include <functional>
#include <variant>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/SelectorTraversal.h"
#include "donner/css/Specificity.h"
#include "donner/css/selectors/CompoundSelector.h"
#include "donner/svg/xml/XMLQualifiedName.h"

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
    Combinator combinator;  ///< The combinator between this compound selector and the next. For
                            ///< the first Entry, this is set to \ref Combinator::Descendant but
                            ///< it has no effect.
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
  Specificity computeSpecificity() const;

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
        return SelectorMatchResult::Match(computeSpecificity());
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
        // TODO: Combinator::Column
        return SelectorMatchResult::None();
      }
    }

    // Default to not match.
    return SelectorMatchResult::None();
  }

  /// Output a human-readable representation of the selector.
  friend std::ostream& operator<<(std::ostream& os, const ComplexSelector& obj);
};

/**
 * A top-level Selector, which is a list of \ref ComplexSelector.
 *
 * This represents the prelude in front of any CSS rule, e.g. `div.foo > span#bar`, which would be
 * a single \ref ComplexSelector. For a comma-separated list, such as `div.foo > span#bar,
 * span#bar`, this would be a \ref Selector with two \ref ComplexSelector entries.
 */
struct Selector {
  /// Constructor.
  Selector();

  /// Destructor.
  ~Selector() noexcept;

  /// Moveable and copyable.
  Selector(Selector&&) noexcept;
  Selector& operator=(Selector&&) noexcept;
  Selector(const Selector&);
  Selector& operator=(const Selector&);

  /// The list of \ref ComplexSelector entries that compose this selector.
  std::vector<ComplexSelector> entries;

  /**
   * Match an element against a Selector.
   *
   * @tparam T A type that fulfills the ElementLike concept, to enable traversing the tree to
   * match the selector.
   * @param targetElement Element to match against.
   * @returns true if any ComplexSelector in the Selector matches the given element.
   */
  template <traversal::ElementLike T>
  SelectorMatchResult matches(const T& targetElement) const {
    for (const auto& entry : entries) {
      if (auto result = entry.matches(targetElement)) {
        return result;
      }
    }

    return SelectorMatchResult::None();
  }

  /**
   * Ostream output operator for Selector in a human-readable format.
   */
  friend std::ostream& operator<<(std::ostream& os, const Selector& obj);
};

template <traversal::ElementLike T>
bool PseudoClassSelector::matches(const T& element) const {
  if (!argsIfFunction.has_value()) {
    if (ident.equalsLowercase("root")) {
      return !element.parentElement().has_value();
    } else if (ident.equalsLowercase("empty")) {
      return !element.firstChild().has_value();
    } else if (ident.equalsLowercase("first-child")) {
      return !element.previousSibling().has_value();
    } else if (ident.equalsLowercase("last-child")) {
      return !element.nextSibling().has_value();
    } else if (ident.equalsLowercase("only-child")) {
      return !element.previousSibling().has_value() && !element.nextSibling().has_value();
    } else if (ident.equalsLowercase("first-of-type")) {
      return isFirstOfType(element, element.xmlTypeName());
    } else if (ident.equalsLowercase("last-of-type")) {
      return isLastOfType(element, element.xmlTypeName());
    } else if (ident.equalsLowercase("only-of-type")) {
      return isFirstOfType(element, element.xmlTypeName()) &&
             isLastOfType(element, element.xmlTypeName());
    }
  } else {
    // It's a function.

    if (ident.equalsLowercase("not")) {
      if (!selector) {
        return false;
      }

      return !selector->matches(element).matched;
    } else if (ident.equalsLowercase("is") || ident.equalsLowercase("where")) {
      if (!selector) {
        return false;
      }

      return selector->matches(element).matched;
    } else {
      const std::optional<T> maybeParent = element.parentElement();
      if (!maybeParent) {
        return false;
      }

      if (ident.equalsLowercase("nth-child") && anbValueIfAnb) {
        const int childIndex = getIndexInParent(*maybeParent, element, /*fromEnd*/ false, selector);
        return anbValueIfAnb->evaluate(childIndex);
      } else if (ident.equalsLowercase("nth-last-child") && anbValueIfAnb) {
        const int childIndex = getIndexInParent(*maybeParent, element, /*fromEnd*/ true, selector);
        return anbValueIfAnb->evaluate(childIndex);
      } else if (ident.equalsLowercase("nth-of-type") && anbValueIfAnb) {
        const int childIndex =
            getIndexInParent(*maybeParent, element, /*fromEnd*/ false,
                             std::make_optional<TypeSelector>(element.xmlTypeName()));
        return anbValueIfAnb->evaluate(childIndex);
      } else if (ident.equalsLowercase("nth-last-of-type") && anbValueIfAnb) {
        const int childIndex =
            getIndexInParent(*maybeParent, element, /*fromEnd*/ true,
                             std::make_optional<TypeSelector>(element.xmlTypeName()));
        return anbValueIfAnb->evaluate(childIndex);
      }
    }
  }

  return false;
}

}  // namespace donner::css
