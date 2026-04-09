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
 *
 * Example usage:
 *
 * ```xml
 * <filter id="InvertBlue">
 *   <feComponentTransfer>
 *     <feFuncB type="table" tableValues="1 0" />
 *   </feComponentTransfer>
 * </filter>
 * ```
 *
 * \htmlonly
 * <svg id="xml_feFuncB_diagram" width="320" height="160" viewBox="0 0 320 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <linearGradient id="xml_feFuncB_grad" x1="0" y1="0" x2="1" y2="0">
 *       <stop offset="0%" stop-color="black" />
 *       <stop offset="100%" stop-color="white" />
 *     </linearGradient>
 *     <filter id="xml_feFuncB_filter" x="-5%" y="-5%" width="110%" height="110%">
 *       <feComponentTransfer>
 *         <feFuncB type="table" tableValues="1 0" />
 *       </feComponentTransfer>
 *     </filter>
 *   </defs>
 *   <rect x="20" y="30" width="120" height="50" fill="url(#xml_feFuncB_grad)" />
 *   <text x="20" y="100" fill="black">Source</text>
 *   <rect x="180" y="30" width="120" height="50" fill="url(#xml_feFuncB_grad)" filter="url(#xml_feFuncB_filter)" />
 *   <text x="180" y="100" fill="#2980b9">Blue channel inverted</text>
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
