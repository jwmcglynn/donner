#pragma once
/// @file

#include "donner/base/element/ElementLike.h"
#include "donner/base/xml/XMLQualifiedName.h"

namespace donner::css {

/**
 * Selector which matches the element type, e.g. `div` or `circle`.  The selector may also include a
 * namespace, or be a universal selector.
 *
 * In the CSS source, this is represented by either a standalone type, or namespace and type
 * separated by a pipe (`|`). Either the namespace or the type may be a wildcard (`*`).
 * - `type`
 * - `*`
 * - `ns|type`
 * - `ns|*`
 * - `*|type`
 *
 * TypeSelector represents the parsed representation, if no namespace is provided it will be an
 * empty string.
 */
struct TypeSelector {
  /**
   * Selector matcher itself, which may contain wildcards.
   *
   * In this context, the members have the following meanings:
   * - \ref XMLQualifiedName::namespacePrefix The namespace matcher of the selector, the
   * wildcard namespace ("*"), or empty if no namespace is specified.
   * - \ref XMLQualifiedName::name The name matcher of the selector, or "*" if the selector is
   * a universal selector.
   */
  XMLQualifiedName matcher;

  /**
   * Create a TypeSelector with the given namespace and name.
   *
   * @param matcher Selector matcher, which may be a wildcard. If the namespace is "*", it will
   * match every namespaces. If the name is "*", it will match every attribute in its namespace.
   */
  TypeSelector(XMLQualifiedName&& matcher) : matcher(std::move(matcher)) {}

  /**
   * Create a TypeSelector with the given namespace and name.
   *
   * @param matcher Selector matcher, which may be a wildcard. If the namespace is "*", it will
   * match every namespaces. If the name is "*", it will match every attribute in its namespace.
   */
  TypeSelector(const XMLQualifiedNameRef& matcher)
      : matcher(RcString(matcher.namespacePrefix), RcString(matcher.name)) {}

  /// Destructor.
  ~TypeSelector() noexcept = default;

  /// Moveable and copyable.
  TypeSelector(TypeSelector&&) = default;
  TypeSelector& operator=(TypeSelector&&) = default;
  TypeSelector(const TypeSelector&) = default;
  TypeSelector& operator=(const TypeSelector&) = default;

  /// Returns true if this is a universal selector.
  bool isUniversal() const { return matcher.name == "*"; }

  /// Returns true if this is a valid selector.
  bool isValid() const {
    // TODO: Error out if the namespace has not been registered.
    return true;
  }

  /**
   * Returns true if the provided element matches this selector.
   *
   * @param element The element to check.
   */
  template <ElementLike T>
  bool matches(const T& element) const {
    const XMLQualifiedNameRef elementName = element.tagName();

    // Match namespace.
    const bool namespaceMatched =
        (matcher.namespacePrefix == "*" ||
         matcher.namespacePrefix.equalsIgnoreCase(elementName.namespacePrefix));
    if (!namespaceMatched) {
      return false;
    }

    // Match type name.
    if (isUniversal()) {
      return true;
    }

    return matcher.name.equalsIgnoreCase(elementName.name);
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const TypeSelector& obj) {
    return os << "TypeSelector(" << obj.matcher << ")";
  }
};

}  // namespace donner::css
