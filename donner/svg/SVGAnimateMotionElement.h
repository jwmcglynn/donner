#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_animateMotion &lt;animateMotion&gt;
 * Moves the target along a path while preserving its ordinary transform.
 *
 * - DOM object: SVGAnimateMotionElement
 * - Specification: https://svgwg.org/specs/animations/#AnimateMotionElement
 *
 * `path` accepts SVG path data. Alternatively, `values` or the `from`, `to`, and `by` endpoint
 * forms describe points; a `by`-only animation is additive. A child \ref xml_mpath takes precedence
 * over `path`. Motion is paced by default; `keyPoints` and `keyTimes` control non-paced motion.
 * `rotate` accepts an angle, `auto`, or `auto-reverse`. Timing, additive composition and repeat
 * accumulation are supported.
 *
 * ```xml
 * <circle r="8">
 *   <animateMotion path="M20 20 Q100 120 180 20" dur="3s" repeatCount="indefinite"/>
 * </circle>
 * ```
 */

/// DOM wrapper for the \ref xml_animateMotion element.
class SVGAnimateMotionElement : public SVGElement {
  friend class details::ElementFactory;

protected:
  /// Wrap an existing element.
  explicit SVGAnimateMotionElement(EntityHandle handle) : SVGElement(handle) {}
  /**
   * Initialize the element on the supplied internal handle.
   *
   * @param handle Internal handle to initialize.
   */
  static SVGAnimateMotionElement CreateOn(EntityHandle handle);

public:
  static constexpr ElementType Type = ElementType::AnimateMotion;  ///< Element type.
  static constexpr std::string_view Tag{"animateMotion"};          ///< XML tag.
};

}  // namespace donner::svg
