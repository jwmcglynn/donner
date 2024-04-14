#pragma once
/// @file

#include <map>
#include <optional>

#include "src/base/rc_string.h"

namespace donner::svg::components {

struct AttributesComponent {
  bool hasAttribute(const RcString& name) const { return attributes.count(name) > 0; }

  std::optional<RcString> getAttribute(const RcString& name) const {
    const auto it = attributes.find(name);
    return (it != attributes.end()) ? std::make_optional(it->second) : std::nullopt;
  }

  void setAttribute(const RcString& name, const RcString& value) { attributes[name] = value; }

  void removeAttribute(const RcString& name) { attributes.erase(name); }

  std::map<RcString, RcString> attributes;
};

}  // namespace donner::svg::components
