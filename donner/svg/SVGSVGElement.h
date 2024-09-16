#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Length.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg {

/**
 * @page xml_svg "<svg>"
 * @ingroup elements_structural
 *
 * The root element of an SVG document.
 *
 * - DOM object: SVGSVGElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/struct.html#SVGElement
 *
 * The `<svg>` element is the root element of an SVG document. It can contain any number of child
 * elements, such as \ref elements_basic_shapes, \ref elements_paint_servers, and \ref
 * elements_structural.
 *
 * ```xml
 * <svg width="300" height="300" style="background-color: white">
 *   <!-- ... -->
 * </svg>
 * ```
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `x`       | `0`     | Top-left X coordinate of the SVG viewport. |
 * | `y`       | `0`     | Top-left Y coordinate of the SVG viewport. |
 * | `width`   | `0`     | Width of the SVG viewport. |
 * | `height`  | `0`     | Height of the SVG viewport. |
 * | `viewBox` | (none)  | Rectangle in userspace that the SVG viewport is mapped to. |
 * | `preserveAspectRatio` | `xMidYMid meet` | How to scale the SVG viewport to fit the SVG content. |
 * | `transform` | (none) | Transformation matrix to apply to SVG content. |
 */

/**
 * DOM object for a \ref xml_svg element.
 *
 * ```xml
 * <svg width="300" height="300" style="background-color: white">
 *   <!-- ... -->
 * </svg>
 * ```
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `x`       | `0`     | Top-left X coordinate of the SVG viewport. |
 * | `y`       | `0`     | Top-left Y coordinate of the SVG viewport. |
 * | `width`   | `0`     | Width of the SVG viewport. |
 * | `height`  | `0`     | Height of the SVG viewport. |
 * | `viewBox` | (none)  | Rectangle in userspace that the SVG viewport is mapped to. |
 * | `preserveAspectRatio` | `xMidYMid meet` | How to scale the SVG viewport to fit the SVG content. |
 * | `transform` | (none) | Transformation matrix to apply to SVG content. |
 */
class SVGSVGElement : public SVGGraphicsElement {
  friend class SVGDocument;

protected:
  /// Create an SVGSVGElement wrapper from an entity.
  explicit SVGSVGElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::SVG;
  /// XML tag name, \ref xml_svg.
  static constexpr std::string_view Tag{"svg"};

  /**
   * Create a new \ref xml_svg element.
   *
   * @param document Containing document.
   */
  static SVGSVGElement Create(SVGDocument& document);

  /**
   * Get the top-left X coordinate of the SVG viewport.
   */
  Lengthd x() const;

  /**
   * Get the top-left Y coordinate of the SVG viewport.
   */
  Lengthd y() const;

  /**
   * Get the width of the SVG viewport, if specified.
   */
  std::optional<Lengthd> width() const;

  /**
   * Get the height of the SVG viewport, if specified.
   */
  std::optional<Lengthd> height() const;

  /**
   * Get the rectangle in userspace that the SVG viewport is mapped to.
   */
  std::optional<Boxd> viewbox() const;

  /**
   * Get how to scale the SVG viewport to fit the SVG content.
   */
  PreserveAspectRatio preserveAspectRatio() const;

  /**
   * Set the top-left X coordinate of the SVG viewport.
   *
   * @param value Coordinate value.
   */
  void setX(Lengthd value);

  /**
   * Set the top-left Y coordinate of the SVG viewport.
   *
   * @param value Coordinate value.
   */
  void setY(Lengthd value);

  /**
   * Set the width of the SVG viewport.
   *
   * @param value Dimension value.
   */
  void setWidth(std::optional<Lengthd> value);

  /**
   * Set the height of the SVG viewport.
   *
   * @param value Dimension value.
   */
  void setHeight(std::optional<Lengthd> value);

  /**
   * Set the rectangle in userspace that the SVG viewport is mapped to.
   *
   * @param viewbox Rectangle in userspace.
   */
  void setViewbox(std::optional<Boxd> viewbox);

  /**
   * Set how to scale the SVG viewport to fit the SVG content.
   *
   * @param preserveAspectRatio Preserve aspect ratio.
   */
  void setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio);
};

}  // namespace donner::svg
