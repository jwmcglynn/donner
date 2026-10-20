#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_mpath &lt;mpath&gt;
 * Supplies a local path reference to a parent \ref xml_animateMotion.
 *
 * - DOM object: SVGMPathElement
 * - Specification: https://svgwg.org/specs/animations/#MPathElement
 *
 * `href="#route"` refers to a path element in the same document; `xlink:href` is also accepted.
 * Changes to the referenced path data are sampled on the next frame. Missing, foreign-namespace,
 * or non-path targets produce no motion instead of falling back to the parent's inline path.
 * External resources are not fetched for motion references.
 */

/// DOM wrapper for the \ref xml_mpath element.
class SVGMPathElement : public SVGElement {
  friend class details::ElementFactory;

protected:
  /// Wrap an existing element.
  explicit SVGMPathElement(EntityHandle handle) : SVGElement(handle) {}
  /**
   * Initialize the element on the supplied internal handle.
   *
   * @param handle Internal handle to initialize.
   */
  static SVGMPathElement CreateOn(EntityHandle handle);

public:
  static constexpr ElementType Type = ElementType::MPath;  ///< Element type.
  static constexpr std::string_view Tag{"mpath"};          ///< XML tag.
};

}  // namespace donner::svg
