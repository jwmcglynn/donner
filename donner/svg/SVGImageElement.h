#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/SVGGraphicsElement.h"

namespace donner::svg {

/**
 * @page xml_image "<image>"
 * @ingroup elements_graphics
 *
 * Embeds an image into the SVG document.
 *
 * - DOM object: SVGImageElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/embedded.html#ImageElement
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `href`    | `""`    | URL or base64 data string of the image. |
 * | `x`       | `0`     | X coordinate of the image. |
 * | `y`       | `0`     | Y coordinate of the image. |
 * | `width`   | `0`     | Width of the image. |
 * | `height`  | `0`     | Height of the image. |
 */

/**
 * DOM object for a \ref xml_image element.
 *
 * Use the `href`, `x`, `y`, `width`, and `height` attributes to define the image.
 */
class SVGImageElement : public SVGGraphicsElement {
private:
  /// Create an SVGImageElement wrapper from an entity.
  explicit SVGImageElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Image;
  /// XML tag name, \ref xml_image.
  static constexpr std::string_view Tag{"image"};

  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_image element.
   *
   * @param document Containing document.
   */
  static SVGImageElement Create(SVGDocument& document);

  /**
   * Set the href attribute.
   *
   * @param value URL or base64 data string of the image.
   */
  void setHref(std::string_view value);

  /**
   * Get the href attribute.
   */
  std::string_view href() const;

  /**
   * Set the X coordinate.
   *
   * @param value Coordinate value.
   */
  void setX(Lengthd value);

  /**
   * Get the X coordinate.
   */
  Lengthd x() const;

  /**
   * Set the Y coordinate.
   *
   * @param value Coordinate value.
   */
  void setY(Lengthd value);

  /**
   * Get the Y coordinate.
   */
  Lengthd y() const;

  /**
   * Set the width.
   *
   * @param value Width value.
   */
  void setWidth(Lengthd value);

  /**
   * Get the width.
   */
  Lengthd width() const;

  /**
   * Set the height.
   *
   * @param value Height value.
   */
  void setHeight(Lengthd value);

  /**
   * Get the height.
   */
  Lengthd height() const;

  /**
   * Applies stylesheet rules to the element, and returns the computed value of the `x` property.
   */
  Lengthd computedX() const;

  /**
   * Applies stylesheet rules to the element, and returns the computed value of the `y` property.
   */
  Lengthd computedY() const;

  /**
   * Applies stylesheet rules to the element, and returns the computed value of the `width` property.
   */
  Lengthd computedWidth() const;

  /**
   * Applies stylesheet rules to the element, and returns the computed value of the `height` property.
   */
  Lengthd computedHeight() const;

private:
  /// Invalidates cached data from the render tree.
  void invalidate() const;
  /// Create the computed data for this image, to be used for rendering.
  void compute() const;
};

}  // namespace donner::svg
