#pragma once
/// @file

#include <map>
#include <optional>

#include "src/base/rc_string.h"
#include "src/svg/xml/xml_attribute.h"

namespace donner::svg::components {

struct AttributesComponent {
  bool hasAttribute(const XMLAttribute& attr) const { return attributes.count(attr) > 0; }

  std::optional<RcString> getAttribute(const XMLAttribute& attr) const {
    const auto it = attributes.find(attr);
    return (it != attributes.end()) ? std::make_optional(it->second) : std::nullopt;
  }

  void setAttribute(const XMLAttribute& attr, const RcString& value) { attributes[attr] = value; }

  void removeAttribute(const XMLAttribute& attr) { attributes.erase(attr); }

  std::map<XMLAttribute, RcString> attributes;
};

}  // namespace donner::svg::components
