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
 * If `width` or `height` are omitted, the sizes will be inferred using the image's intrinsic size,
 * using the CSS default sizing algorithm, https://www.w3.org/TR/css-images-3/#default-sizing.
 *
 * To reference an external image, provide its name or URL. Note that Donner must have a valid
 * ResourceLoader provided to \ref parser::SVGParser::ParseSVG, such as \ref
 * SandboxedFileResourceLoader.
 * ```xml
 * <image href="image.png" x="10" y="10" width="100" height="100" />
 * ```
 *
 * To reference an embedded image using a data URL:
 * ```xml
 * <image href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQBAMAAADt3eJSAAAAFVBMVEUAAAAisUztHCSo5h3/8gD/fgBNbfOxiPXkAAAAAXRSTlMAQObYZgAAAD9JREFUCNdjwASMDIxABGIICEIZQAFBARADDOEMZMCkxMrAwmAMZCmwBrgwM4AZLCzMbAlABlCKmSEBrgYPAACkeQLx8K5PDQAAAABJRU5ErkJggg==" />
 * ```
 *
 * \htmlonly
 * <svg viewbox="-2 -2 18 18" width="300" height="300" xmlns="http://www.w3.org/2000/svg">
 *   <image style="image-rendering: pixelated" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQBAMAAADt3eJSAAAAFVBMVEUAAAAisUztHCSo5h3/8gD/fgBNbfOxiPXkAAAAAXRSTlMAQObYZgAAAD9JREFUCNdjwASMDIxABGIICEIZQAFBARADDOEMZMCkxMrAwmAMZCmwBrgwM4AZLCzMbAlABlCKmSEBrgYPAACkeQLx8K5PDQAAAABJRU5ErkJggg==" />
 * </svg>
 * \endhtmlonly
 *
 * @note The `image-rendering: pixelated` style is used to render the image in a pixelated style in
 * this example. This is not yet supported by Donner.
 *
 * @todo Add support for `image-rendering` property,
 * https://drafts.csswg.org/css-images/#the-image-rendering
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `href`    | (none) | URL or base64 data URL of the image. |
 * | `preserveAspectRatio` | `xMidYMid meet` | How to scale the image to fit the rectangle defined by `width` and `height` if the image's intrinsic size is different. |
 * | `x`       | `0` | X coordinate of the image. |
 * | `y`       | `0` | Y coordinate of the image. |
 * | `width`   | `auto` | Width of the image. If omitted, this value will be inferred from the `height` attribute (if provided), or it will fall back to the image's intrinsic size. |
 * | `height`  | `auto` | Height of the image. If omitted, this value will be inferred from the `width` attribute (if provided), or it will fall back to the image's intrinsic size. |
 */

/**
 * DOM object for a \ref xml_image element.
 *
 * Use the `href`, `x`, `y`, `width`, and `height` attributes to define the image.
 */
class SVGImageElement : public SVGGraphicsElement {
  friend class parser::SVGParserImpl;

private:
  /// Create an SVGImageElement wrapper from an entity.
  explicit SVGImageElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGImageElement CreateOn(EntityHandle handle);

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
  static SVGImageElement Create(SVGDocument& document) { return CreateOn(CreateEntity(document)); }

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
   * Set the width, or \c std::nullopt to use the image's intrinsic width.
   *
   * @param value Width value.
   */
  void setWidth(std::optional<Lengthd> value);

  /**
   * Get the width.
   */
  std::optional<Lengthd> width() const;

  /**
   * Set the height, or \c std::nullopt to use the image's intrinsic height.
   *
   * @param value Height value.
   */
  void setHeight(std::optional<Lengthd> value);

  /**
   * Get the height.
   */
  std::optional<Lengthd> height() const;
};

}  // namespace donner::svg
