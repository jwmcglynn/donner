#pragma once
/// @file

#include "src/svg/svg_filter_primitive_standard_attributes.h"

namespace donner::svg {

/**
 * @defgroup xml_feGaussianBlur '<feGaussianBlur>'
 *
 * Defines a filter primitive that performs a gaussian blur on the input image.
 *
 * - DOM object: SVGFEGaussianBlurElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feGaussianBlurElement
 *
 * This element is defined within a \ref xml_filter element, and is combined with other filter
 * primitives to define a filter applied on the input image.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feGaussianBlur in="SourceGraphic" stdDeviation="5" />
 * </filter>
 * ```
 *
 * To reference it with a filter:
 * ```xml
 * <rect filter="url(#MyFilter)" width="300" height="300" />
 * ```
 */

/**
 * DOM object for a \ref xml_feGaussianBlur element.
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feGaussianBlur in="SourceGraphic" stdDeviation="5" />
 * </filter>
 * ```
 *
 * To reference it with a filter:
 * ```xml
 * <rect filter="url(#MyFilter)" width="300" height="300" />
 * ```
 */
class SVGFEGaussianBlurElement : public SVGFilterPrimitiveStandardAttributes {
protected:
  /// Create an SVGFEGaussianBlurElement wrapper from an entity.
  explicit SVGFEGaussianBlurElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeGaussianBlur;
  /// XML tag name, \ref xml_feGaussianBlur.
  static constexpr std::string_view Tag{"feGaussianBlur"};

  /**
   * Create a new \ref xml_filter element.
   *
   * @param document Containing document.
   */
  static SVGFEGaussianBlurElement Create(SVGDocument& document);

  // TODO: Add attributes
  // - in
  // - stdDeviation
  // - edgeMode
};

}  // namespace donner::svg
