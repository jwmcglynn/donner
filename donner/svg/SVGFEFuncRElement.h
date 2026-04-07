#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @defgroup xml_feFuncR "<feFuncR>"
 *
 * A child element of \ref xml_feComponentTransfer defining the transfer function for the red
 * channel.
 *
 * - DOM object: SVGFEFuncRElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#funcRElement
 */

/**
 * DOM object for a \ref xml_feFuncR element.
 */
class SVGFEFuncRElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEFuncRElement(EntityHandle handle) : SVGElement(handle) {}

  static SVGFEFuncRElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeFuncR;
  /// XML tag name, \ref xml_feFuncR.
  static constexpr std::string_view Tag{"feFuncR"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
