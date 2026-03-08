#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @defgroup xml_feDropShadow "<feDropShadow>"
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
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
