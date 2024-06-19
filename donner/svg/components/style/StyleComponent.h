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

/**
 * This component is added to entities to indicate that 'fill' and 'stroke' attributes should not be
 * inherited, which is used for \ref xml_pattern because it establishes a shadow tree, and we do not
 * want to recursively inherit 'fill' or 'stroke' values into the children.
 */
struct DoNotInheritFillOrStrokeTag {};

}  // namespace donner::svg::components
