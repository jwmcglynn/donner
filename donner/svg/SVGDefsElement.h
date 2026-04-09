#pragma once
/// @file

#include "donner/svg/SVGGraphicsElement.h"

namespace donner::svg {

/**
 * @page xml_defs "<defs>"
 *
 * Container for \b definitions of reusable graphics elements. It is not rendered directly,
 * but its child elements can be referenced by a \ref xml_use or within a `fill` or `stroke`.
 *
 * - DOM object: SVGDefsElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/struct.html#DefsElement
 *
 * The `<defs>` element is SVG's "library" section: a place to declare reusable building blocks
 * that should not be drawn where they appear. Anything inside `<defs>` is parsed and kept in
 * memory, but is only rendered when something else explicitly references it — paint servers
 * via `fill="url(#id)"` or `stroke="url(#id)"`, `<use>` elements via `href="#id"`, filters via
 * `filter="url(#id)"`, and so on.
 *
 * By convention, almost all reusable content lives inside `<defs>`: \ref xml_linearGradient and
 * \ref xml_radialGradient paint servers, \ref xml_pattern tiles, \ref xml_filter effects,
 * \ref xml_clipPath and \ref xml_mask regions, \ref xml_marker arrowheads, and \ref xml_symbol
 * templates. Using `<defs>` keeps the drawable part of your document clean and signals intent
 * to both tools and human readers.
 *
 * ```xml
 * <svg width="300" height="160">
 *   <defs>
 *     <linearGradient id="MyGradient" x1="0" y1="0" x2="1" y2="0">
 *       <stop offset="0%" stop-color="#6cf" />
 *       <stop offset="100%" stop-color="#f6c" />
 *     </linearGradient>
 *   </defs>
 *   <rect x="170" y="30" width="110" height="100" fill="url(#MyGradient)" />
 * </svg>
 * ```
 *
 * The `<defs>` block itself is never rendered, but its children are referenced elsewhere via
 * `id`. In the diagram below, the gradient is defined inside `<defs>` (left) and then referenced
 * by the rectangle (right) via `fill="url(#MyGradient)"`.
 *
 * \htmlonly
 * <svg id="xml_defs" width="320" height="160" style="background-color: white">
 *   <style>
 *     #xml_defs text { font-family: monospace; font-size: 12px; fill: black }
 *     #xml_defs text.label { font-family: sans-serif; font-size: 12px; font-weight: bold; fill: black }
 *     #xml_defs rect.box { fill: none; stroke: #888; stroke-width: 1.5; stroke-dasharray: 4,3 }
 *     #xml_defs line.ref { stroke: #c33; stroke-width: 1.5; stroke-dasharray: 4,3 }
 *   </style>
 *   <defs>
 *     <linearGradient id="xml_defs_MyGradient" x1="0" y1="0" x2="1" y2="0">
 *       <stop offset="0%" stop-color="#6cf" />
 *       <stop offset="100%" stop-color="#f6c" />
 *     </linearGradient>
 *     <marker id="xml_defs_arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">
 *       <path d="M 0 0 L 10 5 L 0 10 z" fill="#c33" />
 *     </marker>
 *   </defs>
 *
 *   <rect class="box" x="10" y="20" width="140" height="120" />
 *   <text class="label" x="14" y="15">&lt;defs&gt;</text>
 *   <text x="20" y="55">&lt;linearGradient</text>
 *   <text x="28" y="72">id="MyGradient"&gt;</text>
 *   <text x="28" y="88">&lt;stop .../&gt;</text>
 *   <text x="28" y="104">&lt;stop .../&gt;</text>
 *   <text x="20" y="120">&lt;/linearGradient&gt;</text>
 *
 *   <rect x="190" y="40" width="110" height="90" fill="url(#xml_defs_MyGradient)" stroke="black" stroke-width="1" />
 *   <text class="label" x="194" y="35">&lt;rect fill="url(#MyGradient)"/&gt;</text>
 *
 *   <line class="ref" x1="150" y1="85" x2="188" y2="85" marker-end="url(#xml_defs_arrow)" />
 * </svg>
 * \endhtmlonly
 */

/**
 * DOM object for a \ref xml_defs element.
 *
 * This element and its children are never rendered directly, but may be referenced by other
 * elements, such as \ref xml_use.
 */
class SVGDefsElement : public SVGGraphicsElement {
  friend class parser::SVGParserImpl;

private:
  /// Create an SVGDefsElement wrapper from an entity.
  explicit SVGDefsElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGDefsElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Defs;
  /// XML tag name, \ref xml_defs.
  static constexpr std::string_view Tag{"defs"};

  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_defs element.
   *
   * @param document Containing document.
   */
  static SVGDefsElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }
};

}  // namespace donner::svg
