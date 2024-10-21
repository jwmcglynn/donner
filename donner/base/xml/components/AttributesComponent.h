#pragma once
/// @file

#include <map>
#include <optional>
#include <set>

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcStringOrRef.h"
#include "donner/base/SmallVector.h"
#include "donner/base/xml/XMLQualifiedName.h"

namespace donner::components {

/**
 * Stores XML attribute values.
 */
struct AttributesComponent {
  /// Constructor.
  AttributesComponent() = default;

  // No copy, move-only.
  AttributesComponent(const AttributesComponent&) = delete;
  AttributesComponent& operator=(const AttributesComponent&) = delete;
  /// Move constructor.
  AttributesComponent(AttributesComponent&&) = default;
  /// Move assignment operator.
  AttributesComponent& operator=(AttributesComponent&&) = default;

  /// Destructor.
  ~AttributesComponent() = default;

  /**
   * Returns true if the element has an attribute with the given name.
   *
   * @param name Name of the attribute to check.
   * @return true if the attribute exists, false otherwise.
   */
  bool hasAttribute(const xml::XMLQualifiedNameRef& name) const {
    return attributes_.count(name) > 0;
  }

  /**
   * Get the value of an attribute, if it exists.
   *
   * @param name Name of the attribute to get.
   * @return The value of the attribute, or std::nullopt if the attribute does not exist.
   */
  std::optional<RcString> getAttribute(const xml::XMLQualifiedNameRef& name) const {
    const auto it = attributes_.find(name);
    return (it != attributes_.end()) ? std::make_optional(it->second.value) : std::nullopt;
  }

  /**
   * Get a list of all attributes.
   *
   * @param name Name of the attribute to get.
   * @return The value of the attribute, or std::nullopt if the attribute does not exist.
   */
  SmallVector<xml::XMLQualifiedNameRef, 10> attributes() const {
    SmallVector<xml::XMLQualifiedNameRef, 10> result;
    for (const auto& [name, _] : attributes_) {
      result.push_back(name);
    }
    return result;
  }

  /**
   * Find attributes matching the given name matcher.
   *
   * @param matcher Matcher to use to find attributes. If \ref XMLQualifiedNameRef::namespacePrefix
   * is "*", the matcher will match any namespace with the given attribute name.
   * @return A vector of attributes matching the given name matcher.
   */
  SmallVector<xml::XMLQualifiedNameRef, 1> findMatchingAttributes(
      const xml::XMLQualifiedNameRef& matcher) const;

  /**
   * Set the value of a generic XML attribute, which may be either a presentation attribute or
   * custom user-provided attribute.
   *
   * This API only stores the underlying strings for the attribute name and value, and does not
   * parse them. To parse, use the upper-layer API: \ref SVGElement::setAttribute.
   *
   * @param registry Registry to use for the operation.
   * @param name Name of the attribute to set.
   * @param value New value to set.
   */
  void setAttribute(Registry& registry, const xml::XMLQualifiedNameRef& name,
                    const RcString& value);

  /**
   * Remove an attribute from the element.
   *
   * @param registry Registry to use for the operation.
   * @param name Name of the attribute to remove.
   */
  void removeAttribute(Registry& registry, const xml::XMLQualifiedNameRef& name);

  /// Returns true if the element has any namespace overrides.
  bool hasNamespaceOverrides() const { return numNamespaceOverrides_ > 0; }

private:
  /**
   * Returns true if the given name is a namespace override.
   *
   * @param name Name to check.
   * @return true if the name is a namespace override, false otherwise.
   */
  bool isNamespaceOverride(const xml::XMLQualifiedNameRef& name) const {
    return name.namespacePrefix == "xmlns" || name == "xmlns";
  }

  /// Storage for attribute name and value.
  struct Storage {
    xml::XMLQualifiedName name;  ///< Name of the attribute.
    RcString value;              ///< Value of the attribute.

    /// Constructor.
    Storage(const xml::XMLQualifiedName& name, const RcString& value) : name(name), value(value) {}

    /// Move operators.
    Storage(Storage&&) = default;
    Storage& operator=(Storage&&) = default;

    /// No copy.
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    /// Destructor.
    ~Storage() = default;
  };

  /// Map of attribute name to value, note that the key values are references to the value in \ref
  /// attrNameStorage_.
  std::map<xml::XMLQualifiedNameRef, Storage> attributes_;
  std::set<xml::XMLQualifiedName> attrNameStorage_;  ///< Storage for XMLQualifiedName.
  int numNamespaceOverrides_ = 0;                    ///< Number of namespace overrides.
};

}  // namespace donner::components
