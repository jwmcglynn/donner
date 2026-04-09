#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_fePointLight "<fePointLight>"
 *
 * `<fePointLight>` is a positional light source — think of a bare light bulb floating in space.
 * It emits light in all directions from a single `(x, y, z)` point in the filter's coordinate
 * system. Unlike \ref xml_feDistantLight (which has a single direction everywhere), the direction
 * of a point light is different at every pixel of the surface, because each pixel sits at a
 * different angle relative to the bulb.
 *
 * The `z` coordinate controls how "high above" the surface the light sits. A large `z` puts the
 * bulb far away so the illumination is soft and nearly uniform; a small `z` puts the bulb right up
 * against the surface so you get a small, bright hotspot with harsh falloff toward the edges.
 * `<fePointLight>` must appear as a child of \ref xml_feDiffuseLighting or \ref
 * xml_feSpecularLighting — on its own it does nothing. It takes no standard filter primitive
 * attributes.
 *
 * - DOM object: SVGFEPointLightElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#fePointLightElement
 *
 * \par Coordinate system
 *
 * `x` and `y` are in the filter's user-space coordinates (the same space your shape is drawn in),
 * and `z` points out of the screen toward the viewer.
 *
 * \htmlonly
 * <svg viewBox="0 0 260 180" width="390" height="270"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <marker id="xml_fePointLight_explainer_arrow" viewBox="0 0 10 10" refX="9" refY="5"
 *             markerWidth="6" markerHeight="6" orient="auto">
 *       <path d="M0,0 L10,5 L0,10 z" fill="#333"/>
 *     </marker>
 *   </defs>
 *   <!-- surface plane (parallelogram) -->
 *   <path d="M 40,130 L 200,130 L 240,90 L 80,90 Z" fill="#eef" stroke="#88a"/>
 *   <!-- axes -->
 *   <line x1="40" y1="130" x2="210" y2="130" stroke="#333"
 *         marker-end="url(#xml_fePointLight_explainer_arrow)"/>
 *   <text x="214" y="134" fill="#333">+X</text>
 *   <line x1="40" y1="130" x2="80" y2="90" stroke="#333"
 *         marker-end="url(#xml_fePointLight_explainer_arrow)"/>
 *   <text x="82" y="90" fill="#333">+Y</text>
 *   <line x1="120" y1="110" x2="120" y2="30" stroke="#333"
 *         marker-end="url(#xml_fePointLight_explainer_arrow)"/>
 *   <text x="124" y="30" fill="#333">+Z</text>
 *   <!-- bulb -->
 *   <circle cx="120" cy="40" r="7" fill="#fc0" stroke="#a80"/>
 *   <text x="132" y="44" fill="#a80">(x, y, z)</text>
 *   <!-- diverging rays -->
 *   <line x1="120" y1="40" x2="70" y2="120" stroke="#fc0" stroke-dasharray="3,3"/>
 *   <line x1="120" y1="40" x2="120" y2="115" stroke="#fc0" stroke-dasharray="3,3"/>
 *   <line x1="120" y1="40" x2="180" y2="118" stroke="#fc0" stroke-dasharray="3,3"/>
 *   <text x="10" y="18" font-weight="bold">point light in filter space</text>
 * </svg>
 * \endhtmlonly
 *
 * \par Rendered examples
 *
 * All examples below light the same blurred-alpha "sphere" bump (roughly 100 × 100 user units
 * with the bump centred around x=50, y=50) using \ref xml_feDiffuseLighting. Only the point
 * light's `(x, y, z)` changes.
 *
 * \htmlonly
 * <svg viewBox="0 0 560 140" width="560" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_fePointLight_high_center" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <fePointLight x="50" y="50" z="100"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_fePointLight_close_center" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <fePointLight x="50" y="50" z="20"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_fePointLight_upleft" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <fePointLight x="20" y="20" z="50"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_fePointLight_downright" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <fePointLight x="80" y="80" z="50"/>
 *       </feDiffuseLighting>
 *     </filter>
 *   </defs>
 *   <g>
 *     <circle cx="50" cy="50" r="40" fill="black" filter="url(#xml_fePointLight_high_center)"/>
 *     <text x="60" y="120" text-anchor="middle">(50,50,100)</text>
 *   </g>
 *   <g transform="translate(140,0)">
 *     <circle cx="50" cy="50" r="40" fill="black" filter="url(#xml_fePointLight_close_center)"/>
 *     <text x="60" y="120" text-anchor="middle">(50,50,20) close</text>
 *   </g>
 *   <g transform="translate(280,0)">
 *     <circle cx="50" cy="50" r="40" fill="black" filter="url(#xml_fePointLight_upleft)"/>
 *     <text x="60" y="120" text-anchor="middle">(20,20,50) up-left</text>
 *   </g>
 *   <g transform="translate(420,0)">
 *     <circle cx="50" cy="50" r="40" fill="black" filter="url(#xml_fePointLight_downright)"/>
 *     <text x="60" y="120" text-anchor="middle">(80,80,50) down-right</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur" />
 *   <feDiffuseLighting in="blur" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *     <fePointLight x="50" y="50" z="100" />
 *   </feDiffuseLighting>
 * </filter>
 * ```
 *
 * Compare the centred lights: at `z=100` the whole bump is lit roughly evenly with a gentle
 * centre-brightness; at `z=20` the bulb has moved down almost onto the surface, producing a
 * harsh, pinpoint-bright centre with the edges falling into shadow. Moving `(x, y)` off-centre
 * walks the hotspot around the bump, the same way moving a flashlight over a ball would.
 *
 * \par Usage
 *
 * `<fePointLight>` can be used as the child of either \ref xml_feDiffuseLighting (matte shading)
 * or \ref xml_feSpecularLighting (glossy highlights). See those pages for the full lighting
 * pipelines. For parallel light (no position, only direction) see \ref xml_feDistantLight, and
 * for a cone-shaped light see \ref xml_feSpotLight.
 *
 * \par Attributes
 *
 * | Attribute | Default | Description |
 * | --------: | :-----: | :---------- |
 * | `x`       | `0`     | X coordinate of the light in the filter's coordinate system. |
 * | `y`       | `0`     | Y coordinate of the light. |
 * | `z`       | `0`     | Z coordinate (height above the surface). Larger values produce softer, more uniform light; smaller values produce a bright hotspot with sharp falloff. |
 *
 * `<fePointLight>` does not take the standard filter primitive attributes (`in`, `result`, `x`,
 * `y`, `width`, `height`) — it is purely a child element of a lighting primitive.
 */

/**
 * DOM object for a \ref xml_fePointLight element.
 *
 * Represents a light source at a specific position in 3D space. Light direction varies
 * per pixel based on the distance from the light to the surface point.
 */
class SVGFEPointLightElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEPointLightElement(EntityHandle handle) : SVGElement(handle) {}

  static SVGFEPointLightElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FePointLight;
  /// XML tag name, \ref xml_fePointLight.
  static constexpr std::string_view Tag{"fePointLight"};
};

}  // namespace donner::svg
