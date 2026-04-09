#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feBlend "<feBlend>"
 *
 * `<feBlend>` combines two inputs using a blend mode, similar to the layer blend modes in
 * Photoshop, GIMP, or Figma. The result takes the colors of `in` (the "top" layer) and `in2`
 * (the "bottom" layer) and merges them pixel-by-pixel according to the formula for the chosen
 * mode.
 *
 * If you have ever stacked two layers in an image editor and changed the top layer's blend mode
 * from "Normal" to "Multiply" or "Screen", `<feBlend>` does the same thing inside an SVG filter
 * graph. It does **not** decide which pixels are kept (that is what \ref xml_feComposite does);
 * it only decides how the colors of two already-overlapping layers are mixed.
 *
 * - DOM object: SVGFEBlendElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feBlendElement
 *
 * ## All 16 blend modes
 *
 * The grid below shows every blend mode SVG supports. Each cell overlaps a red circle on top of
 * a blue circle using the named mode.
 *
 * \note For the preview images below, browsers render each cell with CSS `mix-blend-mode`
 *       because `<feImage>` references to inline elements are not universally supported across
 *       browsers. The actual `<feBlend>` primitive produces identical pixel results — the XML
 *       snippet further down is what Donner executes.
 *
 * \htmlonly
 * <svg xmlns="http://www.w3.org/2000/svg" width="340" height="360"
 *      style="background-color: white" font-family="sans-serif" font-size="9">
 *   <defs>
 *     <style>
 *       .xml_feBlend_bg { fill: #3a6ae4; }
 *       .xml_feBlend_fg { fill: #e43a3a; }
 *       .xml_feBlend_normal      .xml_feBlend_fg { mix-blend-mode: normal; }
 *       .xml_feBlend_multiply    .xml_feBlend_fg { mix-blend-mode: multiply; }
 *       .xml_feBlend_screen      .xml_feBlend_fg { mix-blend-mode: screen; }
 *       .xml_feBlend_overlay     .xml_feBlend_fg { mix-blend-mode: overlay; }
 *       .xml_feBlend_darken      .xml_feBlend_fg { mix-blend-mode: darken; }
 *       .xml_feBlend_lighten     .xml_feBlend_fg { mix-blend-mode: lighten; }
 *       .xml_feBlend_colordodge  .xml_feBlend_fg { mix-blend-mode: color-dodge; }
 *       .xml_feBlend_colorburn   .xml_feBlend_fg { mix-blend-mode: color-burn; }
 *       .xml_feBlend_hardlight   .xml_feBlend_fg { mix-blend-mode: hard-light; }
 *       .xml_feBlend_softlight   .xml_feBlend_fg { mix-blend-mode: soft-light; }
 *       .xml_feBlend_difference  .xml_feBlend_fg { mix-blend-mode: difference; }
 *       .xml_feBlend_exclusion   .xml_feBlend_fg { mix-blend-mode: exclusion; }
 *       .xml_feBlend_hue         .xml_feBlend_fg { mix-blend-mode: hue; }
 *       .xml_feBlend_saturation  .xml_feBlend_fg { mix-blend-mode: saturation; }
 *       .xml_feBlend_color       .xml_feBlend_fg { mix-blend-mode: color; }
 *       .xml_feBlend_luminosity  .xml_feBlend_fg { mix-blend-mode: luminosity; }
 *     </style>
 *     <symbol id="xml_feBlend_cell" viewBox="0 0 80 80" width="80" height="80">
 *       <rect width="80" height="80" fill="#f4f4f4" />
 *       <circle class="xml_feBlend_bg" cx="32" cy="40" r="22" />
 *       <circle class="xml_feBlend_fg" cx="50" cy="40" r="22" />
 *     </symbol>
 *   </defs>
 *   <g transform="translate(10,10)">
 *     <g class="xml_feBlend_normal"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="94" text-anchor="middle">normal</text>
 *   </g>
 *   <g transform="translate(90,10)">
 *     <g class="xml_feBlend_multiply"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="94" text-anchor="middle">multiply</text>
 *   </g>
 *   <g transform="translate(170,10)">
 *     <g class="xml_feBlend_screen"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="94" text-anchor="middle">screen</text>
 *   </g>
 *   <g transform="translate(250,10)">
 *     <g class="xml_feBlend_overlay"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="94" text-anchor="middle">overlay</text>
 *   </g>
 *   <g transform="translate(10,100)">
 *     <g class="xml_feBlend_darken"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="184" text-anchor="middle">darken</text>
 *   </g>
 *   <g transform="translate(90,100)">
 *     <g class="xml_feBlend_lighten"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="184" text-anchor="middle">lighten</text>
 *   </g>
 *   <g transform="translate(170,100)">
 *     <g class="xml_feBlend_colordodge"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="184" text-anchor="middle">color-dodge</text>
 *   </g>
 *   <g transform="translate(250,100)">
 *     <g class="xml_feBlend_colorburn"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="184" text-anchor="middle">color-burn</text>
 *   </g>
 *   <g transform="translate(10,190)">
 *     <g class="xml_feBlend_hardlight"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="274" text-anchor="middle">hard-light</text>
 *   </g>
 *   <g transform="translate(90,190)">
 *     <g class="xml_feBlend_softlight"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="274" text-anchor="middle">soft-light</text>
 *   </g>
 *   <g transform="translate(170,190)">
 *     <g class="xml_feBlend_difference"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="274" text-anchor="middle">difference</text>
 *   </g>
 *   <g transform="translate(250,190)">
 *     <g class="xml_feBlend_exclusion"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="274" text-anchor="middle">exclusion</text>
 *   </g>
 *   <g transform="translate(10,280)">
 *     <g class="xml_feBlend_hue"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="364" text-anchor="middle">hue</text>
 *   </g>
 *   <g transform="translate(90,280)">
 *     <g class="xml_feBlend_saturation"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="364" text-anchor="middle">saturation</text>
 *   </g>
 *   <g transform="translate(170,280)">
 *     <g class="xml_feBlend_color"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="364" text-anchor="middle">color</text>
 *   </g>
 *   <g transform="translate(250,280)">
 *     <g class="xml_feBlend_luminosity"><use href="#xml_feBlend_cell" /></g>
 *     <text x="40" y="364" text-anchor="middle">luminosity</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ## Mode families
 *
 * The 16 modes are grouped into five families by what they do to the underlying pixels:
 *
 * - **Darken family** — `multiply`, `darken`, `color-burn`. The result is never brighter than
 *   either input; white acts as a no-op.
 * - **Lighten family** — `screen`, `lighten`, `color-dodge`. The result is never darker than
 *   either input; black acts as a no-op.
 * - **Contrast family** — `overlay`, `hard-light`, `soft-light`. Darkens dark areas and
 *   brightens bright areas, increasing contrast.
 * - **Difference family** — `difference`, `exclusion`. Produce inverting or contrast-reversing
 *   effects based on the distance between the two colors.
 * - **HSL family** — `hue`, `saturation`, `color`, `luminosity`. Mix channels of the
 *   hue/saturation/luminance color model rather than RGB, letting you transplant one aspect of
 *   one color onto another.
 *
 * ## Example
 *
 * A minimal filter that multiplies the source graphic by a solid red background:
 *
 * ```xml
 * <filter id="MyMultiply">
 *   <feFlood flood-color="#e43a3a" result="red" />
 *   <feBlend in="SourceGraphic" in2="red" mode="multiply" />
 * </filter>
 * ```
 *
 * ## Attributes
 *
 * | Attribute | Default    | Description |
 * | --------: | :--------: | :---------- |
 * | `in`      | *previous* | First (top) input, named filter result or one of `SourceGraphic`, `SourceAlpha`, `FillPaint`, `StrokePaint`. |
 * | `in2`     | *(none)*   | Second (bottom) input. Required. |
 * | `mode`    | `normal`   | One of `normal`, `multiply`, `screen`, `overlay`, `darken`, `lighten`, `color-dodge`, `color-burn`, `hard-light`, `soft-light`, `difference`, `exclusion`, `hue`, `saturation`, `color`, `luminosity`. |
 *
 * Inherits standard filter primitive attributes from \ref SVGFilterPrimitiveStandardAttributes
 * (`x`, `y`, `width`, `height`, `result`).
 */

/**
 * DOM object for a \ref xml_feBlend element.
 *
 * Composites input image `in` over `in2` using a CSS blend mode.
 */
class SVGFEBlendElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEBlendElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  static SVGFEBlendElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeBlend;
  /// XML tag name, \ref xml_feBlend.
  static constexpr std::string_view Tag{"feBlend"};
};

}  // namespace donner::svg
