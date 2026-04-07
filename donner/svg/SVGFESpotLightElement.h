#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @defgroup xml_feSpotLight "<feSpotLight>"
 *
 * Defines a spotlight source for use within \ref xml_feDiffuseLighting or
 * \ref xml_feSpecularLighting filter primitives.
 *
 * - DOM object: SVGFESpotLightElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feSpotLightElement
 */

/**
 * DOM object for a \ref xml_feSpotLight element.
 *
 * Represents a spotlight at a specific position, pointing toward a target point. The light
 * intensity falls off based on the angle from the light's direction axis, controlled by
 * specularExponent and limitingConeAngle.
 */
class SVGFESpotLightElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFESpotLightElement(EntityHandle handle) : SVGElement(handle) {}

  static SVGFESpotLightElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeSpotLight;
  /// XML tag name, \ref xml_feSpotLight.
  static constexpr std::string_view Tag{"feSpotLight"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
