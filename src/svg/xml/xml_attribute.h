#pragma once
/**
 * @file
 *
 * Helper type for an XML attribute name with an optional namespace.
 */

#include "src/base/rc_string.h"

namespace donner::svg {

// Forward declaration.
struct XMLAttributeRef;

/**
 * Represents an XML attribute name with an optional namespace.
 */
struct XMLAttribute {
  RcString namespacePrefix;  //!< The namespace prefix of the attribute, or an empty string if no
                             //!< namespace (default namespace).
  RcString name;             //!< The attribute name.

  /**
   * Construct from an attribute with an empty (default) namespace.
   *
   * @param name The attribute name.
   */
  constexpr XMLAttribute(const RcString& name) : namespacePrefix(), name(name) {}

  /**
   * Construct from an attribute with a namespace prefix.
   *
   * @param namespacePrefix The namespace of the name, or empty if the name belongs to the default
   * namespace.
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
  std::strong_ordering operator<=>(const XMLAttribute& other) const {
    if (namespacePrefix != other.namespacePrefix) {
      return namespacePrefix <=> other.namespacePrefix;
    }
    return name <=> other.name;
  }

  /// Equality operator for gtest.
  bool operator==(const XMLAttribute& other) const = default;

  /// Equality operator for a string, assuming the parameter is lowercase.
  bool equalsIgnoreCase(std::string_view other) const {
    // TODO: How should we support namespaces here?
    return namespacePrefix.empty() && name.equalsIgnoreCase(other);
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const XMLAttribute& obj) {
    if (!obj.namespacePrefix.empty()) {
      os << obj.namespacePrefix << "|";
    }

    os << obj.name;
    return os;
  }

  /// Convert to string operator.
  std::string toString() const {
    std::string str;
    if (!namespacePrefix.empty()) {
      str += namespacePrefix;
      str += "|";
    }

    str += name;
    return str;
  }
};

/**
 * Reference type for \ref XMLAttribute, to pass the value to APIs without needing to allocate an
 * \ref RcString.
 */
struct XMLAttributeRef {
  std::string_view namespacePrefix;  //!< The namespace prefix of the attribute, or an empty string
                                     //!< if no namespace (default namespace).
  std::string_view name;             //!< The attribute name.

  /**
   * Construct from an attribute name as a string_view, assumes no namespacePrefix.
   *
   * @param name The attribute name.
   */
  /* implicit */ XMLAttributeRef(std::string_view name) : namespacePrefix(), name(name) {}

  /**
   * Construct from an attribute name as a const char*, assumes no namespacePrefix.
   *
   * @param name The attribute name.
   */
  /* implicit */ XMLAttributeRef(const char* name) : namespacePrefix(), name(name) {}

  /**
   * Construct from an attribute with a namespace prefix.
   *
   * @param namespacePrefix The namespace of the name, or empty if the name belongs to the default
   * namespace.
   * @param name The attribute name.
   */
  constexpr XMLAttributeRef(std::string_view namespacePrefix, std::string_view name)
      : namespacePrefix(namespacePrefix), name(name) {}

  /**
   * Construct from \ref XMLAttribute.
   */
  /* implicit*/ constexpr XMLAttributeRef(const XMLAttribute& attr)
      : namespacePrefix(attr.namespacePrefix), name(attr.name) {}

  /// Destructor.
  ~XMLAttributeRef() = default;

  // Move and copy constructors.
  XMLAttributeRef(XMLAttributeRef&&) = default;
  XMLAttributeRef(const XMLAttributeRef&) = default;
  XMLAttributeRef& operator=(XMLAttributeRef&&) = default;
  XMLAttributeRef& operator=(const XMLAttributeRef&) = default;

  /// Comparison operator.
  std::strong_ordering operator<=>(const XMLAttributeRef& other) const {
    if (namespacePrefix != other.namespacePrefix) {
      return namespacePrefix <=> other.namespacePrefix;
    }
    return name <=> other.name;
  }

  /// Equality operator for gtest.
  bool operator==(const XMLAttributeRef& other) const = default;

  /// Friend operator for \ref XMLAttribute comparison.
  friend std::strong_ordering operator<=>(const XMLAttributeRef& lhs, const XMLAttribute& rhs) {
    if (lhs.namespacePrefix != rhs.namespacePrefix) {
      return lhs.namespacePrefix <=> rhs.namespacePrefix;
    }
    return lhs.name <=> rhs.name;
  }

  friend std::strong_ordering operator<=>(const XMLAttribute& lhs, const XMLAttributeRef& rhs) {
    if (lhs.namespacePrefix != rhs.namespacePrefix) {
      return lhs.namespacePrefix <=> rhs.namespacePrefix;
    }
    return lhs.name <=> rhs.name;
  }

  /// Friend operator for \ref XMLAttribute equality for gtest.
  friend bool operator==(const XMLAttribute& lhs, const XMLAttributeRef& rhs) {
    return lhs.namespacePrefix == rhs.namespacePrefix && lhs.name == rhs.name;
  }

  friend bool operator==(const XMLAttributeRef& lhs, const XMLAttribute& rhs) {
    return lhs.namespacePrefix == rhs.namespacePrefix && lhs.name == rhs.name;
  }
};

}  // namespace donner::svg

/**
 * Hash function for \ref XMLAttributeRef.
 */
template <>
struct std::hash<donner::svg::XMLAttributeRef> {
  /**
   * Hash function for \ref XMLAttribute.
   *
   * @param str Input attribute.
   * @return std::size_t Output hash.
   */
  std::size_t operator()(const donner::svg::XMLAttributeRef& attr) const {
    std::size_t hash = 0;
    hash ^= std::hash<std::string_view>()(attr.namespacePrefix);
    hash ^= std::hash<std::string_view>()(attr.name);
    return hash;
  }
};

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
    return std::hash<donner::svg::XMLAttributeRef>()(attr);
  }
};
