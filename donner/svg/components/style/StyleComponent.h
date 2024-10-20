#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg::components {

/**
 * Contains properties the `style=""` attribute for an element, local to the element. This is used
 * during the CSS cascade, which stores the final element style in \ref ComputedStyleComponent.
 */
struct StyleComponent {
  /// The properties of the element, which are parsed from the `style=""` attribute.
  PropertyRegistry properties;

  /**
   * Sets the properties from the value of the element's `style=""` attribute. Note that this
   * applies the style additively, and does not invalidate the existing style.
   *
   * @todo Add an option to clear the existing style first. For now, this can be done by setting
   * \ref properties to an empty \ref PropertyRegistry.
   *
   * @param style The value of the `style` attribute.
   */
  void setStyle(std::string_view style) { properties.parseStyle(style); }

  /**
   * Tries to set a presentation attribute on the given entity. This is used during the CSS cascade
   * to apply the computed style to the element.
   *
   * @param handle The entity to set the attribute on.
   * @param name The name of the attribute.
   * @param value The value of the attribute.
   * @return \c true if the attribute was set, \c false if the attribute was not recognized.
   */
  parser::ParseResult<bool> trySetPresentationAttribute(EntityHandle handle, std::string_view name,
                                                        std::string_view value) {
    return properties.parsePresentationAttribute(name, value,
                                                 handle.get<ElementTypeComponent>().type(), handle);
  }
};

}  // namespace donner::svg::components
