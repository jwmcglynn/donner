#pragma once
/// @file

#include <map>
#include <optional>
#include <set>

#include "src/base/rc_string.h"
#include "src/svg/xml/xml_qualified_name.h"

namespace donner::svg::components {

struct AttributesComponent {
  AttributesComponent() = default;

  // No copy, move-only.
  AttributesComponent(const AttributesComponent&) = delete;
  AttributesComponent& operator=(const AttributesComponent&) = delete;
  AttributesComponent(AttributesComponent&&) = default;
  AttributesComponent& operator=(AttributesComponent&&) = default;

  /// Destructor.
  ~AttributesComponent() = default;

  bool hasAttribute(const XMLQualifiedNameRef& name) const { return attributes_.count(name) > 0; }

  std::optional<RcString> getAttribute(const XMLQualifiedNameRef& name) const {
    const auto it = attributes_.find(name);
    return (it != attributes_.end()) ? std::make_optional(it->second.value) : std::nullopt;
  }

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
    XMLQualifiedName name;
    RcString value;

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

  std::map<XMLQualifiedNameRef, Storage> attributes_;
  std::set<XMLQualifiedName> attrNameStorage_;
};

}  // namespace donner::svg::components
