#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @defgroup xml_feSpecularLighting "<feSpecularLighting>"
 *
 * Lights an image using the alpha channel as a bump map, computing the specular component of
 * the Phong lighting model.
 *
 * - DOM object: SVGFESpecularLightingElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feSpecularLightingElement
 *
 * This element is defined within a \ref xml_filter element, and must contain exactly one
 * light source child element (\ref xml_feDistantLight, \ref xml_fePointLight, or \ref
 * xml_feSpotLight).
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feSpecularLighting surfaceScale="5" specularConstant="1" specularExponent="20">
 *     <feDistantLight azimuth="45" elevation="55" />
 *   </feSpecularLighting>
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feSpecularLighting element.
 *
 * Computes specular lighting using the input's alpha channel as a height map. Unlike
 * feDiffuseLighting, the output is non-opaque and intended to be composited with the
 * original image using feComposite with operator="arithmetic".
 */
class SVGFESpecularLightingElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFESpecularLightingElement wrapper from an entity.
  explicit SVGFESpecularLightingElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFESpecularLightingElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeSpecularLighting;
  /// XML tag name, \ref xml_feSpecularLighting.
  static constexpr std::string_view Tag{"feSpecularLighting"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
