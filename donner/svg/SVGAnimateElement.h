#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_animate &lt;animate&gt;
 *
 * Animates a target attribute over time. Donner currently supports linear and discrete
 * interpolation for numeric values, number lists, compatible path data, and values that can be
 * represented by the target attribute parser. Non-interpolable values fall back to discrete
 * sampling.
 *
 * - DOM object: SVGAnimateElement
 * - SVG Animations spec: https://svgwg.org/specs/animations/#AnimateElement
 *
 * @warning Animation support is experimental. Parse with
 * `SVGParser::Options::enableExperimental = true` to create an SVGAnimateElement. Without that
 * option, the parser preserves `<animate>` as an unknown element.
 *
 * ## Example
 *
 * ```xml
 * <circle cx="30" cy="40" r="18" fill="#2563eb">
 *   <animate attributeName="cx" from="30" to="210"
 *            dur="2s" fill="freeze" />
 * </circle>
 * ```
 *
 * The snapshots below show the result at the beginning, midpoint, and end of the active interval:
 *
 * \htmlonly
 * <svg width="360" height="120" viewBox="0 0 360 120"
 *      style="background-color:white" font-family="sans-serif" font-size="12"
 *      role="img" aria-label="Three animation snapshots from left to right">
 *   <path d="M62 44H168M192 44H298" stroke="#94a3b8" stroke-width="2"/>
 *   <path d="M162 39l10 5-10 5M292 39l10 5-10 5" fill="none"
 *         stroke="#94a3b8" stroke-width="2"/>
 *   <circle cx="40" cy="44" r="18" fill="#2563eb"/>
 *   <circle cx="180" cy="44" r="18" fill="#2563eb"/>
 *   <circle cx="320" cy="44" r="18" fill="#2563eb"/>
 *   <text x="40" y="88" text-anchor="middle" fill="#334155">0 s: cx=30</text>
 *   <text x="180" y="88" text-anchor="middle" fill="#334155">1 s: cx=120</text>
 *   <text x="320" y="88" text-anchor="middle" fill="#334155">2 s: cx=210</text>
 * </svg>
 * \endhtmlonly
 *
 * ## Supported attributes
 *
 * | Attribute | Support |
 * | :-------- | :------ |
 * | `attributeName` | Attribute to animate. Required for an override. |
 * | `from`, `to`, `by` | Endpoint forms. `values` takes precedence when present. |
 * | `values` | Semicolon-separated keyframe values. |
 * | `calcMode` | `linear` and `discrete`. `paced` and `spline` currently use linear sampling. |
 * | `keyTimes` | Normalized keyframe times when its count matches `values`. |
 * | `href`, `xlink:href` | Optional target; otherwise the parent element is targeted. |
 * | `begin`, `dur`, `end`, `fill` | Active-interval timing. |
 * | `repeatCount`, `repeatDur`, `restart`, `min`, `max` | Repeat and active-duration controls. |
 *
 * `begin` and `end` accept offset clock values, including the earliest resolvable value in a
 * semicolon-separated list. Event and syncbase timing expressions are preserved but do not yet
 * resolve. The default `fill="remove"` restores the underlying value; `fill="freeze"` retains
 * the last sampled value.
 */

/**
 * DOM object for a \ref xml_animate element.
 *
 * ```xml
 * <circle cx="50" cy="50" r="40">
 *   <animate attributeName="cx" from="50" to="200" dur="3s" fill="freeze" />
 * </circle>
 * ```
 */
class SVGAnimateElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGAnimateElement wrapper from an entity.
  explicit SVGAnimateElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGAnimateElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Animate;
  /// XML tag name, \ref xml_animate.
  static constexpr std::string_view Tag{"animate"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
