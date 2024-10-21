#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @defgroup xml_feGaussianBlur "<feGaussianBlur>"
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
    return CreateOn(CreateEntity(document));
  }

  // TODO: Add attributes
  // - in
  // - edgeMode

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
