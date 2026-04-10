#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_feMergeNode "<feMergeNode>"
 *
 * `<feMergeNode>` is a child of \ref xml_feMerge that names **one layer** to stack. Each
 * `<feMergeNode>` references a named filter result via its `in` attribute, and the parent
 * `<feMerge>` layers them in document order, **bottom to top**.
 *
 * On its own, `<feMergeNode>` does nothing — it only has meaning inside a `<feMerge>`. Think
 * of `<feMerge>` as an ordered list and each `<feMergeNode>` as an entry in that list pointing
 * at an earlier filter result.
 *
 * - DOM object: SVGFEMergeNodeElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#elementdef-femergenode
 *
 * ## Mini example: which node is bottom, which is top?
 *
 * ```xml
 * <feMerge>
 *   <feMergeNode in="shadow" />         <!-- first child  -> BOTTOM layer -->
 *   <feMergeNode in="SourceGraphic" />  <!-- last child   -> TOP layer    -->
 * </feMerge>
 * ```
 *
 * The first `<feMergeNode>` in document order is painted first and ends up at the bottom of
 * the final composite. Each subsequent `<feMergeNode>` is painted on top of the previous one
 * using source-over compositing.
 *
 * ## Full example: drop shadow
 *
 * The most common place you will see `<feMergeNode>` is inside a drop-shadow filter. (This
 * is the same example shown on the \ref xml_feMerge page — duplicated here so you can read
 * either page stand-alone.)
 *
 * \htmlonly
 * <svg xmlns="http://www.w3.org/2000/svg" width="300" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feMergeNode_dropShadow" x="-30%" y="-30%" width="160%" height="160%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="3" result="blur" />
 *       <feOffset in="blur" dx="4" dy="4" result="offsetBlur" />
 *       <feMerge>
 *         <feMergeNode in="offsetBlur" />
 *         <feMergeNode in="SourceGraphic" />
 *       </feMerge>
 *     </filter>
 *   </defs>
 *   <rect x="30" y="30" width="80" height="60" rx="6" fill="#4a90e2"
 *         filter="url(#xml_feMergeNode_dropShadow)" />
 *   <text x="70" y="120" text-anchor="middle">Drop shadow</text>
 *   <text x="200" y="60" font-size="11">Node 1 (bottom):</text>
 *   <text x="200" y="76" font-size="11">  offsetBlur — the shadow</text>
 *   <text x="200" y="96" font-size="11">Node 2 (top):</text>
 *   <text x="200" y="112" font-size="11">  SourceGraphic — the shape</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="DropShadow" x="-30%" y="-30%" width="160%" height="160%">
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="3" result="blur" />
 *   <feOffset    in="blur"        dx="4" dy="4" result="offsetBlur" />
 *   <feMerge>
 *     <feMergeNode in="offsetBlur" />      <!-- bottom: the soft offset shadow -->
 *     <feMergeNode in="SourceGraphic" />   <!-- top:    the original graphic   -->
 *   </feMerge>
 * </filter>
 * ```
 *
 * The two `<feMergeNode>` children are the entire reason `<feMerge>` exists: the first one
 * points at the blurred, offset shadow image built earlier in the filter chain; the second
 * points at the unfiltered source. `<feMerge>` then paints them in that order, bottom to top,
 * so the shape hides most of the shadow and only the offset portion shows.
 *
 * ## Attributes
 *
 * | Attribute | Default       | Description |
 * | --------: | :-----------: | :---------- |
 * | `in`      | *previous*    | Name of the filter result to use as this layer. Either a `result` name defined earlier in the same `<filter>`, or one of the standard sources `SourceGraphic`, `SourceAlpha`, `BackgroundImage`, `BackgroundAlpha`, `FillPaint`, `StrokePaint`. |
 *
 * \note Unlike most filter primitives, `<feMergeNode>` **does not** inherit the standard
 *       filter primitive attributes. It has no `x`, `y`, `width`, `height`, or `result`
 *       attribute — its sole job is to point at an existing result and contribute it as a
 *       layer to the parent `<feMerge>`.
 *
 * \see \ref xml_feMerge — the parent element. `<feMergeNode>` is only valid as a direct child
 *      of `<feMerge>` and has no effect outside that context.
 */

/**
 * DOM object for a \ref xml_feMergeNode element.
 */
class SVGFEMergeNodeElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEMergeNodeElement wrapper from an entity.
  explicit SVGFEMergeNodeElement(EntityHandle handle) : SVGElement(handle) {}

  /// Internal constructor to create the element on an existing \ref donner::Entity.
  static SVGFEMergeNodeElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeMergeNode;
  /// XML tag name, \ref xml_feMergeNode.
  static constexpr std::string_view Tag{"feMergeNode"};
};

}  // namespace donner::svg
