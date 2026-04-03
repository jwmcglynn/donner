#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @defgroup xml_feDistantLight "<feDistantLight>"
 *
 * Defines a distant (directional) light source for use within \ref xml_feDiffuseLighting or
 * \ref xml_feSpecularLighting filter primitives.
 *
 * - DOM object: SVGFEDistantLightElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feDistantLightElement
 */

/**
 * DOM object for a \ref xml_feDistantLight element.
 *
 * Represents a light source at infinite distance, producing parallel light rays. The direction
 * is specified by azimuth (angle in the XY plane) and elevation (angle above the XY plane).
 */
class SVGFEDistantLightElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEDistantLightElement(EntityHandle handle) : SVGElement(handle) {}

  static SVGFEDistantLightElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeDistantLight;
  /// XML tag name, \ref xml_feDistantLight.
  static constexpr std::string_view Tag{"feDistantLight"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
