#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feImage "<feImage>"
 *
 * `<feImage>` imports an external image — or a fragment of the current document — into the
 * filter graph as an additional input. It's how you use a photograph as a lighting source,
 * mask, or displacement map, or how you reuse an existing SVG element's rendering as an input
 * to another primitive like \ref xml_feDisplacementMap or \ref xml_feComposite.
 *
 * Conceptually, `<feImage>` is a tiny "load this picture into the filter canvas" primitive. It
 * doesn't take a filter input (no `in` attribute); instead, its source is pointed at by the
 * `href` attribute. The result fills the primitive subregion at the `<feImage>`'s `x`, `y`,
 * `width`, `height`, and downstream primitives can use it just like any other input.
 *
 * - DOM object: SVGFEImageElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feImageElement
 *
 * ## How `href` works
 *
 * The `href` attribute accepts either:
 *
 * - An **external URL** pointing at an image file (PNG, JPEG, SVG, etc.), e.g.
 *   `href="texture.png"` or `href="https://example.com/photo.jpg"`. The file is loaded and
 *   its pixels are rasterized into the filter subregion.
 * - A **fragment reference** of the form `#id` pointing at another element in the same
 *   document, e.g. `href="#myShape"`. The referenced element is rendered to its own bounding
 *   box, and the resulting pixels are handed to the filter as if they were an image.
 *
 * ## How `preserveAspectRatio` works
 *
 * When the source image's aspect ratio doesn't match the destination rectangle (the filter
 * primitive subregion), `preserveAspectRatio` decides whether to stretch, letterbox, or crop
 * the image so that it fits.
 *
 * The attribute is a pair: `<align> <meetOrSlice>`.
 *
 * - **`<align>`** picks which corner or edge of the source is aligned to which corner or edge
 *   of the destination when there's extra space. Valid values: `none`, `xMinYMin`, `xMidYMin`,
 *   `xMaxYMin`, `xMinYMid`, `xMidYMid` (the default), `xMaxYMid`, `xMinYMax`, `xMidYMax`,
 *   `xMaxYMax`. The `x*` part controls horizontal alignment (`Min` = left, `Mid` = center,
 *   `Max` = right); the `Y*` part controls vertical alignment (`Min` = top, `Mid` = middle,
 *   `Max` = bottom).
 * - **`<meetOrSlice>`** picks the fitting mode:
 *   - **`meet`** (the default) — scale the image uniformly so it fits *entirely* inside the
 *     destination. If aspect ratios differ, you get letterboxing (empty bars on one axis).
 *     Think "fit".
 *   - **`slice`** — scale the image uniformly so it *entirely covers* the destination.
 *     If aspect ratios differ, the image is cropped on one axis. Think "fill".
 * - **`none`** as the alignment value disables aspect-ratio preservation entirely: the image
 *   is stretched non-uniformly to exactly fill the destination rectangle. `<meetOrSlice>` is
 *   ignored when alignment is `none`.
 *
 * The default is `xMidYMid meet`.
 *
 * The diagram below shows the same square source image fitted into a wider landscape
 * destination rectangle under `meet`, `slice`, and `none`. The dashed box is the destination;
 * the shaded area shows where the image pixels actually land:
 *
 * \htmlonly
 * <svg width="420" height="150" viewBox="0 0 420 150" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <pattern id="xml_feImage_par_src" width="20" height="20" patternUnits="userSpaceOnUse">
 *       <rect width="20" height="20" fill="#cfe8ff"/>
 *       <path d="M0 0 L20 20 M20 0 L0 20" stroke="#3b82c4" stroke-width="1"/>
 *     </pattern>
 *   </defs>
 *   <g transform="translate(10,20)">
 *     <text x="60" y="-4" text-anchor="middle" fill="#444">meet (fit)</text>
 *     <rect x="0" y="0" width="120" height="80" fill="none" stroke="#888" stroke-dasharray="3,3"/>
 *     <rect x="20" y="0" width="80" height="80" fill="url(#xml_feImage_par_src)" stroke="#3b82c4"/>
 *     <text x="60" y="100" text-anchor="middle" fill="#666">letterboxed</text>
 *   </g>
 *   <g transform="translate(150,20)">
 *     <text x="60" y="-4" text-anchor="middle" fill="#444">slice (fill)</text>
 *     <rect x="0" y="0" width="120" height="80" fill="none" stroke="#888" stroke-dasharray="3,3"/>
 *     <clipPath id="xml_feImage_par_clip"><rect x="0" y="0" width="120" height="80"/></clipPath>
 *     <rect x="0" y="-20" width="120" height="120" fill="url(#xml_feImage_par_src)" stroke="#3b82c4" clip-path="url(#xml_feImage_par_clip)"/>
 *     <text x="60" y="100" text-anchor="middle" fill="#666">cropped</text>
 *   </g>
 *   <g transform="translate(290,20)">
 *     <text x="60" y="-4" text-anchor="middle" fill="#444">none (stretch)</text>
 *     <rect x="0" y="0" width="120" height="80" fill="none" stroke="#888" stroke-dasharray="3,3"/>
 *     <rect x="0" y="0" width="120" height="80" fill="url(#xml_feImage_par_src)" stroke="#3b82c4"/>
 *     <text x="60" y="100" text-anchor="middle" fill="#666">distorted</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ## Example 1: external image as a filter input
 *
 * The simplest use of `<feImage>`: load an external PNG and output it as the filter result. The
 * filter completely replaces the source graphic with the loaded image. In practice you'd
 * usually combine it with a compositing primitive to mix it with the source:
 *
 * \htmlonly
 * <svg width="260" height="130" viewBox="0 0 260 130" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <pattern id="xml_feImage_ex1_tex" width="14" height="14" patternUnits="userSpaceOnUse">
 *       <rect width="14" height="14" fill="#ffd580"/>
 *       <circle cx="7" cy="7" r="3" fill="#e67a00"/>
 *     </pattern>
 *     <filter id="xml_feImage_ex1_f">
 *       <feImage href="#xml_feImage_ex1_srcimg"/>
 *     </filter>
 *     <rect id="xml_feImage_ex1_srcimg" width="90" height="70" fill="url(#xml_feImage_ex1_tex)"/>
 *   </defs>
 *   <rect x="30" y="25" width="90" height="70" fill="steelblue"/>
 *   <text x="75" y="110" text-anchor="middle" fill="#444">source shape</text>
 *   <rect x="150" y="25" width="90" height="70" fill="steelblue" filter="url(#xml_feImage_ex1_f)"/>
 *   <text x="195" y="110" text-anchor="middle" fill="#444">filter = feImage only</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="LoadImage">
 *   <feImage href="texture.png" />
 * </filter>
 * <rect width="90" height="70" filter="url(#LoadImage)" />
 * ```
 *
 * ## Example 2: referencing an inline element by id
 *
 * `href="#star"` points at another element in the same document. The filter re-renders that
 * element into its own bounding box and uses those pixels as the filter output. This is a
 * handy way to reuse a shape (or a group of shapes) as input to a downstream primitive without
 * duplicating geometry:
 *
 * \htmlonly
 * <svg width="260" height="130" viewBox="0 0 260 130" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <polygon id="xml_feImage_ex2_star" points="45,5 55,35 85,35 60,55 70,85 45,65 20,85 30,55 5,35 35,35" fill="gold" stroke="orange" stroke-width="2"/>
 *     <filter id="xml_feImage_ex2_f">
 *       <feImage href="#xml_feImage_ex2_star"/>
 *     </filter>
 *   </defs>
 *   <rect x="30" y="25" width="90" height="80" fill="steelblue"/>
 *   <text x="75" y="122" text-anchor="middle" fill="#444">source rect</text>
 *   <rect x="150" y="25" width="90" height="80" fill="steelblue" filter="url(#xml_feImage_ex2_f)"/>
 *   <text x="195" y="122" text-anchor="middle" fill="#444">feImage href="#star"</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <defs>
 *   <polygon id="star" points="45,5 55,35 85,35 60,55 70,85 45,65 20,85 30,55 5,35 35,35"
 *            fill="gold" stroke="orange" />
 *   <filter id="UseStar">
 *     <feImage href="#star" />
 *   </filter>
 * </defs>
 * <rect width="90" height="80" filter="url(#UseStar)" />
 * ```
 *
 * ## Example 3: feeding feDisplacementMap
 *
 * Here `<feImage>` produces a gradient, which is then fed to `<feDisplacementMap>` as the
 * displacement source. The gradient's red and green channels push the source graphic's pixels
 * around, producing a warp. This is a common pattern for effects that need a custom "map"
 * input (lighting normals, displacement fields, texture lookups):
 *
 * \htmlonly
 * <svg width="260" height="140" viewBox="0 0 260 140" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <linearGradient id="xml_feImage_ex3_grad" x1="0" y1="0" x2="1" y2="1">
 *       <stop offset="0" stop-color="#000"/>
 *       <stop offset="1" stop-color="#fff"/>
 *     </linearGradient>
 *     <rect id="xml_feImage_ex3_map" width="100" height="80" fill="url(#xml_feImage_ex3_grad)"/>
 *     <filter id="xml_feImage_ex3_f">
 *       <feImage href="#xml_feImage_ex3_map" result="map"/>
 *       <feDisplacementMap in="SourceGraphic" in2="map" scale="20"/>
 *     </filter>
 *   </defs>
 *   <g font-size="28" font-weight="bold">
 *     <text x="30" y="70" fill="steelblue">Warp</text>
 *     <text x="30" y="122" font-size="12" font-weight="normal" fill="#444">original</text>
 *     <text x="150" y="70" fill="steelblue" filter="url(#xml_feImage_ex3_f)">Warp</text>
 *     <text x="150" y="122" font-size="12" font-weight="normal" fill="#444">displaced by feImage</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="Warp">
 *   <feImage href="#gradient" result="map" />
 *   <feDisplacementMap in="SourceGraphic" in2="map" scale="20" />
 * </filter>
 * ```
 *
 * ## Attributes
 *
 * | Attribute             | Default         | Description |
 * | --------------------: | :-------------: | :---------- |
 * | `href`                | (none)          | URL or in-document fragment reference (`#id`) for the source image. |
 * | `preserveAspectRatio` | `xMidYMid meet` | How the source is fitted into the primitive subregion when aspect ratios differ. See above. |
 * | `crossorigin`         | (none)          | CORS setting for external URL loads (`anonymous` or `use-credentials`). |
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
 */

/**
 * DOM object for a \ref xml_feImage element.
 *
 * Fetches image data from an external resource (via `href`) or renders a referenced SVG element
 * fragment, providing the result as filter output.
 */
class SVGFEImageElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEImageElement wrapper from an entity.
  explicit SVGFEImageElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEImageElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeImage;
  /// XML tag name, \ref xml_feImage.
  static constexpr std::string_view Tag{"feImage"};
};

}  // namespace donner::svg
