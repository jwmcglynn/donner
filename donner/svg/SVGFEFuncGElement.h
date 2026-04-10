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
 *
 * Example usage:
 *
 * ```xml
 * <filter id="InvertGreen">
 *   <feComponentTransfer>
 *     <feFuncG type="table" tableValues="1 0" />
 *   </feComponentTransfer>
 * </filter>
 * ```
 *
 * \htmlonly
 * <svg id="xml_feFuncG_diagram" width="320" height="160" viewBox="0 0 320 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <linearGradient id="xml_feFuncG_grad" x1="0" y1="0" x2="1" y2="0">
 *       <stop offset="0%" stop-color="black" />
 *       <stop offset="100%" stop-color="white" />
 *     </linearGradient>
 *     <filter id="xml_feFuncG_filter" x="-5%" y="-5%" width="110%" height="110%">
 *       <feComponentTransfer>
 *         <feFuncG type="table" tableValues="1 0" />
 *       </feComponentTransfer>
 *     </filter>
 *   </defs>
 *   <rect x="20" y="30" width="120" height="50" fill="url(#xml_feFuncG_grad)" />
 *   <text x="20" y="100" fill="black">Source</text>
 *   <rect x="180" y="30" width="120" height="50" fill="url(#xml_feFuncG_grad)" filter="url(#xml_feFuncG_filter)" />
 *   <text x="180" y="100" fill="#27ae60">Green channel inverted</text>
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
 * DOM object for a \ref xml_feFuncG element.
 */
class SVGFEFuncGElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEFuncGElement wrapper from an entity.
  explicit SVGFEFuncGElement(EntityHandle handle) : SVGElement(handle) {}

  /// Internal constructor to create the element on an existing \ref donner::Entity.
  static SVGFEFuncGElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeFuncG;
  /// XML tag name, \ref xml_feFuncG.
  static constexpr std::string_view Tag{"feFuncG"};
};

}  // namespace donner::svg
