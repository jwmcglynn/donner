#pragma once
/**
 * @file
 *
 * Helper type for an XML attribute name with an optional namespace.
 */

#include <ostream>

#include "donner/base/RcString.h"
#include "donner/base/RcStringOrRef.h"

namespace donner {

// Forward declaration, implemented later in the file.
struct XMLQualifiedNameRef;

/**
 * Represents an XML attribute name with an optional namespace.
 */
struct XMLQualifiedName {
  RcString namespacePrefix;  //!< The namespace prefix of the attribute, or an empty string if no
                             //!< namespace (default namespace).
  RcString name;             //!< The attribute name.

  /**
   * Construct from an attribute with an empty (default) namespace.
   *
   * @param name The attribute name.
   */
  constexpr XMLQualifiedName(const RcString& name) : namespacePrefix(), name(name) {}

  /**
   * Construct from an attribute with a namespace prefix.
   *
   * @param namespacePrefix The namespace of the name, or empty if the name belongs to the default
   * namespace.
   * @param name The attribute name.
   */
  constexpr XMLQualifiedName(const RcString& namespacePrefix, const RcString& name)
      : namespacePrefix(namespacePrefix), name(name) {}

  /// Destructor.
  ~XMLQualifiedName() = default;

  // Move and copy constructors.
  /// Move constructor.
  XMLQualifiedName(XMLQualifiedName&&) = default;
  /// Copy constructor.
  XMLQualifiedName(const XMLQualifiedName&) = default;
  /// Move assignment operator.
  XMLQualifiedName& operator=(XMLQualifiedName&&) = default;
  /// Copy assignment operator.
  XMLQualifiedName& operator=(const XMLQualifiedName&) = default;

  /// Comparison operator.
  std::strong_ordering operator<=>(const XMLQualifiedName& other) const {
    if (name != other.name) {
      return name <=> other.name;
    }
    return namespacePrefix <=> other.namespacePrefix;
  }

  /// Equality operator for gtest.
  bool operator==(const XMLQualifiedName& other) const = default;

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const XMLQualifiedName& obj) {
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
 * Reference type for \ref XMLQualifiedName, to pass the value to APIs without needing to allocate
 * an \ref RcString.
 */
struct XMLQualifiedNameRef {
  RcStringOrRef namespacePrefix;  //!< The namespace prefix of the attribute, or an empty string
                                  //!< if no namespace (default namespace).
  RcStringOrRef name;             //!< The attribute name.

  /**
   * Construct from an attribute name as a string_view, assumes no namespacePrefix.
   *
   * @param name The attribute name.
   */
  /* implicit */ constexpr XMLQualifiedNameRef(const RcStringOrRef& name)
      : namespacePrefix(), name(name) {}

  /**
   * Construct from an attribute name as a const char*, assumes no namespacePrefix.
   *
   * @param name The attribute name.
   */
  /* implicit */ constexpr XMLQualifiedNameRef(const char* name) : namespacePrefix(), name(name) {}

  /**
   * Construct from an attribute name as a \c std::string_view, assumes no namespacePrefix.
   *
   * @param name The attribute name.
   */
  /* implicit */ constexpr XMLQualifiedNameRef(std::string_view name)
      : namespacePrefix(), name(name) {}

  /**
   * Construct from an attribute with a namespace prefix.
   *
   * @param namespacePrefix The namespace of the name, or empty if the name belongs to the default
   * namespace.
   * @param name The attribute name.
   */
  constexpr XMLQualifiedNameRef(const RcStringOrRef& namespacePrefix, const RcStringOrRef& name)
      : namespacePrefix(namespacePrefix), name(name) {}

  /**
   * Construct from \ref XMLQualifiedName.
   */
  /* implicit*/ constexpr XMLQualifiedNameRef(const XMLQualifiedName& attr)
      : namespacePrefix(attr.namespacePrefix), name(attr.name) {}

  /// Destructor.
  ~XMLQualifiedNameRef() = default;

  // Move and copy constructors.
  /// Move constructor.
  XMLQualifiedNameRef(XMLQualifiedNameRef&&) = default;
  /// Copy constructor.
  XMLQualifiedNameRef(const XMLQualifiedNameRef&) = default;
  /// Move assignment operator.
  XMLQualifiedNameRef& operator=(XMLQualifiedNameRef&&) = default;
  /// Copy assignment operator.
  XMLQualifiedNameRef& operator=(const XMLQualifiedNameRef&) = default;

  /// Comparison operator.
  std::strong_ordering operator<=>(const XMLQualifiedNameRef& other) const {
    if (name != other.name) {
      return name <=> other.name;
    }

    return namespacePrefix <=> other.namespacePrefix;
  }

  /// Equality operator for gtest.
  bool operator==(const XMLQualifiedNameRef& other) const = default;

  /// Friend operator for \ref XMLQualifiedName comparison.
  friend std::strong_ordering operator<=>(const XMLQualifiedNameRef& lhs,
                                          const XMLQualifiedName& rhs) {
    if (lhs.name != rhs.name) {
      return lhs.name <=> rhs.name;
    }
    return lhs.namespacePrefix <=> rhs.namespacePrefix;
  }

  /// Friend operator for \ref XMLQualifiedName comparison.
  friend std::strong_ordering operator<=>(const XMLQualifiedName& lhs,
                                          const XMLQualifiedNameRef& rhs) {
    if (lhs.name != rhs.name) {
      return lhs.name <=> rhs.name;
    }
    return lhs.namespacePrefix <=> rhs.namespacePrefix;
  }

  /// Friend operator for \ref XMLQualifiedName equality for gtest.
  friend bool operator==(const XMLQualifiedName& lhs, const XMLQualifiedNameRef& rhs) {
    return lhs.name == rhs.name && lhs.namespacePrefix == rhs.namespacePrefix;
  }

  /// Friend operator for \ref XMLQualifiedName equality for gtest.
  friend bool operator==(const XMLQualifiedNameRef& lhs, const XMLQualifiedName& rhs) {
    return lhs.name == rhs.name && lhs.namespacePrefix == rhs.namespacePrefix;
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const XMLQualifiedNameRef& obj) {
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

}  // namespace donner

/**
 * Hash function for \ref donner::XMLQualifiedNameRef.
 */
template <>
struct std::hash<donner::XMLQualifiedNameRef> {
  /**
   * Hash function for \ref donner::XMLQualifiedName.
   *
   * @param attr Input attribute.
   * @return std::size_t Output hash.
   */
  std::size_t operator()(const donner::XMLQualifiedNameRef& attr) const {
    std::size_t hash = 0;
    hash ^= std::hash<std::string_view>()(attr.namespacePrefix);
    hash ^= std::hash<std::string_view>()(attr.name);
    return hash;
  }
};

/**
 * Hash function for \ref donner::XMLQualifiedName.
 */
template <>
struct std::hash<donner::XMLQualifiedName> {
  /**
   * Hash function for \ref donner::XMLQualifiedName.
   *
   * @param attr Input attribute.
   * @return std::size_t Output hash.
   */
  std::size_t operator()(const donner::XMLQualifiedName& attr) const {
    return std::hash<donner::XMLQualifiedNameRef>()(attr);
  }
};
