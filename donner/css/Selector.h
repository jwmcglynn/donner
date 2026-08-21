#pragma once
/// @file

#include "donner/base/element/ElementLike.h"  // IWYU pragma: keep, for ElementLike
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

  // Moveable and copyable.
  /// Move constructor.
  Selector(Selector&&) noexcept;
  /// Move assignment operator.
  Selector& operator=(Selector&&) noexcept;
  /// Copy constructor.
  Selector(const Selector&);
  /// Copy assignment operator.
  Selector& operator=(const Selector&);

  /// The list of \ref ComplexSelector entries that compose this selector.
  std::vector<ComplexSelector> entries;

  /**
   * Collect the attribute names referenced by every attribute selector in this selector.
   *
   * Recurses into functional pseudo-classes that carry their own selector list, such as `:is()`,
   * `:not()`, `:has()` and `:nth-child(An+B of S)`, so a nested `[attr]` is not missed.
   *
   * Namespaces are ignored: only the local name is collected, which over-approximates
   * `[ns|attr]` to `[attr]`. Callers use this to decide whether writing an attribute can change
   * selector matching, where over-approximating is the safe direction.
   *
   * @param outNames Receives each distinct local name found, appended to any existing contents.
   * @param outMatchesAnyName Set to true if a selector uses a wildcard local name, meaning any
   *   attribute write can change matching. No CSS syntax produces this today; it is a fail-safe.
   */
  void collectAttributeSelectorNames(std::vector<RcString>& outNames,
                                     bool& outMatchesAnyName) const;

  /**
   * Get the max specificity of all ComplexSelectors in the Selector.
   */
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
    SelectorTraversalBudget localBudget;
    SelectorMatchOptions<T> boundedOptions = options;
    if (boundedOptions.traversalBudget == nullptr) {
      boundedOptions.traversalBudget = &localBudget;
    }
    for (const auto& entry : entries) {
      if (auto result = entry.matches(targetElement, boundedOptions)) {
        return result;
      }
    }

    return SelectorMatchResult::None();
  }

  /// Ostream output operator for \ref Selector, prints a debug representation of the selector, e.g.
  /// `Selector(div, .class, #id)`.
  friend std::ostream& operator<<(std::ostream& os, const Selector& obj);
};

template <ElementLike T>
std::optional<PseudoClassSelector::PseudoMatchResult> PseudoClassSelector::matchesSimpleState(
    const T& element, const SelectorMatchOptions<T>& options) const {
  if (ident.equalsLowercase("root")) {
    return !element.parentElement().has_value();
  }
  if (ident.equalsLowercase("scope")) {
    const bool matched = options.scopeElement ? element == *options.scopeElement
                                              : !element.parentElement().has_value();
    return PseudoMatchResult(matched, /* isPrimary */ false);
  }
  if (ident.equalsLowercase("empty")) {
    return !element.firstChild().has_value();
  }
  if (ident.equalsLowercase("first-child")) {
    return !element.previousSibling().has_value();
  }
  if (ident.equalsLowercase("last-child")) {
    return !element.nextSibling().has_value();
  }
  if (ident.equalsLowercase("only-child")) {
    return !element.previousSibling().has_value() && !element.nextSibling().has_value();
  }
  if (ident.equalsLowercase("defined")) {
    return element.isKnownType();
  }
  return std::nullopt;
}

template <ElementLike T>
std::optional<PseudoClassSelector::PseudoMatchResult> PseudoClassSelector::matchesTypeState(
    const T& element, const SelectorMatchOptions<T>& options) const {
  if (ident.equalsLowercase("first-of-type")) {
    return isFirstOfType(element, element.tagName(), options.traversalBudget);
  }
  if (ident.equalsLowercase("last-of-type")) {
    return isLastOfType(element, element.tagName(), options.traversalBudget);
  }
  if (ident.equalsLowercase("only-of-type")) {
    return isFirstOfType(element, element.tagName(), options.traversalBudget) &&
           isLastOfType(element, element.tagName(), options.traversalBudget);
  }
  return std::nullopt;
}

template <ElementLike T>
std::optional<PseudoClassSelector::PseudoMatchResult> PseudoClassSelector::matchesSelectorFunction(
    const T& element, const SelectorMatchOptions<T>& options) const {
  if (ident.equalsLowercase("not")) {
    return selector ? PseudoMatchResult(!selector->matches(element, options).matched)
                    : PseudoMatchResult(false);
  }
  if (ident.equalsLowercase("is") || ident.equalsLowercase("where")) {
    return selector ? PseudoMatchResult(selector->matches(element, options).matched)
                    : PseudoMatchResult(false);
  }
  if (!ident.equalsLowercase("has")) {
    return std::nullopt;
  }
  return matchesHasFunction(element, options);
}

template <ElementLike T>
PseudoClassSelector::PseudoMatchResult PseudoClassSelector::matchesHasFunction(
    const T& element, const SelectorMatchOptions<T>& options) const {
  if (!selector) {
    return PseudoMatchResult(false);
  }

  SelectorMatchOptions<T> optionsOverride = options;
  optionsOverride.relativeToElement = &element;
  ElementTraversalGenerator<T> elements = allChildrenRecursiveGenerator(element);
  while (elements.next()) {
    if (options.traversalBudget != nullptr && !options.traversalBudget->consume()) {
      return PseudoMatchResult(false);
    }
    if (selector->matches(elements.getValue(), optionsOverride).matched) {
      return PseudoMatchResult(true);
    }
  }
  return PseudoMatchResult(false);
}

template <ElementLike T>
PseudoClassSelector::PseudoMatchResult PseudoClassSelector::matchesNthFunction(
    const T& element, const SelectorMatchOptions<T>& options) const {
  const std::optional<T> maybeParent = element.parentElement();
  if (!maybeParent || !anbValueIfAnb) {
    return false;
  }
  if (ident.equalsLowercase("nth-child")) {
    return anbValueIfAnb->evaluate(
        getIndexInParent(*maybeParent, element, /*fromEnd*/ false, selector, options));
  }
  if (ident.equalsLowercase("nth-last-child")) {
    return anbValueIfAnb->evaluate(
        getIndexInParent(*maybeParent, element, /*fromEnd*/ true, selector, options));
  }
  const auto matchingType = std::make_optional<TypeSelector>(element.tagName());
  if (ident.equalsLowercase("nth-of-type")) {
    return anbValueIfAnb->evaluate(
        getIndexInParent(*maybeParent, element, /*fromEnd*/ false, matchingType, options));
  }
  if (ident.equalsLowercase("nth-last-of-type")) {
    return anbValueIfAnb->evaluate(
        getIndexInParent(*maybeParent, element, /*fromEnd*/ true, matchingType, options));
  }
  return false;
}

template <ElementLike T>
PseudoClassSelector::PseudoMatchResult PseudoClassSelector::matches(
    const T& element, const SelectorMatchOptions<T>& options) const {
  if (!argsIfFunction) {
    if (auto result = matchesSimpleState(element, options)) {
      return *result;
    }
    return matchesTypeState(element, options).value_or(PseudoMatchResult(false));
  }
  if (auto result = matchesSelectorFunction(element, options)) {
    return *result;
  }
  return matchesNthFunction(element, options);
}

}  // namespace donner::css
