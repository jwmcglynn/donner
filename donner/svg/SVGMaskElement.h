#pragma once
/// @file

#include "donner/svg/SVGElement.h"
#include "donner/svg/core/MaskUnits.h"

namespace donner::svg {

/**
 * @page xml_mask "<mask>"
 *
 * Defines a mask, which is used to apply image-based visibility to graphical elements. Compared to
 * \ref xml_clipPath, which requires the contents to be paths, "<mask>" masking is performed based
 * on the white and black values of the mask contents.
 *
 * - DOM object: SVGMaskElement
 * - SVG2 spec: https://drafts.fxtf.org/css-masking-1/#MaskElement
 *
 * A `<mask>` defines a **luminance-based soft mask**. You fill the `<mask>` with arbitrary SVG
 * graphics — shapes, gradients, even images — and at render time SVG uses the brightness
 * (luminance) of each pixel of the mask as the alpha value for the corresponding pixel of the
 * masked element. White pixels in the mask leave the target fully visible, black pixels hide
 * it completely, and gray pixels produce partial transparency, so you can author smooth fades
 * and soft edges that \ref xml_clipPath cannot express.
 *
 * Declare a `<mask>` inside \ref xml_defs with an `id`, then apply it to any shape via the
 * `mask="url(#id)"` attribute or CSS property. Reach for `<mask>` when you need soft edges,
 * gradients of visibility, or image-based cutouts; use \ref xml_clipPath when a crisp,
 * geometry-only "in or out" clip is sufficient (and faster).
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
 *
 * | Attribute          | Default             | Description  |
 * | -----------------: | :-----------------: | :----------- |
 * | `x`                | `-10%`              | Top-left X coordinate of the mask region. |
 * | `y`                | `-10%`              | Top-left Y coordinate of the mask region. |
 * | `width`            | `120%`              | Width of the mask region. |
 * | `height`           | `120%`              | Height of the mask region. |
 * | `maskUnits`        | `objectBoundingBox` | Coordinate system for `x`, `y`, `width`, and `height`. Either `userSpaceOnUse` or `objectBoundingBox`. |
 * | `maskContentUnits` | `userSpaceOnUse`    | Coordinate system for the contents of the mask. Either `userSpaceOnUse` or `objectBoundingBox`. |
 */

/**
 * Represents the \ref xml_mask element in SVG, which is used to define a mask for graphical
 * elements.
 */
class SVGMaskElement : public SVGElement {
  friend class parser::SVGParserImpl;

private:
  explicit SVGMaskElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGMaskElement CreateOn(EntityHandle handle);

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
  static SVGMaskElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

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
