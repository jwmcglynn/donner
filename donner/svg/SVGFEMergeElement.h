#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feMerge "<feMerge>"
 *
 * `<feMerge>` stacks multiple filter results on top of each other like layers in an image
 * editor, producing a final composite. It takes **zero direct attributes of its own** — instead,
 * its child \ref xml_feMergeNode elements name the intermediate results to layer. Layers are
 * stacked in document order, **bottom to top**: the first `<feMergeNode>` becomes the bottom
 * layer, and each subsequent one is painted over it.
 *
 * `<feMerge>` is the "glue" that lets a filter chain produce outputs that combine several
 * intermediate steps. For example, a drop shadow filter needs to paint the shadow *and* the
 * original graphic in the final result — `<feMerge>` is what unions those two pieces together.
 *
 * Every merge is just a stack of source-over composites, so `<feMerge>` is strictly simpler
 * (and clearer) than writing a chain of \ref xml_feComposite primitives by hand.
 *
 * - DOM object: SVGFEMergeElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feMergeElement
 *
 * ## Children
 *
 * `<feMerge>` expects one or more \ref xml_feMergeNode children. Each `<feMergeNode>` has a
 * single attribute `in` that names the filter result to layer. `<feMerge>` does nothing on its
 * own — all of its behavior is driven by its children. See \ref xml_feMergeNode for the full
 * details of the child element.
 *
 * ```xml
 * <feMerge>
 *   <feMergeNode in="bottomLayer" />   <!-- drawn first, ends up on the bottom -->
 *   <feMergeNode in="middleLayer" />
 *   <feMergeNode in="SourceGraphic" /> <!-- drawn last, ends up on top -->
 * </feMerge>
 * ```
 *
 * ## Example 1: drop shadow
 *
 * A classic drop shadow is built from four primitives: take the source's alpha, blur it,
 * offset it down and to the right, then merge that shadow *underneath* the original graphic.
 *
 * \htmlonly
 * <svg xmlns="http://www.w3.org/2000/svg" width="300" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feMerge_dropShadow" x="-30%" y="-30%" width="160%" height="160%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="3" result="blur" />
 *       <feOffset in="blur" dx="4" dy="4" result="offsetBlur" />
 *       <feMerge>
 *         <feMergeNode in="offsetBlur" />
 *         <feMergeNode in="SourceGraphic" />
 *       </feMerge>
 *     </filter>
 *   </defs>
 *   <rect x="30" y="30" width="80" height="60" rx="6" fill="#4a90e2"
 *         filter="url(#xml_feMerge_dropShadow)" />
 *   <text x="70" y="120" text-anchor="middle">Drop shadow</text>
 *   <text x="200" y="55" font-size="11">1. Blur SourceAlpha</text>
 *   <text x="200" y="75" font-size="11">2. Offset the blur</text>
 *   <text x="200" y="95" font-size="11">3. Merge: blur under graphic</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="DropShadow" x="-30%" y="-30%" width="160%" height="160%">
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="3" result="blur" />
 *   <feOffset    in="blur"        dx="4" dy="4" result="offsetBlur" />
 *   <feMerge>
 *     <feMergeNode in="offsetBlur" />      <!-- bottom layer: the shadow -->
 *     <feMergeNode in="SourceGraphic" />   <!-- top layer:    the original shape -->
 *   </feMerge>
 * </filter>
 * ```
 *
 * **What each primitive does:**
 * 1. `feGaussianBlur` reads `SourceAlpha` (the source shape's opacity, painted black) and
 *    blurs it, producing a soft dark silhouette.
 * 2. `feOffset` shifts that blur 4 pixels right and 4 pixels down so it peeks out from under
 *    the source.
 * 3. `feMerge` stacks the offset blur on the bottom, then the original `SourceGraphic` on
 *    top. The shadow is visible only where the source doesn't cover it.
 *
 * ## Example 2: double halo / glow outline
 *
 * You can merge more than two layers. This example draws two blurred copies of the source at
 * different radii (a wide soft halo and a tighter sharp one) behind the original, producing
 * a two-tone glow effect.
 *
 * \htmlonly
 * <svg xmlns="http://www.w3.org/2000/svg" width="300" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feMerge_halo" x="-50%" y="-50%" width="200%" height="200%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="6" result="wideBlur" />
 *       <feColorMatrix in="wideBlur" result="wideGlow"
 *           values="0 0 0 0 1  0 0 0 0 0.6  0 0 0 0 0.2  0 0 0 1 0" />
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="2" result="tightBlur" />
 *       <feColorMatrix in="tightBlur" result="tightGlow"
 *           values="0 0 0 0 1  0 0 0 0 1    0 0 0 0 0.4  0 0 0 1 0" />
 *       <feMerge>
 *         <feMergeNode in="wideGlow" />
 *         <feMergeNode in="tightGlow" />
 *         <feMergeNode in="SourceGraphic" />
 *       </feMerge>
 *     </filter>
 *   </defs>
 *   <circle cx="80" cy="70" r="28" fill="#222"
 *           filter="url(#xml_feMerge_halo)" />
 *   <text x="80" y="120" text-anchor="middle">Two-tone halo</text>
 *   <text x="170" y="55" font-size="11">Bottom: wide orange blur</text>
 *   <text x="170" y="75" font-size="11">Middle: tight yellow blur</text>
 *   <text x="170" y="95" font-size="11">Top:    original shape</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="Halo" x="-50%" y="-50%" width="200%" height="200%">
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="6" result="wideBlur" />
 *   <feColorMatrix  in="wideBlur"  result="wideGlow"
 *       values="0 0 0 0 1  0 0 0 0 0.6  0 0 0 0 0.2  0 0 0 1 0" />
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="2" result="tightBlur" />
 *   <feColorMatrix  in="tightBlur" result="tightGlow"
 *       values="0 0 0 0 1  0 0 0 0 1    0 0 0 0 0.4  0 0 0 1 0" />
 *   <feMerge>
 *     <feMergeNode in="wideGlow" />      <!-- bottom: wide orange halo -->
 *     <feMergeNode in="tightGlow" />     <!-- middle: inner yellow glow -->
 *     <feMergeNode in="SourceGraphic" /> <!-- top:    original shape -->
 *   </feMerge>
 * </filter>
 * ```
 *
 * **What each layer contributes:**
 * - `wideGlow` is a heavily blurred (`stdDeviation="6"`) copy of the source alpha recolored
 *   orange. Because it is the first merge node, it lives at the bottom and forms the outer
 *   halo.
 * - `tightGlow` is a lightly blurred (`stdDeviation="2"`) copy recolored yellow. Stacked
 *   above the orange halo, it gives the inner portion of the glow a warmer highlight.
 * - `SourceGraphic` is stacked last so the unfiltered shape is drawn crisply on top of
 *   both glows.
 *
 * ## Attributes
 *
 * `<feMerge>` has **no element-specific attributes**. Only the standard filter primitive
 * attributes apply (see below).
 *
 * Inherits standard filter primitive attributes from \ref SVGFilterPrimitiveStandardAttributes
 * (`x`, `y`, `width`, `height`, `result`).
 *
 * \see \ref xml_feMergeNode for the child element that actually names the layers, and
 *      \ref xml_feComposite for the lower-level operator that `<feMerge>` is a stacked
 *      shorthand for.
 */

/**
 * DOM object for a \ref xml_feMerge element.
 */
class SVGFEMergeElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEMergeElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  static SVGFEMergeElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeMerge;
  /// XML tag name, \ref xml_feMerge.
  static constexpr std::string_view Tag{"feMerge"};
};

}  // namespace donner::svg
