#pragma once
/// @file

#include <optional>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/svg/SVGAElement.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"

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
   * Result of a successful \ref hitTestLink query: the enclosing \ref xml_a element that was hit,
   * its retained link target, and the concrete element under the point.
   */
  struct LinkHit {
    /// The enclosing `<a>` element whose content was hit.
    SVGAElement linkElement;

    /// The raw, unresolved link target as authored on the `<a>` element (`href` / `xlink:href`),
    /// for example `"#section"`, `"../other.svg"`, or `"https://example.com/"`. The embedding
    /// application is responsible for resolving this reference against the document base URL and
    /// performing any navigation; Donner returns the target verbatim and never navigates itself.
    RcString href;

    /// The concrete painted descendant actually under the point (e.g. the nested `<rect>` inside
    /// the `<a>`). An `<a>` paints nothing of its own, so this is always a descendant of \ref
    /// linkElement, never the `<a>` element itself.
    SVGGraphicsElement hitElement;
  };

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
  std::optional<SVGGraphicsElement> findIntersecting(const Vector2d& point);

  /**
   * Return every painted element intersecting p point, front to back.
   *
   * This supports scoped editor hit testing without allowing an element
   * outside the active scope to occlude an eligible descendant.
   */
  std::vector<SVGGraphicsElement> findAllIntersecting(const Vector2d& point);

  /**
   * Finds the hyperlink (\ref xml_a) at the given point, if any, resolving the enclosing-`<a>`
   * semantics from the SVG linking model (https://www.w3.org/TR/SVG2/linking.html#Links).
   *
   * The embedding application drives this query directly from its own pointer events, so the same
   * call serves both link activation (on click / tap) and link affordances (on hover, e.g. a
   * pointer-cursor change, hover highlight, or tooltip). Donner intentionally exposes a stateless
   * query rather than registering navigation callbacks: it has no event loop and never navigates,
   * so the application keeps full control over what a hover versus a click does with the returned
   * target.
   *
   * Hit resolution reuses \ref findIntersecting, so it honors `pointer-events`, `visibility`,
   * `display`, paint-order, transforms, and per-shape fill/stroke geometry, and it is independent
   * of the active renderer (TinySkia or Geode). A point that lands on any descendant of an `<a>`
   * resolves to that `<a>` (the nearest ancestor `<a>` with a link target), matching the SVG
   * requirement that the whole subtree of a link is clickable. If the topmost painted element at
   * the point is not inside any `<a>`, no link is returned even when a lower, occluded element
   * would have been linked.
   *
   * The point is in SVG canvas coordinates (the same coordinate space as the root `<svg>`
   * element's viewBox), identical to \ref findIntersecting.
   *
   * Resolution follows the document-tree ancestor chain, which has two consequences worth noting:
   * a `<a>` wrapping graphics (or a whole `<text>`) resolves normally, but an inline `<a>` span
   * nested *inside* a `<text>` resolves to that enclosing `<text>` rather than to the inline span,
   * because text is hit-tested at text-element granularity. Content injected through `<use>` is
   * resolved by the referenced subtree's own ancestry.
   *
   * ```cpp
   * DonnerController controller(document);
   * if (auto link = controller.hitTestLink(cursorPosition)) {
   *   // On hover: show a pointer cursor / highlight link->linkElement.
   *   // On click: navigate to link->href (the app resolves and follows it).
   *   app.openUrl(link->href);
   * }
   * ```
   *
   * @param point Position in canvas coordinates.
   * @return The enclosing link, its raw target, and the hit element, or \c std::nullopt if no link
   *   is at the point.
   */
  std::optional<LinkHit> hitTestLink(const Vector2d& point);

private:
  SVGDocument document_;
};

}  // namespace donner::svg
