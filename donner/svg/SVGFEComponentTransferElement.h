#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feComponentTransfer "<feComponentTransfer>"
 *
 * Defines a filter primitive that applies per-channel transfer functions (lookup tables)
 * to modify the RGBA components of the input image.
 *
 * - DOM object: SVGFEComponentTransferElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feComponentTransferElement
 *
 * Child elements \ref xml_feFuncR, \ref xml_feFuncG, \ref xml_feFuncB, \ref xml_feFuncA
 * define the transfer function for each channel.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="brighten">
 *   <feComponentTransfer>
 *     <feFuncR type="linear" slope="1.5" />
 *     <feFuncG type="linear" slope="1.5" />
 *     <feFuncB type="linear" slope="1.5" />
 *   </feComponentTransfer>
 * </filter>
 * ```
 *
 * \htmlonly
 * <svg id="xml_feComponentTransfer_diagram" width="320" height="160" viewBox="0 0 320 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <linearGradient id="xml_feComponentTransfer_grad" x1="0" y1="0" x2="1" y2="0">
 *       <stop offset="0%" stop-color="black" />
 *       <stop offset="100%" stop-color="white" />
 *     </linearGradient>
 *     <filter id="xml_feComponentTransfer_filter" x="-5%" y="-5%" width="110%" height="110%">
 *       <feComponentTransfer>
 *         <feFuncR type="table" tableValues="0 0 1 1" />
 *         <feFuncG type="table" tableValues="0 0 1 1" />
 *         <feFuncB type="table" tableValues="0 0 1 1" />
 *       </feComponentTransfer>
 *     </filter>
 *   </defs>
 *   <rect x="20" y="30" width="120" height="50" fill="url(#xml_feComponentTransfer_grad)" />
 *   <text x="20" y="100" fill="black">Source gradient</text>
 *   <rect x="180" y="30" width="120" height="50" fill="url(#xml_feComponentTransfer_grad)" filter="url(#xml_feComponentTransfer_filter)" />
 *   <text x="180" y="100" fill="black">table-remapped (posterized)</text>
 * </svg>
 * \endhtmlonly
 *
 * This element takes no attributes of its own; the per-channel transfer functions are defined
 * by child \ref xml_feFuncR, \ref xml_feFuncG, \ref xml_feFuncB, and \ref xml_feFuncA elements.
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
 */

/**
 * DOM object for a \ref xml_feComponentTransfer element.
 *
 * Performs per-channel remapping of the input image using transfer functions defined by child
 * feFuncR/G/B/A elements.
 */
class SVGFEComponentTransferElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEComponentTransferElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  static SVGFEComponentTransferElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeComponentTransfer;
  /// XML tag name, \ref xml_feComponentTransfer.
  static constexpr std::string_view Tag{"feComponentTransfer"};
};

}  // namespace donner::svg
