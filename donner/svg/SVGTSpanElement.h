#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGTextContentElement.h"
#include "donner/svg/SVGTextPositioningElement.h"

namespace donner::svg {

/**
 * @page xml_tspan "<tspan>"
 *
 * The `<tspan>` element defines a sub-string or sub-group of text that can be independently
 * positioned or styled inside an SVG text flow. Common usage includes changing the color,
 * weight, or position of a portion of text within a \ref xml_text element.
 *
 * - DOM object: SVGTSpanElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/text.html#TextElement
 *
 * ```svg
 * <text x="20" y="40">
 *   You are
 *   <tspan dx="10" fill="red" font-weight="bold">
 *     NOT
 *   </tspan>
 *   a banana.
 * </text>
 * ```
 *
 * \htmlonly
 * <svg width="360" height="140" style="background-color: white" id="xml_tspan">
 *   <style>
 *     #xml_tspan text {
 *       font-size: 18px;
 *       font-family: sans-serif;
 *       fill: black;
 *     }
 *   </style>
 *
 *   <text x="20" y="40">
 *     You are
 *     <tspan dx="10" fill="red" font-weight="bold">
 *       NOT
 *     </tspan>
 *     a banana.
 *   </text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description                                          |
 * | --------: | :-----: | :--------------------------------------------------- |
 * | `x`       | `0`     | Absolute X position(s) for the first (or each) glyph |
 * | `y`       | `0`     | Absolute Y position(s) for the first (or each) glyph |
 * | `dx`      | (none)  | Relative X shift(s) for glyphs                       |
 * | `dy`      | (none)  | Relative Y shift(s) for glyphs                       |
 * | `rotate`  | (none)  | Rotation(s) for each glyph in degrees                |
 *
 */

/**
 * DOM object for a \ref xml_tspan element.
 *
 * The `<tspan>` element creates a sub-span of text within a `<text>` (or nested `<tspan>`),
 * allowing partial style changes or explicit repositioning of a portion of text.  It
 * supports the per-glyph positioning attributes (`x`, `y`, `dx`, `dy`, `rotate`) that
 * let you fine-tune the layout of text runs.
 *
 * \see https://www.w3.org/TR/SVG2/text.html#TSpanElement
 *
 * \htmlonly
 * <svg width="360" height="140" style="background-color: white" id="xml_tspan">
 *   <style>
 *     #xml_tspan text {
 *       font-size: 18px;
 *       font-family: sans-serif;
 *       fill: black;
 *     }
 *   </style>
 *
 *   <text x="20" y="40">
 *     You are
 *     <tspan dx="10" fill="red" font-weight="bold">
 *       NOT
 *     </tspan>
 *     a banana.
 *   </text>
 * </svg>
 * \endhtmlonly
 */
class SVGTSpanElement : public SVGTextPositioningElement {
  friend class parser::SVGParserImpl;

private:
  /// Create an SVGTSpanElement wrapper from an entity.
  explicit SVGTSpanElement(EntityHandle handle) : SVGTextPositioningElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGTSpanElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::TSpan;
  /// XML tag name, \ref xml_tspan.
  static constexpr std::string_view Tag{"tspan"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;

  static_assert(SVGTextPositioningElement::IsBaseOf(Type));
  static_assert(SVGTextContentElement::IsBaseOf(Type));
  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_tspan element within the specified document.
   *
   * @param document Containing document.
   */
  static SVGTSpanElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

private:
  /// Invalidate cached data from the render or layout tree.
  void invalidate() const;

  /// Recompute any text layout or bounding box if needed.
  void compute() const;
};

}  // namespace donner::svg
