#pragma once
/// @file

#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/element/ElementTraversalGenerators.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/Specificity.h"
#include "donner/css/WqName.h"
#include "donner/css/details/AnbValue.h"
#include "donner/css/selectors/SelectorMatchOptions.h"

namespace donner::css {

// Forward declaration.
struct Selector;

namespace details {

/**
 * Concept that can either be a `std::optional<TypeSelector>` or `std::unique_ptr<Selector>`, which
 * supports:
 * - `operator bool`
 * - `operator->`
 * - `bool matches(const ElementLike& element);`
 */
template <typename T, typename ElementType>
concept OptionalSelectorLike = requires(const T t, const ElementType element) {
  { bool(t) } -> std::same_as<bool>;
  { bool(t.operator->()->matches(element)) } -> std::same_as<bool>;
};

}  // namespace details

/**
 * Selectors which start with one colon, e.g. `:nth-child()`, are called pseudo-classes, and they
 * represent additional state information not directly present in the document tree.
 *
 * Each pseudo-class selector has a unique behavior.
 *
 * Selectors supported:
 * - `:nth-child(An+B [of S])` - Selects the element if its index within its parent is `An+B`
 *   (1-based) when counting from the first element that would be selected by `S`. If `S` is
 omitted,
 *   the selector matches only elements that are direct children of their parent.
 * - `:nth-last-child(An+B [of S])` - Selects the element if its index within its parent is `An+B`
 *   (1-based) when counting from the last element that would be selected by `S`. If `S` is omitted,
 *   the selector matches only elements that are direct children of their parent.
 * - `:nth-of-type(An+B)` - Selects the element if its index within its parent's children of the
 *   same type is `An+B` (1-based).
 * - `:nth-last-of-type(An+B)` - Selects the element if its index within its parent's children of
 *   the same type is `An+B` (1-based).
 * - `:first-child` - Selects the element if it is the first child of its parent.
 * - `:last-child` - Selects the element if it is the last child of its parent.
 * - `:first-of-type` - Selects the element if it is the first child of its parent and its type is
 *   the same as its parent.
 * - `:last-of-type` - Selects the element if it is the last child of its parent and its type is
 *   the same as its parent.
 * - `:only-child` - Selects the element if it is the only child of its parent.
 * - `:only-of-type` - Selects the element if it is the only child of its parent and its type is
 *   the same as its parent.
 * - `:empty` - Selects the element if it has no children.
 * - `:root` - Selects the element if it is the root of the document.
 * - `:is(S)` - Selects the element if it matches any of the selectors in the argument list.
 * - `:not(S)` - Selects the element if it does not match `S`.
 * - `:where(S)` - Selects the element if it matches all of the selectors in the argument list.
 *
 * Not yet implemented, see https://github.com/jwmcglynn/donner/issues/3:
 * - `:has(S)` - Selects the element if any of its descendants match `S`.
 * - `:defined` - Selects if the element is supported by the user agent (donner svg in this
 *    case).
 *
 * Pseudo-classes are defined in the following specs:
 * - Linguistic Pseudo-classes, such as `:dir()` and `:lang()`,
 *   https://www.w3.org/TR/selectors-4/#linguistic-pseudos
 * - Location Pseudo-classes, such as `:link` and `:visited`,
 *   https://www.w3.org/TR/selectors-4/#location
 * - User Action Pseudo-classes, such as `:hover` and `:active`,
 *   https://www.w3.org/TR/selectors-4/#useraction-pseudos
 * - Time-dimensional Pseudo-classes, such as `:current` and `:past`,
 *   https://www.w3.org/TR/selectors-4/#time-pseudos
 * - Resource State Pseudo-classes, such as `:playing` and `:muted`,
 *   https://www.w3.org/TR/selectors-4/#resource-pseudos
 * - Element Display State Pseudo-classes, such as `:open` and `:fullscreen`,
 *   https://www.w3.org/TR/selectors-4/#display-state-pseudos
 * - Input Pseudo-classes, such as `:enabled` and `:checked`,
 *   https://www.w3.org/TR/selectors-4/#input-pseudos
 * - Tree-Structural Pseudo-classes, such as `:empty` and `:nth-child()`,
 *   https://www.w3.org/TR/selectors-4/#structural-pseudos
 */
struct PseudoClassSelector {
  RcString ident;  //!< The name of the pseudo-class.

  /// The arguments of the pseudo-class, if it is a function.
  std::optional<std::vector<ComponentValue>> argsIfFunction;
  /// The An+B value of the pseudo-class, for An+B pseudo-classes such as `:nth-child`.
  std::optional<AnbValue> anbValueIfAnb;

  /// The selector of the pseudo-class, for pseudo-classes such as `:is()` and `:not()`, or
  /// `:nth-child(An+B of S)`.
  std::unique_ptr<Selector> selector;

  /**
   * Create a PseudoClassSelector with the given ident.
   *
   * @param ident The name of the pseudo-class.
   */
  explicit PseudoClassSelector(const RcString& ident);

  /// Destructor.
  ~PseudoClassSelector() noexcept;

  // Moveable and copyable.
  /// Move constructor.
  PseudoClassSelector(PseudoClassSelector&&) noexcept;
  /// Move assignment operator.
  PseudoClassSelector& operator=(PseudoClassSelector&&) noexcept;
  /// Copy constructor.
  PseudoClassSelector(const PseudoClassSelector& other);
  /// Copy assignment operator.
  PseudoClassSelector& operator=(const PseudoClassSelector& other);

  /**
   * Returns true if this selector is valid and supported by this implementation.
   *
   * @see https://www.w3.org/TR/selectors-4/#invalid
   */
  bool isValid() const {
    if (!argsIfFunction.has_value()) {
      // Check for valid non-function pseudo-classes
      return ident.equalsLowercase("root") || ident.equalsLowercase("empty") ||
             ident.equalsLowercase("first-child") || ident.equalsLowercase("last-child") ||
             ident.equalsLowercase("only-child") || ident.equalsLowercase("first-of-type") ||
             ident.equalsLowercase("last-of-type") || ident.equalsLowercase("only-of-type");
    } else {
      // It's a function.
      if (anbValueIfAnb.has_value()) {
        return ident.equalsLowercase("nth-child") || ident.equalsLowercase("nth-last-child") ||
               ident.equalsLowercase("nth-of-type") || ident.equalsLowercase("nth-last-of-type");
      }
    }

    return false;
  }

  /**
   * Result of \ref matches, returns if the selector matched and if it can be treated as a
   * "primary" matcher. Every matcher except `:scope` is primary, and can match an element directly.
   * `:scope` can only be used to find another element in the tree: `:scope > div` is valid and
   * matches a div, but `:scope` itself cannot match an element.
   */
  struct PseudoMatchResult {
    bool matches = false;   ///< True if the selector matched.
    bool isPrimary = true;  ///< True if the selector is a primary matcher.

    /// Constructor, implicit from a bool.
    /* implicit */ PseudoMatchResult(bool matches, bool isPrimary = true)
        : matches(matches), isPrimary(isPrimary) {}
  };

  /**
   * Returns true if the provided element matches this selector.
   *
   * @tparam T A type that fulfills the ElementLike concept, to enable traversing the tree to
   * match the selector.
   * @param element The element to check.
   * @param options The options to use when matching
   */
  // NOTE: This function is implemented in Selector.h due to a dependency on the Selector type
  template <ElementLike T>
  PseudoMatchResult matches(const T& element, const SelectorMatchOptions<T>& options) const;

  /**
   * Compute the pseudo-class's specificity, using the rules from
   * https://www.w3.org/TR/2022/WD-selectors-4-20221111/#specificity-rules
   */
  Specificity::ABC computeSpecificity() const;

  /// Ostream output operator for \ref PseudoClassSelector, outputs a debug string e.g.
  /// `PseudoClassSelector(after)`.
  friend std::ostream& operator<<(std::ostream& os, const PseudoClassSelector& obj);

private:
  template <ElementLike T, details::OptionalSelectorLike<T> SelectorType>
  static int getIndexInParent(const T& parent, const T& element, bool fromEnd,
                              const SelectorType& matchingType) {
    int childIndex = 1;
    if (!fromEnd) {
      for (std::optional<T> child = parent.firstChild(); child;
           child = child.value().nextSibling()) {
        if (matchingType && !matchingType->matches(child.value())) {
          continue;
        }

        if (child.value() == element) {
          return childIndex;
        } else {
          ++childIndex;
        }
      }
    } else {
      for (std::optional<T> child = parent.lastChild(); child;
           child = child.value().previousSibling()) {
        if (matchingType && !matchingType->matches(child.value())) {
          continue;
        }

        if (child.value() == element) {
          return childIndex;
        } else {
          ++childIndex;
        }
      }
    }

    assert(matchingType &&
           "Should only reach end of child list if there is a Selector skipping elements");
    return -1;
  }

  template <ElementLike T>
  static bool isFirstOfType(const T& element, const XMLQualifiedNameRef& type) {
    for (std::optional<T> child = element.previousSibling(); child;
         child = child.value().previousSibling()) {
      if (child.value().tagName() == type) {
        return false;
      }
    }

    return true;
  }

  template <ElementLike T>
  static bool isLastOfType(const T& element, const XMLQualifiedNameRef& type) {
    for (std::optional<T> child = element.nextSibling(); child;
         child = child.value().nextSibling()) {
      if (child.value().tagName() == type) {
        return false;
      }
    }

    return true;
  }
};

}  // namespace donner::css
