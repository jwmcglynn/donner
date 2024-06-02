#pragma once
/// @file

#include "src/svg/core/path_spline.h"
#include "src/svg/svg_geometry_element.h"

namespace donner::svg {

/**
 * @page xml_path '<path>'
 * @ingroup elements_basic_shapes
 *
 * Defines a shape using a path, which can include straight lines, curves, and sub-paths.
 *
 * - DOM object: SVGPathElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/paths.html#PathElement
 *
 * ```xml
 * <path d="M 40 50 V 250 C 100 100 115 75 190 125" />
 * ```
 *
 * \htmlonly
 * <svg id="xml_path" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_path text { font-size: 16px; font-weight: bold; color: black }
 *     #xml_path path { stroke-width: 2px; stroke: black; fill: none }
 *     #xml_path circle { r: 3px; fill: black }
 *     #xml_path line { stroke-width: 2px; stroke: red; stroke-dasharray: 6,4 }
 *   </style>
 *   <path d="M 40 50 V 250 C 100 100 115 75 190 125" />
 *   <circle cx="40" cy="50" style="fill: red" />
 *   <text x="50" y="53">M 40 50</text>
 *   <polygon points="0,0 5,10 10,0" transform="translate(35,150)" fill="red" />
 *   <circle cx="40" cy="250" />
 *   <text x="50" y="253">V 250</text>
 *   <circle cx="190" cy="125" />
 *   <line x1="40" y1="250" x2="100" y2="100" />
 *   <line x1="115" y1="75" x2="190" y2="125" />
 *   <circle cx="100" cy="100" />
 *   <circle cx="115" cy="75" />
 *   <text x="200" y="128">C 100 100</text>
 *   <text x="200" y="148">115 75</text>
 *   <text x="200" y="168">190 125</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `d`       | (none)  | Path data, see \ref path_data. |
 */

/**
 * DOM object for a \ref xml_path element.
 *
 * Use the `d` attribute to define the path, see \ref path_data for the syntax.
 *
 * Example path:
 * ```
 * M 40 50 V 250 C 100 100 115 75 190 125
 * ```
 *
 * \htmlonly
 * <svg id="xml_path" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_path text { font-size: 16px; font-weight: bold; color: black }
 *     #xml_path path { stroke-width: 2px; stroke: black; fill: none }
 *     #xml_path circle { r: 3px; fill: black }
 *     #xml_path line { stroke-width: 2px; stroke: red; stroke-dasharray: 6,4 }
 *   </style>
 *   <path d="M 40 50 V 250 C 100 100 115 75 190 125" />
 *   <circle cx="40" cy="50" style="fill: red" />
 *   <text x="50" y="53">M 40 50</text>
 *   <polygon points="0,0 5,10 10,0" transform="translate(35,150)" fill="red" />
 *   <circle cx="40" cy="250" />
 *   <text x="50" y="253">V 250</text>
 *   <circle cx="190" cy="125" />
 *   <line x1="40" y1="250" x2="100" y2="100" />
 *   <line x1="115" y1="75" x2="190" y2="125" />
 *   <circle cx="100" cy="100" />
 *   <circle cx="115" cy="75" />
 *   <text x="200" y="128">C 100 100</text>
 *   <text x="200" y="148">115 75</text>
 *   <text x="200" y="168">190 125</text>
 * </svg>
 * \endhtmlonly
 */
class SVGPathElement : public SVGGeometryElement {
private:
  /// Create an SVGPathElement wrapper from an entity.
  explicit SVGPathElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Path;
  /// XML tag name, \ref xml_path.
  static constexpr std::string_view Tag{"path"};

  /**
   * Create a new \ref xml_path element.
   *
   * @param document Containing document.
   */
  static SVGPathElement Create(SVGDocument& document);

  /**
   * Get the path data string, see \ref path_data.
   */
  RcString d() const;

  /**
   * Set the path data string, see \ref path_data.
   *
   * @param d New path data string.
   */
  void setD(RcString d);

  /**
   * Get the path spline, computed from the path data string, \ref d(), which has been parsed with
   * \ref PathParser.
   *
   * @return Path spline, or `std::nullopt` if the path data string is invalid.
   */
  std::optional<PathSpline> computedSpline() const;

private:
  void invalidate() const;
  void compute() const;
};

}  // namespace donner::svg
