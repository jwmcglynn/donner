#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feDropShadow "<feDropShadow>"
 *
 * Defines a filter primitive that creates a drop shadow effect.
 *
 * - DOM object: SVGFEDropShadowElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feDropShadowElement
 *
 * Equivalent to: feGaussianBlur of SourceAlpha, offset by dx/dy, flooded with
 * flood-color/flood-opacity, then merged under SourceGraphic.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="shadow">
 *   <feDropShadow dx="3" dy="3" stdDeviation="2" flood-color="black" flood-opacity="0.5" />
 * </filter>
 * ```
 *
 * \htmlonly
 * <svg id="xml_feDropShadow_diagram" width="320" height="160" viewBox="0 0 320 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feDropShadow_filter" x="-20%" y="-20%" width="140%" height="140%">
 *       <feDropShadow dx="4" dy="4" stdDeviation="3" flood-color="black" flood-opacity="0.5" />
 *     </filter>
 *   </defs>
 *   <rect x="30" y="30" width="70" height="70" fill="#4a90e2" />
 *   <text x="30" y="120" fill="black">Source</text>
 *   <rect x="190" y="30" width="70" height="70" fill="#4a90e2" filter="url(#xml_feDropShadow_filter)" />
 *   <text x="170" y="120" fill="black">dx=4 dy=4 stdDev=3</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute       | Default | Description  |
 * | --------------: | :-----: | :----------- |
 * | `dx`            | `2`     | Horizontal offset of the shadow. |
 * | `dy`            | `2`     | Vertical offset of the shadow. |
 * | `stdDeviation`  | `2`     | Gaussian blur std. deviation applied to the shadow. |
 * | `flood-color`   | `black` | Shadow color (CSS property). |
 * | `flood-opacity` | `1`     | Shadow opacity (CSS property). |
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
 */

/**
 * DOM object for a \ref xml_feDropShadow element.
 */
class SVGFEDropShadowElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEDropShadowElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  static SVGFEDropShadowElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeDropShadow;
  /// XML tag name, \ref xml_feDropShadow.
  static constexpr std::string_view Tag{"feDropShadow"};
};

}  // namespace donner::svg
