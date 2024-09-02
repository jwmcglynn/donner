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

  // TODO: x/y/width/height
};

}  // namespace donner::svg
