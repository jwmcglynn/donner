#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feTurbulence "<feTurbulence>"
 *
 * `<feTurbulence>` generates a procedural noise pattern using the Perlin turbulence algorithm.
 * It's the underlying primitive for paper textures, cloud effects, marbling, water distortions,
 * and pseudo-random fills. Unlike a bitmap, the noise is computed per pixel at render time, so
 * it scales losslessly to any resolution.
 *
 * `<feTurbulence>` takes no input image. It simply fills the filter primitive subregion with
 * computed noise values, which downstream primitives (like \ref xml_feDisplacementMap or
 * \ref xml_feComposite) can then use as a texture, distortion field, or mask.
 *
 * - DOM object: SVGFETurbulenceElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feTurbulenceElement
 *
 * ## `baseFrequency` — the scale of noise features
 *
 * Controls how "zoomed in" the noise is. Lower values produce larger blobs; higher values
 * produce finer grain. You can specify one number (same for X and Y) or two (separate X and Y
 * frequencies, allowing stretched noise).
 *
 * \htmlonly
 * <svg width="420" height="150" viewBox="0 0 420 150" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feTurb_bf_lo"><feTurbulence type="fractalNoise" baseFrequency="0.02" numOctaves="2" seed="1"/></filter>
 *     <filter id="xml_feTurb_bf_md"><feTurbulence type="fractalNoise" baseFrequency="0.05" numOctaves="2" seed="1"/></filter>
 *     <filter id="xml_feTurb_bf_hi"><feTurbulence type="fractalNoise" baseFrequency="0.15" numOctaves="2" seed="1"/></filter>
 *   </defs>
 *   <g transform="translate(15,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_bf_lo)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">0.02 (large blobs)</text>
 *   </g>
 *   <g transform="translate(150,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_bf_md)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">0.05 (medium)</text>
 *   </g>
 *   <g transform="translate(285,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_bf_hi)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">0.15 (fine grain)</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ## `numOctaves` — how many layers of detail
 *
 * Each octave adds a finer layer of noise on top of the previous one. More octaves produces
 * richer, more natural-looking texture at the cost of more computation per pixel.
 *
 * \htmlonly
 * <svg width="420" height="150" viewBox="0 0 420 150" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feTurb_oc_1"><feTurbulence type="fractalNoise" baseFrequency="0.03" numOctaves="1" seed="3"/></filter>
 *     <filter id="xml_feTurb_oc_3"><feTurbulence type="fractalNoise" baseFrequency="0.03" numOctaves="3" seed="3"/></filter>
 *     <filter id="xml_feTurb_oc_5"><feTurbulence type="fractalNoise" baseFrequency="0.03" numOctaves="5" seed="3"/></filter>
 *   </defs>
 *   <g transform="translate(15,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_oc_1)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">numOctaves=1</text>
 *   </g>
 *   <g transform="translate(150,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_oc_3)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">numOctaves=3</text>
 *   </g>
 *   <g transform="translate(285,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_oc_5)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">numOctaves=5</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ## `type` — turbulence vs fractalNoise
 *
 * The two supported noise types have a visibly different character:
 *
 * - **`turbulence`** takes the absolute value of Perlin noise, producing sharper, cloudier,
 *   more contrasted patterns — good for marble, fire, smoke.
 * - **`fractalNoise`** leaves the signed noise alone and remaps it to [0, 1], producing
 *   softer, more balanced patterns — good for clouds, paper, organic textures.
 *
 * \htmlonly
 * <svg width="290" height="150" viewBox="0 0 290 150" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feTurb_t_turb"><feTurbulence type="turbulence" baseFrequency="0.04" numOctaves="3" seed="7"/></filter>
 *     <filter id="xml_feTurb_t_frac"><feTurbulence type="fractalNoise" baseFrequency="0.04" numOctaves="3" seed="7"/></filter>
 *   </defs>
 *   <g transform="translate(15,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_t_turb)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">turbulence</text>
 *   </g>
 *   <g transform="translate(155,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_t_frac)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">fractalNoise</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ## `seed` — choosing a different random pattern
 *
 * Same frequency and octaves, different `seed`. The structure is identical but the specific
 * pattern of light and dark changes — use this when two identical noises would look too
 * repetitive, or to pick a pattern you like:
 *
 * \htmlonly
 * <svg width="290" height="150" viewBox="0 0 290 150" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feTurb_sd_1"><feTurbulence type="fractalNoise" baseFrequency="0.05" numOctaves="3" seed="1"/></filter>
 *     <filter id="xml_feTurb_sd_42"><feTurbulence type="fractalNoise" baseFrequency="0.05" numOctaves="3" seed="42"/></filter>
 *   </defs>
 *   <g transform="translate(15,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_sd_1)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">seed=1</text>
 *   </g>
 *   <g transform="translate(155,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_sd_42)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">seed=42</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ## `stitchTiles` — tileable noise
 *
 * Normally the noise doesn't line up at the edges of the primitive subregion, so if you tile
 * it you can see seams. Setting `stitchTiles="stitch"` adjusts the frequency slightly so the
 * pattern matches up at the edges, producing seamlessly tileable noise:
 *
 * \htmlonly
 * <svg width="290" height="150" viewBox="0 0 290 150" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feTurb_st_no"><feTurbulence type="fractalNoise" baseFrequency="0.05" numOctaves="3" seed="9" stitchTiles="noStitch"/></filter>
 *     <filter id="xml_feTurb_st_yes"><feTurbulence type="fractalNoise" baseFrequency="0.05" numOctaves="3" seed="9" stitchTiles="stitch"/></filter>
 *   </defs>
 *   <g transform="translate(15,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_st_no)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">noStitch (default)</text>
 *   </g>
 *   <g transform="translate(155,15)">
 *     <rect width="120" height="90" filter="url(#xml_feTurb_st_yes)"/>
 *     <text x="60" y="108" text-anchor="middle" fill="#444">stitch</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ## Practical example: watery distortion on text
 *
 * Combine `<feTurbulence>` with `<feDisplacementMap>` to push the source graphic's pixels
 * around according to the noise values. The result is a classic "watery ripple" distortion.
 * The `scale` attribute on the displacement map controls how strong the push is:
 *
 * \htmlonly
 * <svg width="320" height="110" viewBox="0 0 320 110" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feTurb_practical">
 *       <feTurbulence type="turbulence" baseFrequency="0.03" numOctaves="2" seed="5" result="noise"/>
 *       <feDisplacementMap in="SourceGraphic" in2="noise" scale="12"/>
 *     </filter>
 *   </defs>
 *   <text x="20" y="60" font-size="36" font-weight="bold" fill="steelblue" filter="url(#xml_feTurb_practical)">Ripples</text>
 *   <text x="20" y="95" fill="#444">feTurbulence + feDisplacementMap</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="Ripples">
 *   <feTurbulence type="turbulence" baseFrequency="0.03" numOctaves="2" seed="5" result="noise" />
 *   <feDisplacementMap in="SourceGraphic" in2="noise" scale="12" />
 * </filter>
 * <text filter="url(#Ripples)">Ripples</text>
 * ```
 *
 * ## Attributes
 *
 * | Attribute        | Default       | Description |
 * | ---------------: | :-----------: | :---------- |
 * | `baseFrequency`  | `0`           | Noise scale. One or two numbers (`fx` or `fx fy`). Lower = larger features. |
 * | `numOctaves`     | `1`           | Number of detail layers summed together. Higher = finer structure and more cost. |
 * | `seed`           | `0`           | Integer seed for the pseudo-random generator. Changes the pattern without changing its structure. |
 * | `stitchTiles`    | `noStitch`    | Either `noStitch` or `stitch`. `stitch` produces a seamlessly tileable result. |
 * | `type`           | `turbulence`  | Either `turbulence` (sharper, cloudier) or `fractalNoise` (softer). |
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
 */

/**
 * DOM object for a \ref xml_feTurbulence element.
 *
 * Generates a procedural noise texture using the Perlin noise algorithm. Supports both
 * turbulence and fractalNoise types with configurable frequency, octave count, and seed.
 */
class SVGFETurbulenceElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFETurbulenceElement wrapper from an entity.
  explicit SVGFETurbulenceElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFETurbulenceElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeTurbulence;
  /// XML tag name, \ref xml_feTurbulence.
  static constexpr std::string_view Tag{"feTurbulence"};
};

}  // namespace donner::svg
