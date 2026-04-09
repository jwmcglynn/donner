#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feFlood "<feFlood>"
 *
 * \htmlonly
 * <style>.donner-fe-fig { background-color: white; font-family: sans-serif; font-size: 12px; }</style>
 * \endhtmlonly
 *
 * `<feFlood>` fills a rectangular region with a solid color. Think of it as a filter-graph paint
 * bucket that stamps a flat rectangle of color onto the filter canvas. Unlike most filter
 * primitives, it doesn't take an input image at all — it just produces pixels of a single color
 * and opacity.
 *
 * `<feFlood>` is rarely used alone because painting a solid rectangle on top of your shape simply
 * hides it. Instead, it's almost always combined with another primitive such as \ref
 * xml_feComposite or \ref xml_feMerge to *tint*, *recolor*, or *backdrop* the source graphic.
 *
 * - DOM object: SVGFEFloodElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feFloodElement
 *
 * ## What region does it fill?
 *
 * The flood fills the **filter primitive subregion** — a rectangle defined by the `x`, `y`,
 * `width`, and `height` attributes on the `<feFlood>` element itself (these are inherited from
 * the standard filter primitive attributes). If those attributes are omitted, the flood fills
 * the **entire filter region** of the parent `<filter>` element, which by default extends 10%
 * beyond the source graphic's bounding box on every side.
 *
 * The example below shows a 200×120 shape with a filter that floods orange. Because the
 * filter region extends past the shape, the orange rectangle is actually larger than the shape
 * itself:
 *
 * \htmlonly
 * <svg width="260" height="170" viewBox="0 0 260 170" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feFlood_region_f" x="-10%" y="-10%" width="120%" height="120%">
 *       <feFlood flood-color="orange" flood-opacity="0.6" />
 *     </filter>
 *   </defs>
 *   <rect x="30" y="25" width="200" height="120" fill="none" stroke="#888" stroke-dasharray="3,3"/>
 *   <text x="30" y="18" fill="#666">filter region (flooded)</text>
 *   <rect x="50" y="45" width="160" height="80" fill="steelblue" filter="url(#xml_feFlood_region_f)"/>
 *   <text x="130" y="160" text-anchor="middle" fill="#444">shape inside filter region</text>
 * </svg>
 * \endhtmlonly
 *
 * ## Example 1: flood as the only filter output
 *
 * The simplest possible filter. The `<feFlood>` produces orange, and because no other primitive
 * runs, the output *is* that orange. The original blue rectangle's geometry is completely
 * replaced by the flood rectangle — notice the shape outline is lost:
 *
 * \htmlonly
 * <svg width="260" height="120" viewBox="0 0 260 120" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feFlood_ex1_f">
 *       <feFlood flood-color="orange" flood-opacity="0.6" />
 *     </filter>
 *   </defs>
 *   <rect x="30" y="25" width="90" height="70" fill="steelblue"/>
 *   <text x="75" y="110" text-anchor="middle" fill="#444">no filter</text>
 *   <rect x="150" y="25" width="90" height="70" fill="steelblue" filter="url(#xml_feFlood_ex1_f)"/>
 *   <text x="195" y="110" text-anchor="middle" fill="#444">flood only</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="FloodOnly">
 *   <feFlood flood-color="orange" flood-opacity="0.6" />
 * </filter>
 * <rect width="90" height="70" fill="steelblue" filter="url(#FloodOnly)" />
 * ```
 *
 * ## Example 2: flood + composite to tint a shape
 *
 * This is the canonical use of `<feFlood>`. The flood produces an orange rectangle, then
 * `<feComposite operator="in" in2="SourceGraphic">` keeps the flood *only* where the source
 * graphic is opaque. The result is a version of the shape recolored to orange, preserving its
 * exact silhouette:
 *
 * \htmlonly
 * <svg width="260" height="120" viewBox="0 0 260 120" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feFlood_ex2_f">
 *       <feFlood flood-color="orange" result="f"/>
 *       <feComposite in="f" in2="SourceGraphic" operator="in"/>
 *     </filter>
 *   </defs>
 *   <circle cx="65" cy="60" r="35" fill="steelblue"/>
 *   <text x="65" y="110" text-anchor="middle" fill="#444">original</text>
 *   <circle cx="195" cy="60" r="35" fill="steelblue" filter="url(#xml_feFlood_ex2_f)"/>
 *   <text x="195" y="110" text-anchor="middle" fill="#444">tinted via flood+composite</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="Tint">
 *   <feFlood flood-color="orange" result="f" />
 *   <feComposite in="f" in2="SourceGraphic" operator="in" />
 * </filter>
 * <circle cx="50" cy="50" r="35" fill="steelblue" filter="url(#Tint)" />
 * ```
 *
 * ## Example 3: flood as a backdrop behind a blurred shadow
 *
 * Here the filter first produces a light-yellow backdrop with `<feFlood>`, then blurs the source
 * graphic, then merges the backdrop, the blurred shadow, and the original shape on top. The
 * backdrop becomes a colored card behind the object:
 *
 * \htmlonly
 * <svg width="260" height="140" viewBox="0 0 260 140" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feFlood_ex3_f" x="-20%" y="-20%" width="140%" height="140%">
 *       <feFlood flood-color="#fff3c4" result="bg"/>
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="3" result="blur"/>
 *       <feMerge>
 *         <feMergeNode in="bg"/>
 *         <feMergeNode in="blur"/>
 *         <feMergeNode in="SourceGraphic"/>
 *       </feMerge>
 *     </filter>
 *   </defs>
 *   <text x="130" y="120" text-anchor="middle" fill="#444">flood backdrop + blurred shadow + source</text>
 *   <circle cx="130" cy="60" r="35" fill="tomato" filter="url(#xml_feFlood_ex3_f)"/>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="Card" x="-20%" y="-20%" width="140%" height="140%">
 *   <feFlood flood-color="#fff3c4" result="bg" />
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="3" result="blur" />
 *   <feMerge>
 *     <feMergeNode in="bg" />
 *     <feMergeNode in="blur" />
 *     <feMergeNode in="SourceGraphic" />
 *   </feMerge>
 * </filter>
 * ```
 *
 * ## Attributes
 *
 * | Attribute       | Default     | Description |
 * | --------------: | :---------: | :---------- |
 * | `flood-color`   | `black`     | CSS color used to paint the region. Also accepts CSS `currentColor`. |
 * | `flood-opacity` | `1`         | Number in [0, 1] controlling the alpha of the flood. |
 *
 * Note: the *region* filled by the flood is controlled by the standard primitive attributes
 * (`x`, `y`, `width`, `height`), **not** by these attributes.
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
 */

/**
 * DOM object for a \ref xml_feFlood element.
 *
 * Fills the filter primitive subregion with a solid color specified by `flood-color` and
 * `flood-opacity`. This primitive does not use any input image.
 */
class SVGFEFloodElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEFloodElement wrapper from an entity.
  explicit SVGFEFloodElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEFloodElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeFlood;
  /// XML tag name, \ref xml_feFlood.
  static constexpr std::string_view Tag{"feFlood"};
};

}  // namespace donner::svg
