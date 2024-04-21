#pragma once
/// @file

#include <map>
#include <optional>
#include <set>

#include "src/base/rc_string.h"
#include "src/svg/xml/xml_attribute.h"

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

  bool hasAttribute(const XMLAttributeRef& attr) const { return attributes_.count(attr) > 0; }

  std::optional<RcString> getAttribute(const XMLAttributeRef& attr) const {
    const auto it = attributes_.find(attr);
    return (it != attributes_.end()) ? std::make_optional(it->second.value) : std::nullopt;
  }

  void setAttribute(const XMLAttribute& attr, const RcString& value) {
    auto [xmlAttrStorageIt, _inserted] = xmlAttributeStorage_.insert(attr);
    const XMLAttributeRef attrRef = *xmlAttrStorageIt;

    auto [attrIt, newAttrInserted] = attributes_.emplace(attrRef, Storage(attr, value));
    if (!newAttrInserted) {
      attrIt->second.value = value;
    }
  }

  void removeAttribute(const XMLAttributeRef& attr) {
    const auto it = attributes_.find(attr);
    if (it != attributes_.end()) {
      const XMLAttribute attr = it->second.name;
      attributes_.erase(it);

      // Erase the XMLAttribute storage _after_ the attributes map, since the attributes map key
      // takes a reference to the data in XMLAttribute storage.
      xmlAttributeStorage_.erase(attr);
    }
  }

private:
  struct Storage {
    XMLAttribute name;
    RcString value;

    Storage(const XMLAttribute& name, const RcString& value) : name(name), value(value) {}

    /// Move operators.
    Storage(Storage&&) = default;
    Storage& operator=(Storage&&) = default;

    /// No copy.
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    /// Destructor.
    ~Storage() = default;
  };

  std::map<XMLAttributeRef, Storage> attributes_;
  std::set<XMLAttribute> xmlAttributeStorage_;
};

}  // namespace donner::svg::components
