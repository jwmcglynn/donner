#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGTextContentElement.h"
#include "donner/svg/components/text/TextFlowComponent.h"
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
 * Unlike shapes (e.g., circles, rectangles), text does not produce
 * path geometry in the same way. Instead, it manages glyph placement.
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
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;

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

  void addFlowRegion(components::FlowRegion region) {
    handle_.get<components::TextFlowComponent>().regions.push_back(std::move(region));
  }

  void setFlowAlignment(std::optional<components::FlowAlignment> alignment) {
    handle_.get<components::TextFlowComponent>().alignment = alignment;
  }

  void setFlowOverflow(std::optional<Overflow> overflow) {
    handle_.get<components::TextFlowComponent>().overflow = overflow;
  }

  const components::TextFlowComponent& flowComponent() const {
    return handle_.get<components::TextFlowComponent>();
  }
};

}  // namespace donner::svg
