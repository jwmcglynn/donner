#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @defgroup xml_feMerge "<feMerge>"
 *
 * Defines a filter primitive that composites multiple input layers using Source Over.
 *
 * - DOM object: SVGFEMergeElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feMergeElement
 *
 * Each child `<feMergeNode>` specifies an input via its `in` attribute. The layers are composited
 * bottom-to-top (first child on bottom).
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feMerge>
 *     <feMergeNode in="blur" />
 *     <feMergeNode in="SourceGraphic" />
 *   </feMerge>
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feMerge element.
 */
class SVGFEMergeElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEMergeElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  static SVGFEMergeElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeMerge;
  /// XML tag name, \ref xml_feMerge.
  static constexpr std::string_view Tag{"feMerge"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
