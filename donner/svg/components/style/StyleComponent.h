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
   * Sets the properties from the value of the element's `style=""` attribute, replacing the
   * prior `style=""` contribution on this element. Presentation-attribute-origin properties
   * (e.g. `fill="red"`) and author-stylesheet properties are left intact — the replacement
   * is scoped to declarations previously tagged with
   * \ref css::Specificity::StyleAttribute().
   *
   * Safe to call from the SVG parse pipeline (where presentation attributes may already
   * be in the registry) and from the editor's text-edit rewrite path.
   *
   * Callers that want to *merge* new declarations into an existing `style=""` — e.g. a
   * tool that toggles one property — should use \ref updateStyle instead.
   *
   * @param style The value of the `style` attribute.
   */
  void setStyle(std::string_view style) {
    properties.clearStyleAttributeProperties();
    properties.parseStyle(style);
  }

  /**
   * Updates the properties from the value of the element's `style=""` attribute, adding new
   * properties or updating existing ones. Does not remove existing `style=""` attribute
   * information.
   *
   * @param style The update style to apply, as a CSS style string (e.g. "fill:red;").
   */
  void updateStyle(std::string_view style) { properties.parseStyle(style); }

  /**
   * Tries to set a common presentation attribute (fill, stroke, opacity, transform, etc.).
   *
   * For element-specific attributes (cx, cy, r, d, etc.), callers should also invoke
   * \c parser::ParsePresentationAttribute separately.
   *
   * @param handle The entity to set the attribute on.
   * @param name The name of the attribute.
   * @param value The value of the attribute.
   * @return \c true if the attribute was set, \c false if the attribute was not recognized.
   */
  ParseResult<bool> trySetPresentationAttribute(EntityHandle handle, std::string_view name,
                                                std::string_view value) {
    return properties.parsePresentationAttribute(name, value, handle);
  }
};

}  // namespace donner::svg::components
