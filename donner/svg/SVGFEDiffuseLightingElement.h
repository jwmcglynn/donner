#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @defgroup xml_feDiffuseLighting "<feDiffuseLighting>"
 *
 * Lights an image using the alpha channel as a bump map, computing the diffuse component of
 * the Phong lighting model.
 *
 * - DOM object: SVGFEDiffuseLightingElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feDiffuseLightingElement
 *
 * This element is defined within a \ref xml_filter element, and must contain exactly one
 * light source child element (\ref xml_feDistantLight, \ref xml_fePointLight, or \ref
 * xml_feSpotLight).
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feDiffuseLighting surfaceScale="5" diffuseConstant="1" lighting-color="white">
 *     <fePointLight x="50" y="50" z="100" />
 *   </feDiffuseLighting>
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feDiffuseLighting element.
 *
 * Computes diffuse lighting using the input's alpha channel as a height map to generate
 * surface normals, then applies the Phong diffuse lighting equation with the specified
 * light source.
 */
class SVGFEDiffuseLightingElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEDiffuseLightingElement wrapper from an entity.
  explicit SVGFEDiffuseLightingElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEDiffuseLightingElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeDiffuseLighting;
  /// XML tag name, \ref xml_feDiffuseLighting.
  static constexpr std::string_view Tag{"feDiffuseLighting"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
