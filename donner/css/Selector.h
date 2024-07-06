#pragma once
/// @file

#include "donner/base/element/ElementLike.h"
#include "donner/base/element/ElementTraversalGenerators.h"
#include "donner/css/Specificity.h"
#include "donner/css/selectors/ComplexSelector.h"
#include "donner/css/selectors/SelectorMatchOptions.h"

namespace donner::css {

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

  Specificity::ABC maxSpecificity() const {
    Specificity::ABC result;
    for (const auto& entry : entries) {
      const Specificity::ABC entrySpecificity = entry.computeSpecificity();
      if (entrySpecificity > result) {
        result = entrySpecificity;
      }
    }

    return result;
  }

  /**
   * Match an element against a Selector.
   *
   * @tparam T A type that fulfills the ElementLike concept, to enable traversing the tree to
   * match the selector.
   * @param targetElement Element to match against.
   * @param options Options to control matching.
   * @returns true if any ComplexSelector in the Selector matches the given element.
   */
  template <ElementLike T>
  SelectorMatchResult matches(const T& targetElement, const SelectorMatchOptions<T>& options =
                                                          SelectorMatchOptions<T>()) const {
    for (const auto& entry : entries) {
      if (auto result = entry.matches(targetElement, options)) {
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

template <ElementLike T>
PseudoClassSelector::PseudoMatchResult PseudoClassSelector::matches(
    const T& element, const SelectorMatchOptions<T>& options) const {
  if (!argsIfFunction.has_value()) {
    if (ident.equalsLowercase("root")) {
      return !element.parentElement().has_value();
    } else if (ident.equalsLowercase("scope")) {
      if (options.scopeElement) {
        return PseudoMatchResult(element == *options.scopeElement, /* isPrimary */ false);
      } else {
        return PseudoMatchResult(!element.parentElement().has_value(), /* isPrimary */ false);
      }
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

      return !selector->matches(element, options).matched;
    } else if (ident.equalsLowercase("is") || ident.equalsLowercase("where")) {
      if (!selector) {
        return false;
      }

      return selector->matches(element, options).matched;
    } else if (ident.equalsLowercase("has")) {
      if (!selector) {
        return false;
      }

      SelectorMatchOptions<T> optionsOverride = options;
      optionsOverride.relativeToElement = &element;

      // Iterate over all children and match.
      ElementTraversalGenerator<T> elements = allChildrenRecursiveGenerator(element);
      while (elements.next()) {
        const T childElement = elements.getValue();
        if (selector->matches(childElement, optionsOverride).matched) {
          return true;
        }
      }

      return false;
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
