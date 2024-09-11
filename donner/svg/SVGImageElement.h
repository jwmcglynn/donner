#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg {

/**
 * @defgroup xml_image "<image>"
 *
 * Embeds an image into the SVG document.
 *
 * - DOM object: SVGImageElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/embedded.html#ImageElement
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `href`    | (none)  | URL or base64 data string of the image. |
 * | `preserveAspectRatio` | `xMidYMid meet` | How to scale the image to fit the rectangle defined by `width` and `height` if the image's intrinsic size is different. |
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
  void setHref(RcStringOrRef value);

  /**
   * Get the href attribute.
   */
  RcString href() const;

  /**
   * Set the `preserveAspectRatio` attribute, which defines how to scale the image to fit the
   * rectangle defined by `width` and `height` if the image's intrinsic size doesn't match.
   *
   * @param preserveAspectRatio The preserveAspectRatio value to set.
   */
  void setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio);

  /**
   * The value of the `preserveAspectRatio` attribute, which defines how to scale the image to fit
   * the rectangle defined by `width` and `height` if the image's intrinsic size doesn't match.
   */
  PreserveAspectRatio preserveAspectRatio() const;

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
  std::optional<Lengthd> width() const;

  /**
   * Set the height.
   *
   * @param value Height value.
   */
  void setHeight(Lengthd value);

  /**
   * Get the height.
   */
  std::optional<Lengthd> height() const;
};

}  // namespace donner::svg
