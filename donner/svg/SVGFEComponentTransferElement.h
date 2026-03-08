#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @defgroup xml_feComponentTransfer "<feComponentTransfer>"
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
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
