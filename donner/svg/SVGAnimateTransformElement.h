#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_animateTransform &lt;animateTransform&gt;
 *
 * Animates a target element's transform over time.
 *
 * - DOM object: SVGAnimateTransformElement
 * - SVG Animations spec: https://svgwg.org/specs/animations/#AnimateTransformElement
 *
 * @warning Animation support is experimental. Parse with
 * `SVGParser::Options::enableExperimental = true` to create an SVGAnimateTransformElement.
 *
 * The `type` attribute selects `translate`, `scale`, `rotate`, `skewX`, or `skewY`.
 * Donner interpolates `from`/`to`, `from`/`by`, `by`, and semicolon-separated
 * `values` forms.
 *
 * ## Example
 *
 * ```xml
 * <rect x="20" y="20" width="70" height="40" fill="#0f766e">
 *   <animateTransform attributeName="transform" type="rotate"
 *                     from="0 55 40" to="90 55 40"
 *                     dur="2s" fill="freeze" />
 * </rect>
 * ```
 *
 * \htmlonly
 * <svg width="360" height="140" viewBox="0 0 360 140"
 *      style="background-color:white" font-family="sans-serif" font-size="12"
 *      role="img" aria-label="Rotation animation snapshots">
 *   <g transform="translate(20,20)">
 *     <rect width="70" height="40" rx="4" fill="#0f766e"/>
 *     <text x="35" y="76" text-anchor="middle" fill="#334155">0 s</text>
 *   </g>
 *   <g transform="translate(180,40) rotate(45)">
 *     <rect x="-35" y="-20" width="70" height="40" rx="4" fill="#0f766e"/>
 *   </g>
 *   <text x="180" y="116" text-anchor="middle" fill="#334155">1 s</text>
 *   <g transform="translate(320,55) rotate(90)">
 *     <rect x="-35" y="-20" width="70" height="40" rx="4" fill="#0f766e"/>
 *   </g>
 *   <text x="320" y="116" text-anchor="middle" fill="#334155">2 s</text>
 *   <path d="M100 50H137M222 50H267" stroke="#94a3b8" stroke-width="2"/>
 *   <path d="M131 45l10 5-10 5M261 45l10 5-10 5" fill="none"
 *         stroke="#94a3b8" stroke-width="2"/>
 * </svg>
 * \endhtmlonly
 *
 * ## Supported attributes
 *
 * | Attribute | Support |
 * | :-------- | :------ |
 * | `type` | `translate`, `scale`, `rotate`, `skewX`, or `skewY`. |
 * | `from`, `to`, `by`, `values` | Transform keyframes in the selected type's number format. |
 * | `href`, `xlink:href` | Optional target; otherwise the parent element is targeted. |
 * | Shared timing | `begin`, `dur`, `end`, `fill`, `repeatCount`, `repeatDur`, `restart`, `min`, and `max`. |
 *
 * Event and syncbase begin/end expressions are not resolved yet. See \ref xml_animate for shared
 * timing behavior and limitations.
 */

/**
 * DOM object for a \ref xml_animateTransform element.
 */
class SVGAnimateTransformElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGAnimateTransformElement wrapper from an entity.
  explicit SVGAnimateTransformElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGAnimateTransformElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::AnimateTransform;
  /// XML tag name, \ref xml_animateTransform.
  static constexpr std::string_view Tag{"animateTransform"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
