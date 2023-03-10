#pragma once
/// @file

#include "src/svg/components/tree_component.h"
#include "src/svg/properties/property_registry.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

struct StyleComponent {
  PropertyRegistry properties;

  void setStyle(std::string_view style) { properties.parseStyle(style); }
  ParseResult<bool> trySetPresentationAttribute(EntityHandle handle, std::string_view name,
                                                std::string_view value) {
    return properties.parsePresentationAttribute(name, value, handle.get<TreeComponent>().type(),
                                                 handle);
  }
};

/**
 * This component is added to entities to indicate that 'fill' and 'stroke' attributes should not be
 * inherited, which is used for <pattern> because it establishes a shadow tree, and we do not want
 * to recursively inherit 'fill' or 'stroke' values into the children.
 */
struct DoNotInheritFillOrStrokeTag {};

}  // namespace donner::svg
