#pragma once
/// @file

#include "donner/base/Box.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/core/CursorType.h"
#include "donner/svg/core/Event.h"

namespace donner::svg {

/**
 * Allows querying and controlling the Donner scene, beyond what the DOM provides.
 *
 * Provides spatial queries for hit testing, event dispatch, and element picking, using a spatial
 * grid acceleration structure for documents with many elements.
 */
class DonnerController {
public:
  /**
   * Constructor to create the controller from a given \ref SVGDocument. Allows querying and
   * controlling the SVG contents beyond what the DOM provides.
   */
  explicit DonnerController(SVGDocument document);

  /// @name Spatial Queries
  /// @{

  /**
   * Finds the first element that intersects the given point.
   *
   * @param point Pointer position to find the intersecting element for
   */
  std::optional<SVGGeometryElement> findIntersecting(const Vector2d& point);

  /**
   * Finds all elements that intersect the given point, ordered front-to-back (highest draw order
   * first). Unlike \ref findIntersecting which returns only the topmost hit, this returns every
   * element under the pointer.
   *
   * @param point Pointer position to find intersecting elements for
   */
  std::vector<SVGElement> findAllIntersecting(const Vector2d& point);

  /**
   * Finds all elements whose world-space bounds intersect the given rectangle, ordered
   * front-to-back. Useful for marquee/lasso selection in editors.
   *
   * @param rect Selection rectangle in document coordinates
   */
  std::vector<SVGElement> findIntersectingRect(const Boxd& rect);

  /**
   * Get the world-space bounding box of an element. Returns the shape's AABB in
   * document coordinates (fill bounds, not including stroke width).
   *
   * @param element The element to query
   * @return The world-space AABB, or std::nullopt if the element has no renderable shape
   */
  std::optional<Boxd> getWorldBounds(SVGElement element);

  /// @}
  /// @name Event Dispatch
  /// @{

  /**
   * Dispatch a DOM event. Hit-tests to find the target element (if not already set), then
   * propagates through capture, target, and bubble phases.
   *
   * @param event The event to dispatch. If `event.target` is `entt::null`, it will be set
   *              via hit testing using `event.documentPosition`.
   * @return The target element, or std::nullopt if no element was hit.
   */
  std::optional<SVGElement> dispatchEvent(Event& event);

  /**
   * Update hover state for the given pointer position. Call this on every mouse move for correct
   * mouseenter/mouseleave/mouseover/mouseout event semantics.
   *
   * @param point Pointer position in document coordinates.
   */
  void updateHover(const Vector2d& point);

  /**
   * Get the element currently under the pointer (the hovered element).
   *
   * @return The hovered element, or std::nullopt if the pointer is not over any element.
   */
  std::optional<SVGElement> hoveredElement();

  /// @}
  /// @name Cursor
  /// @{

  /**
   * Get the cursor type that should be displayed at the given point. Performs hit testing
   * to find the element under the point, then resolves the CSS `cursor` property from the
   * element's computed style (inherited from ancestors if not set directly).
   *
   * @param point Position in document coordinates.
   * @return The resolved cursor type, or CursorType::Auto if no element is hit.
   */
  CursorType getCursorAt(const Vector2d& point);

  /// @}

private:
  SVGDocument document_;
};

}  // namespace donner::svg
