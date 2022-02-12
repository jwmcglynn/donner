#pragma once

#include "src/svg/components/registry.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/properties/property_registry.h"

namespace donner::svg {

struct StyleComponent : public HandleOfMixin<StyleComponent> {
  PropertyRegistry properties;

  void setStyle(std::string_view style) { properties.parseStyle(style); }
  bool trySetPresentationAttribute(Registry& registry, std::string_view name,
                                   std::string_view value) {
    auto handle = handleOf(registry);
    return properties.parsePresentationAttribute(name, value, handle.get<TreeComponent>().type(),
                                                 handle);
  }
};

}  // namespace donner::svg
