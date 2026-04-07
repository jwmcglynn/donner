#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @defgroup xml_fePointLight "<fePointLight>"
 *
 * Defines a point light source for use within \ref xml_feDiffuseLighting or
 * \ref xml_feSpecularLighting filter primitives.
 *
 * - DOM object: SVGFEPointLightElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#fePointLightElement
 */

/**
 * DOM object for a \ref xml_fePointLight element.
 *
 * Represents a light source at a specific position in 3D space. Light direction varies
 * per pixel based on the distance from the light to the surface point.
 */
class SVGFEPointLightElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEPointLightElement(EntityHandle handle) : SVGElement(handle) {}

  static SVGFEPointLightElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FePointLight;
  /// XML tag name, \ref xml_fePointLight.
  static constexpr std::string_view Tag{"fePointLight"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
