#pragma once
/// @file

#include "donner/svg/SVGElement.h"
#include "donner/svg/core/MaskUnits.h"

namespace donner::svg {

/**
 * @defgroup xml_mask "<mask>"
 *
 * Defines a mask, which is used to apply image-based visibility to graphical elements. Compared to
 * \ref xml_clipPath, which requires the contents to be paths, "<mask>" masking is performed based
 * on the white and black values of the mask contents.
 *
 * - DOM object: SVGMaskElement
 * - SVG2 spec: https://drafts.fxtf.org/css-masking-1/#MaskElement
 *
 * These elements are typically placed within a `<defs>` element, and then referenced by id from a
 * `mask` attribute.
 *
 * Example usage:
 * ```xml
 * <mask id="MyMask">
 *  <!-- Things under a white pixel will be drawn -->
 *  <rect x="0" y="0" width="100" height="100" fill="white" />
 *
 *  <!-- Things under a black pixel will be invisible -->
 *  <circle cx="50" cy="50" r="40" fill="black" />
 * </mask>
 * ```
 *
 * To reference it with the mask attribute:
 * ```xml
 * <rect mask="url(#MyMask)" width="100" height="100" fill="green" />
 * ```
 *
 * This draws a green rectangle with a circle cut out of the middle of it.
 *
 * \htmlonly
 * <svg viewbox="-10 -10 120 120" width="300" height="300" style="background-color: white">
 *   <mask id="MyMask">
 *    <!-- Things under a white pixel will be drawn -->
 *    <rect x="0" y="0" width="100" height="100" fill="white" />
 *
 *    <!-- Things under a black pixel will be invisible -->
 *    <circle cx="50" cy="50" r="40" fill="black" />
 *   </mask>
 *
 *   <rect mask="url(#MyMask)" width="100" height="100" fill="green" />
 * </svg>
 * \endhtmlonly
 */

/**
 * Represents the \ref xml_mask element in SVG, which is used to define a mask for graphical
 * elements.
 */
class SVGMaskElement : public SVGElement {
private:
private:
  explicit SVGMaskElement(EntityHandle handle) : SVGElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Mask;
  /// XML tag name, \ref xml_mask.
  static constexpr std::string_view Tag{"mask"};

  /**
   * Create a new \ref xml_mask element.
   *
   * @param document Containing document.
   */
  static SVGMaskElement Create(SVGDocument& document);

  /**
   * Get the value of the `maskUnits` attribute, which defines the coordinate system for the `x`,
   * `y`, `width`, and `height` attributes of the mask.
   *
   * @return The attribute value.
   */
  MaskUnits maskUnits() const;

  /**
   * Set the value of the `maskUnits` attribute, which defines the coordinate system for the `x`,
   * `y`, `width`, and `height` attributes of the mask.
   *
   * @param value The new attribute value.
   */
  void setMaskUnits(MaskUnits value);

  /**
   * Get the value of the `maskContentUnits` attribute, which defines the coordinate system for the
   * contents of the mask.
   *
   * @return The attribute value.
   */
  MaskContentUnits maskContentUnits() const;

  /**
   * Set the value of the `maskContentUnits` attribute, which defines the coordinate system for the
   * contents of the mask.
   *
   * @param value The attribute value.
   */
  void setMaskContentUnits(MaskContentUnits value);

  /**
   * Get the top-left X coordinate of the mask region. If this is not specified and at least one of
   * the attributes `y`, `width`, or `height` is specified the effect is as if the initial value is
   * '-10%'. If no attributes are specified, the effect will fill the canvas.
   */
  std::optional<Lengthd> x() const;

  /**
   * Get the top-left Y coordinate of the mask region. If this is not specified and at least one of
   * the attributes `x`, `width`, or `height` is specified the effect is as if the initial value is
   * '-10%'. If no attributes are specified, the effect will fill the canvas.
   */
  std::optional<Lengthd> y() const;

  /**
   * Get the width of the mask region. If this is not specified and at least one of the attributes
   * `x`, `y`, or `height` is specified the effect is as if the initial value is '120%'. If no
   * attributes are specified, the effect will fill the canvas.
   */
  std::optional<Lengthd> width() const;

  /**
   * Get the height of the mask region. If this is not specified and at least one of the attributes
   * `x`, `y`, or `width` is specified the effect is as if the initial value is '120%'. If no
   * attributes are specified, the effect will fill the canvas.
   */
  std::optional<Lengthd> height() const;

  /**
   * Set the top-left X coordinate of the mask region. If this is not specified and at least one of
   * the attributes `y`, `width`, or `height` is specified the effect is as if the initial value is
   * '-10%'. If no attributes are specified, the effect will fill the canvas.
   *
   * @param value Coordinate value.
   */
  void setX(std::optional<Lengthd> value);

  /**
   * Set the top-left Y coordinate of the mask region. If this is not specified and at least one of
   * the attributes `x`, `width`, or `height` is specified the effect is as if the initial value is
   * '-10%'. If no attributes are specified, the effect will fill the canvas.
   *
   * @param value Coordinate value.
   */
  void setY(std::optional<Lengthd> value);

  /**
   * Set the width of the mask region. If this is not specified and at least one of the attributes
   * `x`, `y`, or `height` is specified the effect is as if the initial value is '120%'. If no
   * attributes are specified, the effect will fill the canvas.
   *
   * @param value Dimension value.
   */
  void setWidth(std::optional<Lengthd> value);

  /**
   * Set the height of the mask region. If this is not specified and at least one of the attributes
   * `x`, `y`, or `width` is specified the effect is as if the initial value is '120%'. If no
   * attributes are specified, the effect will fill the canvas.
   *
   * @param value Dimension value.
   */
  void setHeight(std::optional<Lengthd> value);
};

}  // namespace donner::svg
