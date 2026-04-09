#pragma once
/// @file

#include "donner/base/Box.h"
#include "donner/base/Length.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGTextContentElement.h"
#include "donner/svg/SVGTextPositioningElement.h"

namespace donner::svg {

/**
 * @page xml_text "<text>"
 *
 * Defines a graphics element consisting of text.
 *
 * - DOM object: SVGTextElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/text.html#TextElement
 *
 * The `<text>` element renders a string of text as a proper graphics primitive. Unlike text
 * rasterized into a bitmap, SVG text is fully live: it remains selectable and searchable,
 * scales cleanly to any resolution, and accepts every normal SVG presentation attribute —
 * `fill`, `stroke`, `opacity`, `transform`, gradients via `url(#id)`, filters, clip paths,
 * and so on. Fonts are loaded through standard CSS `@font-face` rules (TTF, OTF, WOFF, WOFF2)
 * or fall back to a built-in typeface when no match is available.
 *
 * Use `<text>` for labels, titles, axis tick marks, captions, and any other readable content.
 * Substrings inside a `<text>` can be re-styled or repositioned with \ref xml_tspan, and text
 * can be made to follow an arbitrary curve with \ref xml_textPath.
 *
 * ```
 * <text x="50" y="60">Hello, SVG text!</text>
 * ```
 *
 * \htmlonly
 * <svg width="300" height="120" style="background-color: white">
 *   <style>
 *     #xml_text text { font-size: 16px; font-weight: bold; fill: black }
 *     #xml_text line { stroke: black; stroke-width: 2px; stroke-dasharray: 6,4 }
 *   </style>
 *
 *   <text x="50" y="60">Hello, SVG text!</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute       | Default  | Description                                                                                                           |
 * | --------------: | :------: | :-------------------------------------------------------------------------------------------------------------------- |
 * | `lengthAdjust`  | `spacing` | `"spacing"` or `"spacingAndGlyphs"`. Indicates how the text may stretch (extra letter spacing only, or glyph-stretch as well). |
 * | `x`             | `0`      | If a single value is provided, sets the absolute x-position of the first character. If multiple values, sets positions per glyph. |
 * | `y`             | `0`      | Similar to `x`, but for y-position.                                                                                   |
 * | `dx`            | (none)   | One or more relative x shifts (optional).                                                                             |
 * | `dy`            | (none)   | One or more relative y shifts (optional).                                                                             |
 * | `rotate`        | (none)   | A list of rotation values per character (optional).                                                                   |
 * | `textLength`    | (none)   | Author-specified total text advance length for calibration.                                                           |
 *
 * \note For multi-value attributes (`x`, `y`, `dx`, `dy`, `rotate`), additional entries beyond the
 * number of glyphs do nothing. If fewer entries than glyphs exist, the final value is reused for
 * the remaining glyphs (for `rotate`), or the shift stays at 0 if no `dx/dy` is available.
 */

/**
 * DOM object for a \ref xml_text element.
 *
 * Text rendering supports `<text>`, \ref xml_tspan for sub-spans, and \ref xml_textPath for text
 * along a path. Fonts are loaded from `@font-face` rules (TTF, OTF, WOFF, WOFF2) or a built-in
 * fallback. With the `text_full` build config, HarfBuzz provides complex script shaping.
 *
 * \htmlonly
 * <svg width="300" height="120" style="background-color: white">
 *   <style>
 *     #xml_text text { font-size: 16px; font-weight: bold; fill: black }
 *     #xml_text line { stroke: black; stroke-width: 2px; stroke-dasharray: 6,4 }
 *   </style>
 *
 *   <text x="50" y="60">Hello, SVG text!</text>
 * </svg>
 * \endhtmlonly
 */
class SVGTextElement : public SVGTextPositioningElement {
  friend class parser::SVGParserImpl;

private:
  /// Create an SVGTextElement wrapper from an entity.
  explicit SVGTextElement(EntityHandle handle) : SVGTextPositioningElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGTextElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Text;
  /// XML tag name, \ref xml_text.
  static constexpr std::string_view Tag{"text"};

  static_assert(SVGTextPositioningElement::IsBaseOf(Type));
  static_assert(SVGTextContentElement::IsBaseOf(Type));
  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_text element within the specified document.
   *
   * @param document Containing document.
   */
  static SVGTextElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

  /**
   * Convert this text element to positioned glyph outlines, one \ref Path per glyph.
   *
   * Useful for custom rendering, export to path-only formats, or computing text geometry without
   * the full rendering pipeline.
   */
  std::vector<Path> convertToPath() const;

  /**
   * Return the tight bounding box of the actual rendered glyphs (ink extents). This is the
   * smallest rectangle that encloses all painted pixels.
   */
  Box2d inkBoundingBox() const;

  /**
   * Return the object bounding box as defined by SVG2. This is used for resolving
   * `objectBoundingBox` units in gradients, patterns, and clip paths applied to this text. It may
   * differ from inkBoundingBox() due to line-height and baseline positioning.
   */
  Box2d objectBoundingBox() const;
};

}  // namespace donner::svg
