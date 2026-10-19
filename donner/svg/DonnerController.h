#pragma once
/// @file

#include <optional>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/svg/SVGAElement.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/core/Cursor.h"
#include "donner/svg/core/HitTestMode.h"

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
  /// Immutable hit geometry for one rendered document state. No live layout access during queries.
  class HitTestSnapshot {
  public:
    /// Find the frontmost hit in the captured document geometry.
    /// @param point Point in document coordinates.
    /// @return The frontmost captured element containing \p point, or \c std::nullopt when none
    /// does or the capture is incomplete.
    std::optional<SVGGraphicsElement> findIntersecting(const Vector2d& point) const;
    /// False when capture exceeded its resource budget; queries then fail closed.
    bool complete() const;

  private:
    friend class DonnerController;
    struct Impl;
    explicit HitTestSnapshot(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<const Impl> impl_;
  };

  /// Capture current hit geometry and targets for interaction with the corresponding rendered
  /// frame. Targets resolve as for \ref findIntersecting, so they remain valid document elements
  /// after later render-tree rebuilds.
  /// @return A snapshot; check \ref HitTestSnapshot::complete for budget exhaustion.
  std::shared_ptr<const HitTestSnapshot> captureHitTest();

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
    /// the `<a>`), or the `<use>` element whose content was hit. An `<a>` paints nothing of its
    /// own, so this is always a descendant of \ref linkElement, never the `<a>` element itself.
    SVGGraphicsElement hitElement;
  };

  /**
   * Create a controller for the given document.
   *
   * @param document The SVG document to query.
   */
  explicit DonnerController(SVGDocument document);

  /**
   * Finds the topmost rendered element (a shape, or a `<text>` element when text support is
   * enabled) whose rendered area contains the given point.
   *
   * The point is in document coordinates: the user space of the root `<svg>` element's viewBox.
   * To hit-test a canvas pixel, map it through the inverse of
   * \ref SVGDocument::canvasFromDocumentTransform first. Returns the last-painted (topmost)
   * matching element.
   *
   * Content rendered by a `<use>` element resolves to that `<use>` element (the outermost one for
   * nested `<use>` instances), matching SVG 2 event retargeting for `<use>` instances. Pattern,
   * mask and marker content is painted as part of the element that references it and is never
   * returned.
   *
   * @param point Position in document coordinates.
   * @param mode SVG event targeting by default; EditorSelection ignores pointer-events while
   * retaining visible painted geometry. Neither mode changes the document's properties.
   * @return The topmost intersecting element, or \c std::nullopt if no element is hit.
   */
  std::optional<SVGGraphicsElement> findIntersecting(
      const Vector2d& point, HitTestMode mode = HitTestMode::SvgPointerEvents);

  /**
   * Return every painted element intersecting \p point, front to back.
   *
   * This supports scoped editor hit testing without allowing an element
   * outside the active scope to occlude an eligible descendant. Targets resolve as for
   * \ref findIntersecting, and each element is reported once, at its frontmost hit.
   *
   * @param point Position in document coordinates, as for \ref findIntersecting.
   * @param mode Hit eligibility policy, with the same semantics as findIntersecting.
   * @return Intersecting elements, topmost first, or an empty vector if no element is hit.
   */
  std::vector<SVGGraphicsElement> findAllIntersecting(
      const Vector2d& point, HitTestMode mode = HitTestMode::SvgPointerEvents);

  /**
   * Returns the inherited CSS cursor value for the topmost interactive element at a point. Cursor
   * image URLs are returned as authored, without resolving them against a base URL.
   *
   * @param point Position in document coordinates, as for \ref findIntersecting.
   * @return Ordered image candidates and keyword fallback, or `std::nullopt` when no element is
   * hit.
   */
  std::optional<Cursor> cursorAt(const Vector2d& point);

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
   * The point is in document coordinates (the user space of the root `<svg>` element's viewBox),
   * identical to \ref findIntersecting.
   *
   * Resolution follows the document-tree ancestor chain, which has two consequences worth noting:
   * a `<a>` wrapping graphics (or a whole `<text>`) resolves normally, but an inline `<a>` span
   * nested *inside* a `<text>` resolves to that enclosing `<text>` rather than to the inline span,
   * because text is hit-tested at text-element granularity. Content rendered through `<use>`
   * resolves to the `<use>` element, so a link applies when it encloses that `<use>` element.
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
   * @param point Position in document coordinates.
   * @return The enclosing link, its raw target, and the hit element, or \c std::nullopt if no link
   *   is at the point.
   */
  std::optional<LinkHit> hitTestLink(const Vector2d& point);

private:
  /// Resolve a rendered hit to the document-tree graphics element that receives its events. Hits
  /// on `<use>` content resolve to the `<use>` element, which outlives the shadow tree rebuilt with
  /// the render tree.
  /// @param registry Document registry. @param hit Entity returned by the render-tree hit test.
  static std::optional<SVGGraphicsElement> HitTarget(Registry& registry, Entity hit);

  SVGDocument document_;
};

}  // namespace donner::svg
