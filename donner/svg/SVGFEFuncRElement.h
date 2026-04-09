#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_feFuncR "<feFuncR>"
 *
 * A child element of \ref xml_feComponentTransfer defining the transfer function for the red
 * channel.
 *
 * - DOM object: SVGFEFuncRElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#funcRElement
 *
 * Example usage:
 *
 * ```xml
 * <filter id="InvertRed">
 *   <feComponentTransfer>
 *     <feFuncR type="table" tableValues="1 0" />
 *   </feComponentTransfer>
 * </filter>
 * ```
 *
 * \htmlonly
 * <svg id="xml_feFuncR_diagram" width="320" height="160" viewBox="0 0 320 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <linearGradient id="xml_feFuncR_grad" x1="0" y1="0" x2="1" y2="0">
 *       <stop offset="0%" stop-color="black" />
 *       <stop offset="100%" stop-color="white" />
 *     </linearGradient>
 *     <filter id="xml_feFuncR_filter" x="-5%" y="-5%" width="110%" height="110%">
 *       <feComponentTransfer>
 *         <feFuncR type="table" tableValues="1 0" />
 *       </feComponentTransfer>
 *     </filter>
 *   </defs>
 *   <rect x="20" y="30" width="120" height="50" fill="url(#xml_feFuncR_grad)" />
 *   <text x="20" y="100" fill="black">Source</text>
 *   <rect x="180" y="30" width="120" height="50" fill="url(#xml_feFuncR_grad)" filter="url(#xml_feFuncR_filter)" />
 *   <text x="180" y="100" fill="#c0392b">Red channel inverted</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute     | Default    | Description  |
 * | ------------: | :--------: | :----------- |
 * | `type`        | `identity` | `identity`, `table`, `discrete`, `linear`, or `gamma`. |
 * | `tableValues` | (none)     | List of values for `table` or `discrete`. |
 * | `slope`       | `1`        | Slope for `linear`. |
 * | `intercept`   | `0`        | Intercept for `linear`. |
 * | `amplitude`   | `1`        | Amplitude for `gamma`. |
 * | `exponent`    | `1`        | Exponent for `gamma`. |
 * | `offset`      | `0`        | Offset for `gamma`. |
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
};

}  // namespace donner::svg
