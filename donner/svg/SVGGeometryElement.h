#pragma once
/// @file

#include "donner/svg/SVGGraphicsElement.h"

namespace donner::svg {

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
  /// Get the length of the path in user units. To override this value for `stroke-dasharray` and
  /// other path-offset-relative values, use \ref setPathLength.
  double computedPathLength() const;

  /// Get the path length override, if set. To get the computed path length, use \ref
  /// computedPathLength().
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
