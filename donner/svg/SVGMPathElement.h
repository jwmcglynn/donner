#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @defgroup xml_mpath "<mpath>"
 *
 * References an external `<path>` element as the motion path for `<animateMotion>`.
 *
 * - DOM object: SVGMPathElement
 * - SVG Animations spec: https://svgwg.org/specs/animations/#MPathElement
 *
 * Example usage:
 * ```xml
 * <path id="motionPath" d="M10,80 Q95,10 180,80" />
 * <circle cx="0" cy="0" r="5" fill="red">
 *   <animateMotion dur="3s">
 *     <mpath href="#motionPath" />
 *   </animateMotion>
 * </circle>
 * ```
 */

/**
 * DOM object for a \ref xml_mpath element.
 *
 * `<mpath>` is a child of `<animateMotion>` that references a `<path>` element via `href`.
 * When present, the referenced path's `d` attribute is used as the motion path, taking
 * precedence over the `path`, `from`/`to`/`by`, and `values` attributes on `<animateMotion>`.
 */
class SVGMPathElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGMPathElement wrapper from an entity.
  explicit SVGMPathElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGMPathElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::MPath;
  /// XML tag name, \ref xml_mpath.
  static constexpr std::string_view Tag{"mpath"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
