#pragma once
/// @file

#include "donner/svg/SVGGeometryElement.h"

namespace donner::svg {

/**
 * @page xml_path "<path>"
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
  friend class parser::SVGParserImpl;

private:
  /// Create an SVGPathElement wrapper from an entity.
  explicit SVGPathElement(EntityHandle handle) : SVGGeometryElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGPathElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Path;
  /// XML tag name, \ref xml_path.
  static constexpr std::string_view Tag{"path"};

  static_assert(SVGGeometryElement::IsBaseOf(Type));
  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_path element.
   *
   * @param document Containing document.
   */
  static SVGPathElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

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
   * Set the path spline manually. Overrides the value of the `d` attribute.
   *
   * @param spline Path spline.
   */
  void setSpline(const PathSpline& spline);
};

}  // namespace donner::svg
