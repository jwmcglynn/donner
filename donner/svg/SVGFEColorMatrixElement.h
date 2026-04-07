#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feColorMatrix "<feColorMatrix>"
 *
 * Defines a filter primitive that applies a matrix transformation on color values.
 *
 * - DOM object: SVGFEColorMatrixElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feColorMatrixElement
 *
 * This element supports four modes: a direct 5x4 matrix, a saturation adjustment,
 * a hue rotation, and a luminance-to-alpha conversion.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="Grayscale">
 *   <feColorMatrix type="saturate" values="0" />
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feColorMatrix element.
 *
 * Applies a matrix transformation to the RGBA color and alpha values of every pixel of the
 * input graphics to produce an output with a new set of color values.
 */
class SVGFEColorMatrixElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEColorMatrixElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  static SVGFEColorMatrixElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeColorMatrix;
  /// XML tag name, \ref xml_feColorMatrix.
  static constexpr std::string_view Tag{"feColorMatrix"};
};

}  // namespace donner::svg
