#pragma once
/// @file

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"

namespace donner::svg {

/**
 * Provides hit-testing and spatial queries for an SVG document.
 *
 * Use DonnerController when you need to determine which element is at a given point (e.g., for
 * mouse interaction in a viewer). This supplements the DOM traversal API on \ref SVGElement with
 * geometry-aware queries.
 *
 * ```cpp
 * DonnerController controller(document);
 * if (auto element = controller.findIntersecting(Vector2d(100, 50))) {
 *   std::cout << "Hit: " << element->tagName() << "\n";
 * }
 * ```
 */
class DonnerController {
public:
  /**
   * Create a controller for the given document.
   *
   * @param document The SVG document to query.
   */
  explicit DonnerController(SVGDocument document);

  /**
   * Finds the topmost geometry element whose rendered area contains the given point.
   *
   * The point is in SVG canvas coordinates (the same coordinate space as the root `<svg>`
   * element's viewBox). Returns the deepest matching element in paint order (last painted =
   * topmost).
   *
   * @param point Position in canvas coordinates.
   * @return The topmost intersecting geometry element, or \c std::nullopt if no element is hit.
   */
  std::optional<SVGGeometryElement> findIntersecting(const Vector2d& point);

private:
  SVGDocument document_;
};

}  // namespace donner::svg
