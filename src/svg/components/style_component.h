#pragma once

#include "src/svg/components/registry.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/properties/property_registry.h"

namespace donner {

struct StyleComponent : public HandleOfMixin<StyleComponent> {
  svg::PropertyRegistry properties;

  void setStyle(std::string_view style) { properties.parseStyle(style); }
  bool trySetPresentationAttribute(Registry& registry, std::string_view name,
                                   std::string_view value) {
    auto handle = handleOf(registry);
    return properties.parsePresentationAttribute(name, value, handle.get<TreeComponent>().type(),
                                                 handle);
  }
};

}  // namespace donner
