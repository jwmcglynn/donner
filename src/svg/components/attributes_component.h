#pragma once
/// @file

#include <map>
#include <optional>
#include <set>

#include "src/base/rc_string_or_ref.h"
#include "src/svg/xml/xml_qualified_name.h"

namespace donner::svg::components {

/**
 * Stores XML attribute values.
 */
struct AttributesComponent {
  /// Constructor.
  AttributesComponent() = default;

  // No copy, move-only.
  AttributesComponent(const AttributesComponent&) = delete;
  AttributesComponent& operator=(const AttributesComponent&) = delete;
  AttributesComponent(AttributesComponent&&) = default;
  AttributesComponent& operator=(AttributesComponent&&) = default;

  /// Destructor.
  ~AttributesComponent() = default;

  /**
   * Returns true if the element has an attribute with the given name.
   *
   * @param name Name of the attribute to check.
   * @return true if the attribute exists, false otherwise.
   */
  bool hasAttribute(const XMLQualifiedNameRef& name) const { return attributes_.count(name) > 0; }

  /**
   * Get the value of an attribute, if it exists.
   *
   * @param name Name of the attribute to get.
   * @return The value of the attribute, or std::nullopt if the attribute does not exist.
   */
  std::optional<RcString> getAttribute(const XMLQualifiedNameRef& name) const {
    const auto it = attributes_.find(name);
    return (it != attributes_.end()) ? std::make_optional(it->second.value) : std::nullopt;
  }

  /**
   * Find attributes matching the given name matcher.
   *
   * @param matcher Matcher to use to find attributes. If \ref XMLQualifiedNameRef::namespacePrefix
   * is "*", the matcher will match any namespace with the given attribute name.
   * @return A vector of attributes matching the given name matcher.
   */
  std::vector<XMLQualifiedNameRef> findMatchingAttributes(
      const XMLQualifiedNameRef& matcher) const {
    std::vector<XMLQualifiedNameRef> result;

    if (matcher.namespacePrefix == "*") {
      const XMLQualifiedNameRef attributeNameOnly(matcher.name);

      for (auto it = attributes_.lower_bound(attributeNameOnly); it != attributes_.end(); ++it) {
        if (StringUtils::Equals<StringComparison::IgnoreCase>(it->first.name, matcher.name)) {
          result.push_back(it->first);
        } else {
          break;
        }
      }
    } else if (attributes_.count(matcher)) {
      result.push_back(matcher);
    }

    return result;
  }

  /**
   * Set the value of a generic XML attribute, which may be either a presentation attribute or
   * custom user-provided attribute.
   *
   * This API supports a superset of \ref trySetPresentationAttribute, however its parse errors are
   * ignored. If the attribute is not a presentation attribute, or there are parse errors the
   * attribute will be stored as a custom attribute instead.
   *
   * @param name Name of the attribute to set.
   * @param value New value to set.
   */
  void setAttribute(const XMLQualifiedNameRef& name, const RcString& value) {
    XMLQualifiedName nameAllocated(RcString(name.namespacePrefix), RcString(name.name));

    auto [xmlAttrStorageIt, _inserted] = attrNameStorage_.insert(nameAllocated);
    const XMLQualifiedNameRef attrRef = *xmlAttrStorageIt;

    auto [attrIt, newAttrInserted] = attributes_.emplace(attrRef, Storage(nameAllocated, value));
    if (!newAttrInserted) {
      attrIt->second.value = value;
    }
  }

  void removeAttribute(const XMLQualifiedNameRef& name) {
    const auto it = attributes_.find(name);
    if (it != attributes_.end()) {
      const XMLQualifiedName attrToRemove = std::move(it->second.name);
      attributes_.erase(it);

      // Erase the XMLQualifiedName storage _after_ the attributes map, since the attributes map key
      // takes a reference to the data in XMLQualifiedName storage.
      attrNameStorage_.erase(attrToRemove);
    }
  }

private:
  struct Storage {
    XMLQualifiedName name;  ///< Name of the attribute.
    RcString value;         ///< Value of the attribute.

    /// Constructor.
    Storage(const XMLQualifiedName& name, const RcString& value) : name(name), value(value) {}

    /// Move operators.
    Storage(Storage&&) = default;
    Storage& operator=(Storage&&) = default;

    /// No copy.
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    /// Destructor.
    ~Storage() = default;
  };

  std::map<XMLQualifiedNameRef, Storage>
      attributes_;  ///< Map of attribute name to value, note that the key values are references to
                    ///< the value in \ref attrNameStorage_.
  std::set<XMLQualifiedName> attrNameStorage_;  ///< Storage for XMLQualifiedName.
};

}  // namespace donner::svg::components
