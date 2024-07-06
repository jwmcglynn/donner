#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/base/element/ElementLike.h"

namespace donner::css {

/**
 * Selector which match the element's `id` attribute, for example `#foo` matches an element with
 * an `id="foo"` attribute.
 *
 * See https://www.w3.org/TR/selectors-4/#id-selectors for the full definition.
 */
struct IdSelector {
  RcString name;  ///< The id to match, without the leading `#`.

  /**
   * Create an IdSelector with the given name.
   *
   * @param name The id to match, without the leading `#`.
   */
  explicit IdSelector(const RcString& name) : name(name) {}

  /// Destructor.
  ~IdSelector() noexcept = default;

  /// Moveable and copyable.
  IdSelector(IdSelector&&) = default;
  IdSelector& operator=(IdSelector&&) = default;
  IdSelector(const IdSelector&) = default;
  IdSelector& operator=(const IdSelector&) = default;

  /// Returns true if this is a valid selector.
  bool isValid() const { return true; }

  /**
   * Returns true if the provided element matches this selector, based on a case-sensitive match
   * of the provided id against the element's `id` attribute.
   *
   * @param element The element to check.
   */
  template <ElementLike T>
  bool matches(const T& element) const {
    return element.id() == name;
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const IdSelector& obj) {
    os << "IdSelector(" << obj.name << ")";
    return os;
  }
};

}  // namespace donner::css
