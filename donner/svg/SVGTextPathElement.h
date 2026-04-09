#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGTextContentElement.h"
#include "donner/svg/SVGTextPositioningElement.h"

namespace donner::svg {

/**
 * @page xml_textPath "<textPath>"
 *
 * The `<textPath>` element renders text along an arbitrary path referenced by the `href`
 * attribute. It must be a child of a \ref xml_text element.
 *
 * - DOM object: SVGTextPathElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/text.html#TextPathElement
 *
 * A `<textPath>` lets text flow along the curve of any \ref xml_path instead of in a straight
 * horizontal line — think of a label hugging the edge of a circle, a headline bent along a
 * wave, or caption text tracking the outline of a shape. You reference the target path by its
 * `id` via the `href` attribute, and SVG lays out each glyph along the path's geometry,
 * automatically rotating it to follow the local tangent. The `startOffset` attribute controls
 * where along the path the text begins, allowing fine adjustment or animation.
 *
 * `<textPath>` must always live inside a \ref xml_text element; it cannot be used on its own.
 *
 * \htmlonly
 * <svg id="xml_textPath" width="320" height="160" viewBox="0 0 320 160" style="background-color: white">
 *   <defs>
 *     <path id="xml_textPath_path" d="M 20 110 Q 160 -10 300 110" fill="none" />
 *   </defs>
 *   <use href="#xml_textPath_path" fill="none" stroke="#bbb" stroke-width="1" stroke-dasharray="4,3" />
 *   <text font-family="sans-serif" font-size="22" fill="#1f5a8a">
 *     <textPath href="#xml_textPath_path">Text flows along the path</textPath>
 *   </text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <defs>
 *   <path id="myPath" d="M 20 110 Q 160 -10 300 110" fill="none"/>
 * </defs>
 * <text font-size="22">
 *   <textPath href="#myPath">Text flows along the path</textPath>
 * </text>
 * ```
 *
 * | Attribute     | Default   | Description                                              |
 * | ------------: | :-------: | :------------------------------------------------------- |
 * | `href`        | (none)    | Reference to a `<path>` element                         |
 * | `startOffset` | `0`       | Offset along the path where text begins                  |
 * | `method`      | `align`   | How glyphs are placed: `align` or `stretch`              |
 * | `side`        | `left`    | Which side of the path: `left` or `right`                |
 * | `spacing`     | `exact`   | Spacing control: `auto` or `exact`                       |
 *
 */

/**
 * DOM object for a \ref xml_textPath element.
 *
 * The `<textPath>` element places text along an arbitrary SVG path, allowing text to follow
 * curves and complex shapes. It references a `<path>` element via the `href` attribute and
 * supports positioning along the path via `startOffset`.
 *
 * \see https://www.w3.org/TR/SVG2/text.html#TextPathElement
 */
class SVGTextPathElement : public SVGTextPositioningElement {
  friend class parser::SVGParserImpl;

private:
  /// Create an SVGTextPathElement wrapper from an entity.
  explicit SVGTextPathElement(EntityHandle handle) : SVGTextPositioningElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGTextPathElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::TextPath;
  /// XML tag name, \ref xml_textPath.
  static constexpr std::string_view Tag{"textPath"};

  static_assert(SVGTextPositioningElement::IsBaseOf(Type));
  static_assert(SVGTextContentElement::IsBaseOf(Type));
  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_textPath element within the specified document.
   *
   * @param document Containing document.
   */
  static SVGTextPathElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

  /**
   * Set the `href` attribute, which references the path element.
   *
   * @param href IRI reference (e.g., "#myPath").
   */
  void setHref(const RcStringOrRef& href);

  /**
   * Get the `href` attribute value.
   */
  std::optional<RcString> href() const;

  /**
   * Set the `startOffset` attribute.
   *
   * @param offset Offset along the path where text begins.
   */
  void setStartOffset(std::optional<Lengthd> offset);

  /**
   * Get the `startOffset` attribute value.
   */
  std::optional<Lengthd> startOffset() const;

private:
  /// Invalidate cached data from the render or layout tree.
  void invalidate() const;

  /// Recompute any text layout or bounding box if needed.
  void compute() const;
};

}  // namespace donner::svg
