#pragma once
/// @file

#include "donner/base/OptionalRef.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/core/MarkerOrient.h"
#include "donner/svg/core/MarkerUnits.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg {

/**
 * @defgroup xml_marker "<marker>"
 *
 * Creates a marker element that can be used to define graphical objects that can be used repeatedly
 * in a drawing, such as arrowheads or other markers on paths.
 *
 * - DOM object: SVGMarkerElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/painting.html#MarkerElement
 *
 * ```xml
 * <marker id="arrow" refX="3" refY="3" markerWidth="6" markerHeight="6" orient="auto">
 *   <path d="M 0 0 L 6 3 L 0 6 z" fill="red" />
 * </marker>
 * ```
 *
 * \htmlonly
 * <svg width="300" height="300" xmlns="http://www.w3.org/2000/svg" style="background-color: white">
 *   <defs>
 *     <marker id="arrow" refX="3" refY="3" markerWidth="6" markerHeight="6" orient="auto">
 *       <path d="M 0 0 L 6 3 L 0 6 z" fill="red" />
 *     </marker>
 *   </defs>
 *
 *   <path
 *     d="m17,151 c29,146 58,146 87,0 c29,-146 58,-146 87,0 c29,146 58,146 87,0"
 *     stroke="black"
 *     fill="none"
 *     marker-start="url(#arrow)"
 *     marker-mid="url(#arrow)"
 *     marker-end="url(#arrow)"
 *     stroke-width="3px"
 *      />
 * </svg>
 * \endhtmlonly
 *
 * | Attribute      | Default | Description  |
 * | -------------: | :-----: | :----------- |
 * | `viewBox` | (none)  | A list of four numbers (min-x, min-y, width, height) separated by whitespace and/or a comma, that specify a rectangle in userspace that should be mapped to the SVG viewport bounds established by the marker. |
 * | `preserveAspectRatio` | `xMidYMid meet` | How to scale the viewport to fit the content. Only applies is `viewBox` is specified. |
 * | `markerWidth`  | `3`     | Width of the marker viewport. |
 * | `markerHeight` | `3`     | Height of the marker viewport. |
 * | `refX`         | `0`     | X coordinate for the reference point of the marker, where the marker is centered. |
 * | `refY`         | `0`     | Y coordinate for the reference point of the marker, where the marker is centered. |
 * | `orient`       | `0`     | Orientation of the marker relative to the path. Supported values: `auto`, `auto-start-reverse`, or an angle for a fixed rotation such as `45deg` or `3.14rad`. |
 */

/**
 * DOM object for a \ref xml_marker element, which is used to define graphical objects that can be
 * used repeatedly along a path, such as arrowheads or other markers on paths.
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
   * Set the `viewBox` attribute, which defines a rectangle in userspace that should be mapped to
   * the SVG viewport bounds established by the pattern.
   *
   * @param viewbox The viewBox value to set.
   */
  void setViewbox(OptionalRef<Boxd> viewbox);

  /**
   * Get the parsed value of the `viewBox` attribute, if specified, which defines a rectangle in
   * userspace that should be mapped to the SVG viewport bounds established by the pattern.
   */
  std::optional<Boxd> viewbox() const;

  /**
   * Set the `preserveAspectRatio` attribute, which defines how to scale the viewport to fit the
   * content. Only applies if `viewBox` is specified.
   *
   * @param preserveAspectRatio The preserveAspectRatio value to set.
   */
  void setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio);

  /**
   * The value of the `preserveAspectRatio` attribute, which defines how to scale the viewport to
   * fit the content. Only applies is `viewBox` is specified.
   */
  PreserveAspectRatio preserveAspectRatio() const;

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
   * Get the `markerUnits` attribute which defines the coordinate system for attributes
   * `markerWidth`, `markerHeight`, and the contents of the marker.
   *
   * Defaults to \ref MarkerUnits::StrokeWidth.
   *
   * @return Coordinate system for marker attributes and contents.
   */
  MarkerUnits markerUnits() const;

  /**
   * Set the `markerUnits` attribute which defines the coordinate system for attributes
   * `markerWidth`, `markerHeight`, and the contents of the marker.
   *
   * @param value Coordinate system for marker attributes and contents.
   */
  void setMarkerUnits(MarkerUnits value);

  /**
   * Set the orientation of the marker, the `orient` attribute.
   *
   * @param value Orientation of the marker relative to the path.
   */
  void setOrient(MarkerOrient value);

  /**
   * Get the orientation of the marker, the `orient`  attribute.
   *
   * @return Orientation of the marker relative to the path.
   */
  MarkerOrient orient() const;
};

}  // namespace donner::svg
