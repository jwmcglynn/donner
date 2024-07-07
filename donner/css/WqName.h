#pragma once
/// @file

#include <ostream>

#include "donner/base/xml/XMLQualifiedName.h"

namespace donner::css {

/**
 * A CSS qualified name, which is a name optionally associated with a namespace. See
 * https://www.w3.org/TR/selectors-4/#type-nmsp for the full definition.
 *
 * For example, the following are all valid qualified names:
 * - `foo`, represents the name `foo` which belongs to the default namespace.
 * - `|foo`, represents the name `foo` which belongs to no namespace.
 * - `ns|foo`, represents the name `foo` which belongs to the namespace `ns`.
 * - `*|foo`, represents the name `foo` which belongs to any namespace.
 */
struct WqName {
  XMLQualifiedName name;

  /**
   * Create a WqName with the given namespace and name.
   *
   * @param name The name with optional namespace
   */
  WqName(XMLQualifiedName&& name) : name(std::move(name)) {}

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const WqName& obj) { return os << obj.name; }
};

}  // namespace donner::css
