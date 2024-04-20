#pragma once
/**
 * @file
 *
 * Helper type for an XML attribute name with an optional namespace.
 */

#include "src/base/rc_string.h"

namespace donner::svg {

/**
 * Represents an XML attribute name with an optional namespace.
 */
struct XMLAttribute {
  RcString namespacePrefix;  //!< The namespace prefix of the attribute, or an empty string if no
                             //!< namespace.
  RcString name;             //!< The attribute name.

  /**
   * Construct from an attribute name, assumes no namespacePrefix.
   *
   * @param name The attribute name.
   */
  /* implicit */ constexpr XMLAttribute(const RcString& name) : namespacePrefix(), name(name) {}

  /**
   * Construct from an attribute name as a string_view, assumes no namespacePrefix.
   *
   * @param name The attribute name.
   */
  /* implicit */ XMLAttribute(std::string_view name) : namespacePrefix(), name(RcString(name)) {}

  /**
   * Construct from an attribute name as a const char*, assumes no namespacePrefix.
   *
   * @param name The attribute name.
   */
  /* implicit */ XMLAttribute(const char* name) : namespacePrefix(), name(RcString(name)) {}

  /**
   * Construct from an attribute with a namespace prefix.
   *
   * @param namespacePrefix The namespace prefix of the attribute.
   * @param name The attribute name.
   */
  constexpr XMLAttribute(const RcString& namespacePrefix, const RcString& name)
      : namespacePrefix(namespacePrefix), name(name) {}

  /// Destructor.
  ~XMLAttribute() = default;

  // Move and copy constructors.
  XMLAttribute(XMLAttribute&&) = default;
  XMLAttribute(const XMLAttribute&) = default;
  XMLAttribute& operator=(XMLAttribute&&) = default;
  XMLAttribute& operator=(const XMLAttribute&) = default;

  /// Comparison operator.
  auto operator<=>(const XMLAttribute& other) const {
    if (namespacePrefix != other.namespacePrefix) {
      return namespacePrefix <=> other.namespacePrefix;
    }
    return name <=> other.name;
  }

  /// Equality operator.
  bool operator==(const XMLAttribute& other) const = default;
};

}  // namespace donner::svg

/**
 * Hash function for \ref XMLAttribute.
 */
template <>
struct std::hash<donner::svg::XMLAttribute> {
  /**
   * Hash function for \ref XMLAttribute.
   *
   * @param str Input attribute.
   * @return std::size_t Output hash.
   */
  std::size_t operator()(const donner::svg::XMLAttribute& attr) const {
    std::size_t hash = 0;
    hash ^= std::hash<donner::RcString>()(attr.namespacePrefix);
    hash ^= std::hash<donner::RcString>()(attr.name);
    return hash;
  }
};
