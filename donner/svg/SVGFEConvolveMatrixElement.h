#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @defgroup xml_feConvolveMatrix "<feConvolveMatrix>"
 *
 * Applies a matrix convolution filter effect to the input image.
 *
 * - DOM object: SVGFEConvolveMatrixElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feConvolveMatrixElement
 *
 * This element is defined within a \ref xml_filter element, and is combined with other filter
 * primitives to define a filter applied on the input image.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feConvolveMatrix kernelMatrix="0 -1 0 -1 5 -1 0 -1 0" />
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feConvolveMatrix element.
 *
 * Applies a matrix convolution filter to the input image, enabling effects such as blurring,
 * edge detection, sharpening, embossing, and beveling.
 */
class SVGFEConvolveMatrixElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEConvolveMatrixElement wrapper from an entity.
  explicit SVGFEConvolveMatrixElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEConvolveMatrixElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeConvolveMatrix;
  /// XML tag name, \ref xml_feConvolveMatrix.
  static constexpr std::string_view Tag{"feConvolveMatrix"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
