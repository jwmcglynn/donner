#pragma once
/// @file

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"

namespace donner::svg {

/**
 * Allows querying and controlling the Donner scene, beyond what the DOM provides.
 */
class DonnerController {
public:
  /**
   * Constructor to create the controller from a given \ref SVGDocument. Allows querying and
   * controlling the SVG contents beyond what the DOM provides.
   */
  explicit DonnerController(SVGDocument document);

  /**
   * Finds the first element that intersects the given point.
   *
   * @param point Pointer position to find the intersecting element for
   */
  std::optional<SVGGeometryElement> findIntersecting(const Vector2d& point);

private:
  SVGDocument document_;
};

}  // namespace donner::svg
