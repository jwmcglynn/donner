#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/css/SelectorTraversal.h"

namespace donner::css {

/**
 * Selector which match the element's `class` attribute, for example `.foo` matches an element with
 * class `foo`.
 *
 * See https://www.w3.org/TR/selectors-4/#class-html for the full definition.
 */
struct ClassSelector {
  RcString name;  ///< The class to match, without the leading `.`.

  /**
   * Create a ClassSelector with the given name.
   *
   * @param name The class to match, without the leading `.`.
   */
  explicit ClassSelector(const RcString& name) : name(name) {}

  /// Destructor.
  ~ClassSelector() noexcept = default;

  /// Moveable and copyable.
  ClassSelector(ClassSelector&&) = default;
  ClassSelector& operator=(ClassSelector&&) = default;
  ClassSelector(const ClassSelector&) = default;
  ClassSelector& operator=(const ClassSelector&) = default;

  /// Returns true if this is a valid selector.
  bool isValid() const { return true; }

  /**
   * Returns true if the provided element matches this selector, based on if the element's `class`
   * attribute's whitespace-separated list of classes exactly contains this selector's name.
   *
   * Comparison is case-sensitive.
   *
   * @param element The element to check.
   */
  template <traversal::ElementLike T>
  bool matches(const T& element) const {
    // Matching is equivalent to `[class=~name]`.

    // Returns true if attribute value is a whitespace-separated list of values, and one of them
    // exactly matches the matcher value.
    const RcString className = element.className();
    for (auto str : StringUtils::Split(className, ' ')) {
      if (str == name) {
        return true;
      }
    }

    return false;
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const ClassSelector& obj) {
    os << "ClassSelector(" << obj.name << ")";
    return os;
  }
};

}  // namespace donner::css
