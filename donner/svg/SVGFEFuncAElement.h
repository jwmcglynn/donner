#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_feFuncA "<feFuncA>"
 *
 * A child element of \ref xml_feComponentTransfer defining the transfer function for the alpha
 * channel.
 *
 * - DOM object: SVGFEFuncAElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#funcAElement
 */

/**
 * DOM object for a \ref xml_feFuncA element.
 */
class SVGFEFuncAElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEFuncAElement(EntityHandle handle) : SVGElement(handle) {}

  static SVGFEFuncAElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeFuncA;
  /// XML tag name, \ref xml_feFuncA.
  static constexpr std::string_view Tag{"feFuncA"};
};

}  // namespace donner::svg
