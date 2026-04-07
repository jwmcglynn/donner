#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_feFuncG "<feFuncG>"
 *
 * A child element of \ref xml_feComponentTransfer defining the transfer function for the green
 * channel.
 *
 * - DOM object: SVGFEFuncGElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#funcGElement
 */

/**
 * DOM object for a \ref xml_feFuncG element.
 */
class SVGFEFuncGElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEFuncGElement(EntityHandle handle) : SVGElement(handle) {}

  static SVGFEFuncGElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeFuncG;
  /// XML tag name, \ref xml_feFuncG.
  static constexpr std::string_view Tag{"feFuncG"};
};

}  // namespace donner::svg
