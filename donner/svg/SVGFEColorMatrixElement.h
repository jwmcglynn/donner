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
 *
 * \htmlonly
 * <svg id="xml_feColorMatrix_diagram" width="320" height="200" viewBox="0 0 320 200" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <linearGradient id="xml_feColorMatrix_grad" x1="0" y1="0" x2="1" y2="0">
 *       <stop offset="0%" stop-color="#e74c3c" />
 *       <stop offset="50%" stop-color="#f1c40f" />
 *       <stop offset="100%" stop-color="#2ecc71" />
 *     </linearGradient>
 *     <filter id="xml_feColorMatrix_gray" x="-5%" y="-5%" width="110%" height="110%">
 *       <feColorMatrix in="SourceGraphic" type="saturate" values="0" />
 *     </filter>
 *     <filter id="xml_feColorMatrix_hue" x="-5%" y="-5%" width="110%" height="110%">
 *       <feColorMatrix in="SourceGraphic" type="hueRotate" values="90" />
 *     </filter>
 *   </defs>
 *   <rect x="20" y="20" width="80" height="50" fill="url(#xml_feColorMatrix_grad)" />
 *   <text x="20" y="85" fill="black">Source</text>
 *   <rect x="120" y="20" width="80" height="50" fill="url(#xml_feColorMatrix_grad)" filter="url(#xml_feColorMatrix_gray)" />
 *   <text x="115" y="85" fill="black">saturate="0"</text>
 *   <rect x="220" y="20" width="80" height="50" fill="url(#xml_feColorMatrix_grad)" filter="url(#xml_feColorMatrix_hue)" />
 *   <text x="215" y="85" fill="black">hueRotate="90"</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default  | Description  |
 * | --------: | :------: | :----------- |
 * | `type`    | `matrix` | One of `matrix`, `saturate`, `hueRotate`, `luminanceToAlpha`. |
 * | `values`  | (none)   | Matrix values or scalar parameter depending on `type`. |
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
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
  /// Create an SVGFEColorMatrixElement wrapper from an entity.
  explicit SVGFEColorMatrixElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /// Internal constructor to create the element on an existing \ref donner::Entity.
  static SVGFEColorMatrixElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeColorMatrix;
  /// XML tag name, \ref xml_feColorMatrix.
  static constexpr std::string_view Tag{"feColorMatrix"};
};

}  // namespace donner::svg
