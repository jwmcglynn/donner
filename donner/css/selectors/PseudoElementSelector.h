#pragma once
/// @file

#include <vector>

#include "donner/base/RcString.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/SelectorTraversal.h"

namespace donner::css {

/**
 * Selectors which start with two colons are called pseudo-elements, e.g. '::before'. They are used
 * to represent elements which are not directly present in the document tree.
 *
 * See https://www.w3.org/TR/selectors-4/#pseudo-elements for the full definition.
 *
 * Pseudo-elements are listed in the CSS Pseudo-Elements Module Level 4 spec:
 * https://www.w3.org/TR/css-pseudo-4/
 *
 * For SVG, there are no supported pseudo-elements, however pseudo-classes are supported.
 */
struct PseudoElementSelector {
  RcString ident;  ///< The identifier of the pseudo-element.
  std::optional<std::vector<ComponentValue>>
      argsIfFunction;  ///< The arguments to the function, if this is a function.

  /**
   * Create a PseudoElementSelector with the given identifier.
   *
   * @param ident The identifier of the pseudo-element.
   */
  explicit PseudoElementSelector(const RcString& ident) : ident(ident) {}

  /// Destructor.
  ~PseudoElementSelector() noexcept = default;

  /// Moveable and copyable.
  PseudoElementSelector(PseudoElementSelector&&) = default;
  PseudoElementSelector& operator=(PseudoElementSelector&&) = default;
  PseudoElementSelector(const PseudoElementSelector&) = default;
  PseudoElementSelector& operator=(const PseudoElementSelector&) = default;

  /**
   * Returns true if this selector is valid and supported by this implementation. This is always
   * false for donner.
   *
   * @see https://www.w3.org/TR/selectors-4/#invalid
   */
  bool isValid() const { return false; }

  /**
   * Returns true if the provided element matches this selector. This is always false for donner.
   *
   * @param element The element to check.
   */
  template <traversal::ElementLike T>
  bool matches(const T& element) const {
    return false;
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const PseudoElementSelector& obj) {
    os << "PseudoElementSelector(" << obj.ident;
    if (obj.argsIfFunction.has_value()) {
      os << " args[";
      for (auto& arg : obj.argsIfFunction.value()) {
        os << arg << ", ";
      }
      os << "]";
    }
    os << ")";
    return os;
  }
};

}  // namespace donner::css
