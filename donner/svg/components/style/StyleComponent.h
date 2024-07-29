#pragma once
/// @file

#include "donner/svg/components/TreeComponent.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

struct StyleComponent {
  PropertyRegistry properties;

  void setStyle(std::string_view style) { properties.parseStyle(style); }
  parser::ParseResult<bool> trySetPresentationAttribute(EntityHandle handle, std::string_view name,
                                                        std::string_view value) {
    return properties.parsePresentationAttribute(name, value, handle.get<TreeComponent>().type(),
                                                 handle);
  }
};

}  // namespace donner::svg::components
