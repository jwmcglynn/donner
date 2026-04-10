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
 *
 * Example usage:
 *
 * ```xml
 * <filter id="FadeAlpha">
 *   <feComponentTransfer>
 *     <feFuncA type="linear" slope="0.5" />
 *   </feComponentTransfer>
 * </filter>
 * ```
 *
 * \htmlonly
 * <svg id="xml_feFuncA_diagram" width="320" height="160" viewBox="0 0 320 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feFuncA_filter" x="-5%" y="-5%" width="110%" height="110%">
 *       <feComponentTransfer>
 *         <feFuncA type="linear" slope="0.4" />
 *       </feComponentTransfer>
 *     </filter>
 *   </defs>
 *   <pattern id="xml_feFuncA_checker" width="16" height="16" patternUnits="userSpaceOnUse">
 *     <rect width="16" height="16" fill="#eee" />
 *     <rect width="8" height="8" fill="#bbb" />
 *     <rect x="8" y="8" width="8" height="8" fill="#bbb" />
 *   </pattern>
 *   <rect x="20" y="30" width="120" height="60" fill="url(#xml_feFuncA_checker)" />
 *   <rect x="20" y="30" width="120" height="60" fill="#e74c3c" />
 *   <text x="20" y="110" fill="black">Source</text>
 *   <rect x="180" y="30" width="120" height="60" fill="url(#xml_feFuncA_checker)" />
 *   <rect x="180" y="30" width="120" height="60" fill="#e74c3c" filter="url(#xml_feFuncA_filter)" />
 *   <text x="180" y="110" fill="black">alpha slope=0.4</text>
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
 * DOM object for a \ref xml_feFuncA element.
 */
class SVGFEFuncAElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEFuncAElement wrapper from an entity.
  explicit SVGFEFuncAElement(EntityHandle handle) : SVGElement(handle) {}

  /// Internal constructor to create the element on an existing \ref donner::Entity.
  static SVGFEFuncAElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeFuncA;
  /// XML tag name, \ref xml_feFuncA.
  static constexpr std::string_view Tag{"feFuncA"};
};

}  // namespace donner::svg
