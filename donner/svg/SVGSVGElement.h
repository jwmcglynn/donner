#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Length.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg {

namespace parser {

// Forward declaration
class SVGParser;

}  // namespace parser

/**
 * @page xml_svg "<svg>"
 *
 * The root element of an SVG document.
 *
 * - DOM object: SVGSVGElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/struct.html#SVGElement
 *
 * The `<svg>` element is the container for everything drawn in SVG. It is usually the root of
 * a standalone `.svg` file or the outermost SVG tag inside an HTML document, but it can also be
 * nested inside another `<svg>` to create an embedded sub-viewport with its own coordinate
 * system. Every drawable element — shapes, groups, text, images — must live inside an `<svg>`.
 *
 * A `<svg>` has two related but distinct sizes. The `width` and `height` attributes set its
 * **size on the page** (the rectangle it occupies in the parent layout, measured in CSS
 * pixels). The optional `viewBox` attribute sets its **internal coordinate system**: a
 * rectangle in "user units" that gets mapped onto that on-page rectangle. By separating the
 * two, you can author graphics in a convenient coordinate system (say, `0 0 1000 1000`) and
 * have them scale cleanly to any display size, with `preserveAspectRatio` controlling how the
 * content is fit when the aspect ratios differ.
 *
 * It can contain any number of child elements, such as \ref elements_basic_shapes,
 * \ref elements_painting, \ref elements_text, \ref elements_filters, and
 * \ref elements_structural.
 *
 * ```xml
 * <svg width="300" height="200" viewBox="0 0 100 100">
 *   <circle cx="50" cy="50" r="40" fill="crimson" />
 * </svg>
 * ```
 *
 * The `width` and `height` attributes define the size of the SVG on the page, while `viewBox`
 * defines the user coordinate system that child elements are drawn in. The diagram below shows
 * a `<svg width="280" height="180" viewBox="0 0 100 100">` where the user-coordinate `viewBox`
 * (the `0,0` to `100,100` square) is stretched to fill the outer canvas.
 *
 * \htmlonly
 * <svg id="xml_svg" width="320" height="220" style="background-color: white">
 *   <style>
 *     #xml_svg text { font-size: 13px; font-weight: bold; fill: black }
 *     #xml_svg line { stroke: black; stroke-width: 1.5; stroke-dasharray: 5,3 }
 *     #xml_svg .axis { stroke: #c33; stroke-width: 1.5; stroke-dasharray: 0 }
 *     #xml_svg rect.canvas { fill: none; stroke: black; stroke-width: 2 }
 *   </style>
 *   <defs>
 *     <marker id="xml_svg_arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">
 *       <path d="M 0 0 L 10 5 L 0 10 z" fill="#c33" />
 *     </marker>
 *   </defs>
 *
 *   <rect class="canvas" x="20" y="20" width="280" height="180" />
 *   <circle cx="160" cy="110" r="72" fill="crimson" fill-opacity="0.8" />
 *
 *   <line x1="20" y1="10" x2="300" y2="10" />
 *   <text x="145" y="8">width</text>
 *   <line x1="10" y1="20" x2="10" y2="200" />
 *   <text x="14" y="115" transform="rotate(-90 14 115)">height</text>
 *
 *   <line class="axis" x1="20" y1="20" x2="60" y2="20" marker-end="url(#xml_svg_arrow)" />
 *   <line class="axis" x1="20" y1="20" x2="20" y2="60" marker-end="url(#xml_svg_arrow)" />
 *   <text x="28" y="35" fill="#c33">x</text>
 *   <text x="28" y="60" fill="#c33">y</text>
 *   <text x="24" y="15" fill="#c33">viewBox (0,0)</text>
 *   <text x="240" y="195" fill="#c33">(100,100)</text>
 * </svg>
 * \endhtmlonly
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
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGSVGElement wrapper from an entity.
  explicit SVGSVGElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

  /**
   * Create a new \ref xml_svg element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGSVGElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::SVG;
  /// XML tag name, \ref xml_svg.
  static constexpr std::string_view Tag{"svg"};

  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_svg element.
   *
   * @param document Containing document.
   */
  static SVGSVGElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

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
  std::optional<Box2d> viewBox() const;

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
   * Set the viewBox rectangle in userspace that the SVG viewport is mapped to.
   *
   * @param viewBox Rectangle in userspace.
   */
  void setViewBox(std::optional<Box2d> viewBox);

  /**
   * Set the viewBox rectangle in userspace that the SVG viewport is mapped to.
   *
   * @param x Top-left X coordinate.
   * @param y Top-left Y coordinate.
   * @param width Width.
   * @param height Height.
   */
  void setViewBox(double x, double y, double width, double height) {
    setViewBox(Box2d::FromXYWH(x, y, width, height));
  }

  /**
   * Set how to scale the SVG viewport to fit the SVG content.
   *
   * @param preserveAspectRatio Preserve aspect ratio.
   */
  void setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio);
};

}  // namespace donner::svg
