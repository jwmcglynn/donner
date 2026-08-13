#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_set &lt;set&gt;
 *
 * Sets an attribute to a discrete value for the duration of an animation.
 *
 * - DOM object: SVGSetElement
 * - SVG Animations spec: https://svgwg.org/specs/animations/#SetElement
 *
 * @warning Animation support is experimental. Parse with
 * `SVGParser::Options::enableExperimental = true` to create an SVGSetElement.
 *
 * `<set>` is the simplest SVG animation element. It sets a target attribute to a specified
 * value when the animation is active. Unlike `<animate>`, it does not interpolate between
 * values.
 *
 * ## Example
 *
 * ```xml
 * <rect x="20" y="20" width="80" height="50" fill="#dc2626">
 *   <set attributeName="fill" to="blue" begin="2s" dur="3s" />
 * </rect>
 * ```
 *
 * \htmlonly
 * <svg width="360" height="120" viewBox="0 0 360 120"
 *      style="background-color:white" font-family="sans-serif" font-size="12"
 *      role="img" aria-label="Set animation before, during, and after its active interval">
 *   <rect x="15" y="20" width="80" height="50" rx="4" fill="#dc2626"/>
 *   <rect x="140" y="20" width="80" height="50" rx="4" fill="#2563eb"/>
 *   <rect x="265" y="20" width="80" height="50" rx="4" fill="#dc2626"/>
 *   <text x="55" y="94" text-anchor="middle" fill="#334155">before: red</text>
 *   <text x="180" y="94" text-anchor="middle" fill="#334155">active: blue</text>
 *   <text x="305" y="94" text-anchor="middle" fill="#334155">after: red</text>
 * </svg>
 * \endhtmlonly
 *
 * ## Supported attributes
 *
 * | Attribute | Support |
 * | :-------- | :------ |
 * | `attributeName` | Attribute to replace during the active interval. |
 * | `to` | Replacement value. |
 * | `href`, `xlink:href` | Optional target; otherwise the parent element is targeted. |
 * | Shared timing | `begin`, `dur`, `end`, `fill`, `repeatCount`, `repeatDur`, `restart`, `min`, and `max`. |
 *
 * The default `fill="remove"` restores the underlying value after the active interval;
 * `fill="freeze"` keeps the replacement. Event and syncbase timing are not resolved yet.
 */

/**
 * DOM object for a \ref xml_set element.
 *
 * ```xml
 * <rect width="100" height="100" fill="red">
 *   <set attributeName="fill" to="blue" begin="2s" dur="3s" />
 * </rect>
 * ```
 */
class SVGSetElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGSetElement wrapper from an entity.
  explicit SVGSetElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGSetElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Set;
  /// XML tag name, \ref xml_set.
  static constexpr std::string_view Tag{"set"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
