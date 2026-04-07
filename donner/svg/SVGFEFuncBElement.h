#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_feFuncB "<feFuncB>"
 *
 * A child element of \ref xml_feComponentTransfer defining the transfer function for the blue
 * channel.
 *
 * - DOM object: SVGFEFuncBElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#funcBElement
 */

/**
 * DOM object for a \ref xml_feFuncB element.
 */
class SVGFEFuncBElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEFuncBElement(EntityHandle handle) : SVGElement(handle) {}

  static SVGFEFuncBElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeFuncB;
  /// XML tag name, \ref xml_feFuncB.
  static constexpr std::string_view Tag{"feFuncB"};
};

}  // namespace donner::svg
