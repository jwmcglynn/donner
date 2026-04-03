#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @defgroup xml_feImage "<feImage>"
 *
 * Fetches image data from an external resource or renders a referenced SVG element, and provides
 * the pixel data as the filter primitive output.
 *
 * - DOM object: SVGFEImageElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feImageElement
 *
 * This element is defined within a \ref xml_filter element, and is combined with other filter
 * primitives to define a filter applied on the input image.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feImage href="texture.png" />
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feImage element.
 *
 * Fetches image data from an external resource (via `href`) or renders a referenced SVG element
 * fragment, providing the result as filter output.
 */
class SVGFEImageElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEImageElement wrapper from an entity.
  explicit SVGFEImageElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEImageElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeImage;
  /// XML tag name, \ref xml_feImage.
  static constexpr std::string_view Tag{"feImage"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
