#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feGaussianBlur "<feGaussianBlur>"
 *
 * Defines a filter primitive that performs a gaussian blur on the input image.
 *
 * - DOM object: SVGFEGaussianBlurElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feGaussianBlurElement
 *
 * This element is defined within a \ref xml_filter element, and is combined with other filter
 * primitives to define a filter applied on the input image.
 *
 * \htmlonly
 * <svg id="xml_feGaussianBlur" width="320" height="160" viewBox="0 0 320 160" style="background-color: white">
 *   <defs>
 *     <filter id="xml_feGaussianBlur_filter" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceGraphic" stdDeviation="5" />
 *     </filter>
 *   </defs>
 *   <g font-family="sans-serif" font-size="12" fill="#333" text-anchor="middle">
 *     <text x="75" y="20">Source</text>
 *     <text x="240" y="20">stdDeviation="5"</text>
 *   </g>
 *   <rect x="25" y="35" width="100" height="100" rx="8" fill="#5aa9ff" stroke="#1f5a8a" stroke-width="3" />
 *   <rect x="190" y="35" width="100" height="100" rx="8" fill="#5aa9ff" stroke="#1f5a8a" stroke-width="3" filter="url(#xml_feGaussianBlur_filter)" />
 * </svg>
 * \endhtmlonly
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
 *
 * | Attribute      | Default | Description  |
 * | -------------: | :-----: | :----------- |
 * | `in`           | (previous result) | Input for the filter primitive. |
 * | `stdDeviation` | `0 0`   | Standard deviation of the blur, in user units. A single value applies to both axes; two values specify X and Y independently. |
 * | `edgeMode`     | `none`  | How to extend the input image at edges: `duplicate`, `wrap`, or `none`. |
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
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEGaussianBlurElement wrapper from an entity.
  explicit SVGFEGaussianBlurElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEGaussianBlurElement CreateOn(EntityHandle handle);

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
  static SVGFEGaussianBlurElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

  // TODO: Add attribute accessor
  // - in

  /**
   * Get the X-axis of the standard deviation, which is used to control the blur level.
   *
   * @return X-axis value of stdDeviation.
   */
  double stdDeviationX() const;

  /**
   * Get the Y-axis of the standard deviation, which is used to control the blur level.
   *
   * @return Y-axis value of stdDeviation.
   */
  double stdDeviationY() const;

  /**
   * Set the standard deviation, which is used to control the blur level.
   *
   * Negative values or a value of zero disables the effect of the given filter primitive (i.e., the
   * result is the filter input image).
   *
   * If the value is 0 in only one of X or Y, then the effect is that the blur is only applied in
   * the direction that has a non-zero value.
   *
   * The initial value is (0, 0).
   *
   * @param valueX X-axis value of stdDeviation.
   * @param valueY Y-axis value of stdDeviation.
   */
  void setStdDeviation(double valueX, double valueY);
};

}  // namespace donner::svg
