#pragma once
/// @file

#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"

namespace donner::svg {

/**
 * @page xml_marker "<marker>"
 * @ingroup elements_basic_shapes
 *
 * Creates a marker element that can be used to define graphical objects that can be used repeatedly
 * in a drawing, such as arrowheads or other markers on paths.
 *
 * - DOM object: SVGMarkerElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/painting.html#MarkerElement
 *
 * ```xml
 * <marker id="arrow" markerWidth="10" markerHeight="10" refX="5" refY="5" orient="auto">
 *   <path d="M0,0 L10,5 L0,10 Z" />
 * </marker>
 * ```
 *
 * \htmlonly
 * <svg id="xml_marker" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_marker path {
 *       fill: red;
 *     }
 *   </style>
 *   <defs>
 *     <marker id="arrow" markerWidth="10" markerHeight="10" refX="5" refY="5" orient="auto">
 *       <path d="M0,0 L10,5 L0,10 Z" />
 *     </marker>
 *   </defs>
 *   <path d="M50,50 L250,50 L150,150 L250,250 L50,250" marker-mid="url(#arrow)" />
 * </svg>
 * \endhtmlonly
 *
 * | Attribute      | Default | Description  |
 * | -------------: | :-----: | :----------- |
 * | `markerWidth`  | (none)  | Width of the marker viewport. |
 * | `markerHeight` | (none)  | Height of the marker viewport. |
 * | `refX`         | (none)  | X coordinate for the reference point of the marker. |
 * | `refY`         | (none)  | Y coordinate for the reference point of the marker. |
 * | `orient`       | (none)  | Orientation of the marker relative to the path. |
 */

/**
 * DOM object for a \ref xml_marker element.
 *
 * The `<marker>` element is used to define graphical objects that can be used repeatedly in a
 * drawing, such as arrowheads or other markers on paths.
 *
 * \htmlonly
 * <svg id="xml_marker" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_marker path {
 *       fill: red;
 *     }
 *   </style>
 *   <defs>
 *     <marker id="arrow" markerWidth="10" markerHeight="10" refX="5" refY="5" orient="auto">
 *       <path d="M0,0 L10,5 L0,10 Z" />
 *     </marker>
 *   </defs>
 *   <path d="M50,50 L250,50 L150,150 L250,250 L50,250" marker-mid="url(#arrow)" />
 * </svg>
 * \endhtmlonly
 */
class SVGMarkerElement : public SVGElement {
private:
  /// Create an SVGMarkerElement wrapper from an entity.
  explicit SVGMarkerElement(EntityHandle handle) : SVGElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Marker;
  /// XML tag name, \ref xml_marker.
  static constexpr std::string_view Tag{"marker"};

  /**
   * Create a new \ref xml_marker element.
   *
   * @param document Containing document.
   */
  static SVGMarkerElement Create(SVGDocument& document);

  /**
   * Set the marker width.
   *
   * @param value Width of the marker viewport.
   */
  void setMarkerWidth(double value);

  /**
   * Get the marker width.
   *
   * @return Width of the marker viewport.
   */
  double markerWidth() const;

  /**
   * Set the marker height.
   *
   * @param value Height of the marker viewport.
   */
  void setMarkerHeight(double value);

  /**
   * Get the marker height.
   *
   * @return Height of the marker viewport.
   */
  double markerHeight() const;

  /**
   * Set the reference point X coordinate.
   *
   * @param value X coordinate for the reference point of the marker.
   */
  void setRefX(double value);

  /**
   * Get the reference point X coordinate.
   *
   * @return X coordinate for the reference point of the marker.
   */
  double refX() const;

  /**
   * Set the reference point Y coordinate.
   *
   * @param value Y coordinate for the reference point of the marker.
   */
  void setRefY(double value);

  /**
   * Get the reference point Y coordinate.
   *
   * @return Y coordinate for the reference point of the marker.
   */
  double refY() const;

  /**
   * Set the orientation of the marker.
   *
   * @param value Orientation of the marker relative to the path.
   */
  void setOrient(std::string_view value);

  /**
   * Get the orientation of the marker.
   *
   * @return Orientation of the marker relative to the path.
   */
  std::string_view orient() const;
};

}  // namespace donner::svg
