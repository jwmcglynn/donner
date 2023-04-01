#pragma once
/// @file

#include "src/svg/svg_graphics_element.h"

namespace donner::svg {

/**
 * @defgroup basic_shapes Basic Shapes
 *
 * \details Basic shapes are defined by a combination of straight lines and curves, and may be
 * stroked, filled, or used as a clipping path.
 *
 * - SVG2 spec: https://www.w3.org/TR/SVG2/shapes.html
 *
 * Basic shapes have the DOM base class \ref SVGGeometryElement.
 */

/**
 * Base class for all \ref basic_shapes.
 *
 * This is the DOM base class for all basic shapes, such as \ref SVGCircleElement, \ref
 * SVGRectElement, \ref SVGPathElement, etc.
 */
class SVGGeometryElement : public SVGGraphicsElement {
protected:
  /**
   * Internal constructor to create an SVGGeometryElement from an \ref EntityHandle.
   *
   * To create a geometry element, use the static \ref Create methods on the derived class, such as
   * \ref SVGCircleElement::Create.
   *
   * @param handle EntityHandle to wrap.
   */
  explicit SVGGeometryElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  /// Get the path length override, if set.
  std::optional<double> pathLength() const;

  /**
   * Set the path length override.
   *
   * @param value New path length to set, which will scale path-offset-relative values such as
   * stroke-dasharray. If unset, the path length will be calculated automatically.
   */
  void setPathLength(std::optional<double> value);
};

}  // namespace donner::svg
