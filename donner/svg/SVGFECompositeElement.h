#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feComposite "<feComposite>"
 *
 * `<feComposite>` merges two inputs (`in` and `in2`) into a single output using a
 * **Porter-Duff compositing operator**. Unlike \ref xml_feBlend, which mixes the *colors* of
 * two layers, `<feComposite>` primarily decides **which pixels are kept** based on the shapes'
 * alpha channels. It is how you do shape-based masking, intersection, or subtraction inside an
 * SVG filter graph.
 *
 * If you have ever used boolean shape operations ("intersect", "subtract", "exclude") in a
 * vector editor, Porter-Duff operators are the per-pixel equivalent applied to alpha.
 *
 * - DOM object: SVGFECompositeElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feCompositeElement
 *
 * ## Mental model
 *
 * Throughout this page we refer to the two inputs as **A** (`in`, the red circle) and
 * **B** (`in2`, the blue circle). The diagram below shows what each operator produces when A
 * and B overlap:
 *
 * \htmlonly
 * <svg xmlns="http://www.w3.org/2000/svg" width="560" height="170"
 *      style="background-color: white" font-family="sans-serif" font-size="11">
 *   <g transform="translate(10,10)">
 *     <text x="60" y="12" text-anchor="middle" font-weight="bold">A (in) over B (in2)</text>
 *     <circle cx="48" cy="75" r="32" fill="#3a6ae4" />
 *     <circle cx="76" cy="75" r="32" fill="#e43a3a" />
 *     <text x="36" y="138" text-anchor="middle" fill="#3a6ae4">B</text>
 *     <text x="88" y="138" text-anchor="middle" fill="#e43a3a">A</text>
 *   </g>
 *   <g transform="translate(150,10)">
 *     <text x="40" y="12" text-anchor="middle" font-weight="bold">over</text>
 *     <rect width="80" height="100" y="20" fill="#f4f4f4" />
 *     <circle cx="32" cy="70" r="26" fill="#3a6ae4" />
 *     <circle cx="48" cy="70" r="26" fill="#e43a3a" />
 *     <text x="40" y="138" text-anchor="middle" font-size="9">A on top of B</text>
 *   </g>
 *   <g transform="translate(240,10)">
 *     <text x="40" y="12" text-anchor="middle" font-weight="bold">in</text>
 *     <rect width="80" height="100" y="20" fill="#f4f4f4" />
 *     <clipPath id="xml_feComposite_clipB">
 *       <circle cx="32" cy="70" r="26" />
 *     </clipPath>
 *     <circle cx="48" cy="70" r="26" fill="#e43a3a" clip-path="url(#xml_feComposite_clipB)" />
 *     <text x="40" y="138" text-anchor="middle" font-size="9">A clipped to B</text>
 *   </g>
 *   <g transform="translate(330,10)">
 *     <text x="40" y="12" text-anchor="middle" font-weight="bold">out</text>
 *     <rect width="80" height="100" y="20" fill="#f4f4f4" />
 *     <mask id="xml_feComposite_maskOut">
 *       <rect width="80" height="100" y="20" fill="white" />
 *       <circle cx="32" cy="70" r="26" fill="black" />
 *     </mask>
 *     <circle cx="48" cy="70" r="26" fill="#e43a3a" mask="url(#xml_feComposite_maskOut)" />
 *     <text x="40" y="138" text-anchor="middle" font-size="9">A with B punched out</text>
 *   </g>
 *   <g transform="translate(420,10)">
 *     <text x="40" y="12" text-anchor="middle" font-weight="bold">atop</text>
 *     <rect width="80" height="100" y="20" fill="#f4f4f4" />
 *     <clipPath id="xml_feComposite_clipAtop">
 *       <circle cx="32" cy="70" r="26" />
 *     </clipPath>
 *     <g clip-path="url(#xml_feComposite_clipAtop)">
 *       <circle cx="32" cy="70" r="26" fill="#3a6ae4" />
 *       <circle cx="48" cy="70" r="26" fill="#e43a3a" />
 *     </g>
 *     <text x="40" y="138" text-anchor="middle" font-size="9">A over B, clipped to B</text>
 *   </g>
 *   <g transform="translate(150,150)" />
 * </svg>
 * <svg xmlns="http://www.w3.org/2000/svg" width="560" height="160"
 *      style="background-color: white" font-family="sans-serif" font-size="11">
 *   <g transform="translate(10,10)">
 *     <text x="40" y="12" text-anchor="middle" font-weight="bold">xor</text>
 *     <rect width="80" height="100" y="20" fill="#f4f4f4" />
 *     <mask id="xml_feComposite_maskXorA">
 *       <rect width="80" height="100" y="20" fill="white" />
 *       <circle cx="48" cy="70" r="26" fill="black" />
 *     </mask>
 *     <mask id="xml_feComposite_maskXorB">
 *       <rect width="80" height="100" y="20" fill="white" />
 *       <circle cx="32" cy="70" r="26" fill="black" />
 *     </mask>
 *     <circle cx="32" cy="70" r="26" fill="#3a6ae4" mask="url(#xml_feComposite_maskXorA)" />
 *     <circle cx="48" cy="70" r="26" fill="#e43a3a" mask="url(#xml_feComposite_maskXorB)" />
 *     <text x="40" y="138" text-anchor="middle" font-size="9">Non-overlapping parts</text>
 *   </g>
 *   <g transform="translate(100,10)">
 *     <text x="130" y="12" text-anchor="middle" font-weight="bold">arithmetic (k1=0, k2=1, k3=1, k4=0)</text>
 *     <rect x="90" width="80" height="100" y="20" fill="#f4f4f4" />
 *     <g style="mix-blend-mode: normal">
 *       <circle cx="122" cy="70" r="26" fill="#3a6ae4" />
 *       <circle cx="138" cy="70" r="26" fill="#e43a3a" />
 *     </g>
 *     <text x="130" y="138" text-anchor="middle" font-size="9">k1*A*B + k2*A + k3*B + k4</text>
 *     <text x="130" y="150" text-anchor="middle" font-size="9">Here: A + B (linear add)</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ## What each operator means
 *
 * - **`over`** (default) — "A drawn on top of B". A wins where both exist; where only B exists,
 *   B shows through. This is the normal "paint on top" behavior.
 * - **`in`** — "A clipped to B's shape". A is kept **only where B is opaque**; everywhere else
 *   is transparent. Useful for masking a texture to a shape.
 * - **`out`** — "A with B punched out". A is kept **only where B is transparent**. The inverse
 *   of `in` — useful for carving holes.
 * - **`atop`** — "A drawn on top of B, but only inside B's shape". Combines `in` (keeps the
 *   overlap) with B showing through outside that overlap.
 * - **`xor`** — "Either A or B, but not where they overlap". The overlap region becomes
 *   transparent.
 * - **`arithmetic`** — "Pixel-wise formula `k1*A*B + k2*A + k3*B + k4`". Gives you complete
 *   control over the per-channel result. Use it for cross-dissolves (`k2=α, k3=1-α`), custom
 *   blends, or specular-style highlights.
 *
 * ## Practical example: clipping a shadow to its shape
 *
 * A common use of `<feComposite operator="in">` is to clip a blurred copy of a shape back to
 * the original shape, producing an *inner* shadow/glow that stays within the shape's
 * boundaries:
 *
 * \htmlonly
 * <svg xmlns="http://www.w3.org/2000/svg" width="320" height="130"
 *      style="background-color: white" font-family="sans-serif" font-size="11">
 *   <defs>
 *     <filter id="xml_feComposite_innerGlow" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="6" result="blur" />
 *       <feComposite in="blur" in2="SourceAlpha" operator="in" result="clipped" />
 *       <feMerge>
 *         <feMergeNode in="SourceGraphic" />
 *         <feMergeNode in="clipped" />
 *       </feMerge>
 *     </filter>
 *   </defs>
 *   <rect x="20" y="20" width="90" height="90" rx="10" fill="#ffd166" />
 *   <text x="65" y="125" text-anchor="middle">Source</text>
 *   <rect x="150" y="20" width="90" height="90" rx="10" fill="#ffd166"
 *         filter="url(#xml_feComposite_innerGlow)" />
 *   <text x="195" y="125" text-anchor="middle">With inner shadow</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="InnerGlow">
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="6" result="blur" />
 *   <feComposite in="blur" in2="SourceAlpha" operator="in" result="clipped" />
 *   <feMerge>
 *     <feMergeNode in="SourceGraphic" />
 *     <feMergeNode in="clipped" />
 *   </feMerge>
 * </filter>
 * ```
 *
 * The blur would normally extend outside the shape — `<feComposite operator="in">` with
 * `in2="SourceAlpha"` keeps only the portion of the blur that falls *inside* the original
 * shape's silhouette, producing the inner-shadow effect.
 *
 * ## Attributes
 *
 * | Attribute  | Default    | Description |
 * | ---------: | :--------: | :---------- |
 * | `in`       | *previous* | First input (A). |
 * | `in2`      | *(none)*   | Second input (B). Required. |
 * | `operator` | `over`     | One of `over`, `in`, `out`, `atop`, `xor`, `arithmetic`. |
 * | `k1`       | `0`        | Arithmetic coefficient. Only used when `operator="arithmetic"`. |
 * | `k2`       | `0`        | Arithmetic coefficient. Only used when `operator="arithmetic"`. |
 * | `k3`       | `0`        | Arithmetic coefficient. Only used when `operator="arithmetic"`. |
 * | `k4`       | `0`        | Arithmetic coefficient. Only used when `operator="arithmetic"`. |
 *
 * Inherits standard filter primitive attributes from \ref SVGFilterPrimitiveStandardAttributes
 * (`x`, `y`, `width`, `height`, `result`).
 */

/**
 * DOM object for a \ref xml_feComposite element.
 *
 * Composites `in` over `in2` using the specified Porter-Duff operator. The `arithmetic` operator
 * uses the formula: `result = k1*in*in2 + k2*in + k3*in2 + k4`.
 */
class SVGFECompositeElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFECompositeElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  static SVGFECompositeElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeComposite;
  /// XML tag name, \ref xml_feComposite.
  static constexpr std::string_view Tag{"feComposite"};
};

}  // namespace donner::svg
