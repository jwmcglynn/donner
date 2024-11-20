#pragma once
/// @file

#include "donner/svg/SVGElement.h"
#include "donner/svg/core/ClipPathUnits.h"

namespace donner::svg {

/**
 * @page xml_clipPath "<clipPath>"
 *
 * Defines a clipping path, which is used to clip the rendering of other elements using paths and
 * shapes. The clipping path is defined by the child elements of this element. Compared to
 * \ref xml_mask, which uses image-based rendering and their white and black values to determine
 * visibility, "<clipPath>" uses paths and shapes to define the clipping area.
 *
 * This element is not rendered directly, but is referenced by other elements using the `clip-path`
 * CSS property.
 *
 * - DOM object: SVGClipPathElement
 * - SVG2 spec: https://drafts.fxtf.org/css-masking-1/#ClipPathElement
 *
 * ```xml
 * <defs>
 *  <clipPath id="myClipPath">
 *    <circle cx="100" cy="100" r="80"/>
 *    <rect x="100" y="100" width="80" height="80"/>
 *  </clipPath>
 * </defs>
 *
 * <rect x="0" y="0" width="200" height="200" fill="purple" clip-path="url(#myClipPath)"/>
 * ```
 *
 * \htmlonly
 * <svg width="200" height="200" xmlns="http://www.w3.org/2000/svg">
 *   <defs>
 *     <clipPath id="myClipPath">
 *       <circle cx="100" cy="100" r="80"/>
 *       <rect x="100" y="100" width="80" height="80"/>
 *     </clipPath>
 *   </defs>
 *
 *   <rect x="0" y="0" width="200" height="200" fill="purple" clip-path="url(#myClipPath)"/>
 * </svg>
 * \endhtmlonly
 */

/**
 * DOM object for a \ref xml_clipPath element.
 *
 * This element and its children are never rendered directly, but may be referenced by other
 * elements, via `clip-path`.
 */
class SVGClipPathElement : public SVGElement {
  friend class parser::SVGParserImpl;

private:
  /// Create an SVGClipPathElement wrapper from an entity.
  explicit SVGClipPathElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGClipPathElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::ClipPath;
  /// XML tag name, \ref xml_clipPath.
  static constexpr std::string_view Tag{"clipPath"};

  /**
   * Create a new \ref xml_clipPath element.
   *
   * @param document Containing document.
   */
  static SVGClipPathElement Create(SVGDocument& document) {
    return CreateOn(CreateEntity(document));
  }

  /**
   * Get the value of the `"clipPathUnits"` attribute.
   *
   * @return The attribute value.
   */
  ClipPathUnits clipPathUnits() const;

  /**
   * Set the value of the `"clipPathUnits"` attribute.
   *
   * @param value The new attribute value.
   */
  void setClipPathUnits(ClipPathUnits value);
};

}  // namespace donner::svg
