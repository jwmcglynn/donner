#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_feDistantLight "<feDistantLight>"
 *
 * `<feDistantLight>` is a parallel light source — think of the sun. All its light rays are
 * parallel, so every pixel on the surface gets hit at the same angle, and the light has no
 * "position" at all, only a direction. Because of this, moving the lit object around the canvas
 * doesn't change the shading at all — only rotating the light does.
 *
 * The direction is defined by two angles: `azimuth` (horizontal rotation around the surface, where
 * 0° points along the +X axis / "east", 90° points along the +Y axis / "south") and `elevation`
 * (vertical angle above the XY plane, where 0° is a grazing ray parallel to the surface and 90° is
 * a ray coming straight down). `<feDistantLight>` must appear as a child of \ref
 * xml_feDiffuseLighting or \ref xml_feSpecularLighting — on its own it does nothing. It takes no
 * standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`); only
 * `azimuth` and `elevation`.
 *
 * - DOM object: SVGFEDistantLightElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feDistantLightElement
 *
 * \par Azimuth and elevation
 *
 * This explainer diagram shows the two angles. Azimuth sweeps around the compass in the XY plane;
 * elevation tilts the light up from the surface toward the viewer.
 *
 * \htmlonly
 * <svg viewBox="0 0 320 200" width="480" height="300"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <marker id="xml_feDistantLight_explainer_arrow" viewBox="0 0 10 10" refX="9" refY="5"
 *             markerWidth="6" markerHeight="6" orient="auto">
 *       <path d="M0,0 L10,5 L0,10 z" fill="#333"/>
 *     </marker>
 *   </defs>
 *   <!-- Azimuth (left) -->
 *   <g transform="translate(80,110)">
 *     <ellipse cx="0" cy="0" rx="60" ry="20" fill="#eef" stroke="#88a"/>
 *     <line x1="-70" y1="0" x2="70" y2="0" stroke="#aaa" stroke-dasharray="2,2"/>
 *     <line x1="0" y1="-30" x2="0" y2="30" stroke="#aaa" stroke-dasharray="2,2"/>
 *     <text x="62" y="-4" fill="#555">+X (az=0)</text>
 *     <text x="4" y="42" fill="#555">+Y (az=90)</text>
 *     <line x1="0" y1="0" x2="45" y2="15" stroke="#c33" stroke-width="2"
 *           marker-end="url(#xml_feDistantLight_explainer_arrow)"/>
 *     <path d="M 20,0 A 20,6 0 0,1 19,6" fill="none" stroke="#c33"/>
 *     <text x="22" y="-6" fill="#c33">azimuth</text>
 *     <text x="-60" y="-30" font-weight="bold">azimuth (top view)</text>
 *   </g>
 *   <!-- Elevation (right) -->
 *   <g transform="translate(220,140)">
 *     <line x1="-70" y1="0" x2="70" y2="0" stroke="#88a" stroke-width="2"/>
 *     <text x="60" y="14" fill="#555">surface</text>
 *     <line x1="0" y1="0" x2="50" y2="-40" stroke="#c33" stroke-width="2"
 *           marker-end="url(#xml_feDistantLight_explainer_arrow)"/>
 *     <path d="M 25,0 A 25,25 0 0,0 20,-15" fill="none" stroke="#c33"/>
 *     <text x="28" y="-18" fill="#c33">elevation</text>
 *     <text x="-65" y="-60" font-weight="bold">elevation (side view)</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * \par Rendered examples
 *
 * All examples below light the same blurred-alpha "sphere" bump using \ref xml_feDiffuseLighting
 * and only vary the `azimuth` / `elevation` of the distant light. Compare how the bright spot
 * moves across the bump as the angles change.
 *
 * \htmlonly
 * <svg viewBox="0 0 600 140" width="600" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feDistantLight_az0_el45" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <feDistantLight azimuth="0" elevation="45"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feDistantLight_az90_el45" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <feDistantLight azimuth="90" elevation="45"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feDistantLight_az180_el45" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <feDistantLight azimuth="180" elevation="45"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feDistantLight_az0_el15" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <feDistantLight azimuth="0" elevation="15"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feDistantLight_az0_el80" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <feDistantLight azimuth="0" elevation="80"/>
 *       </feDiffuseLighting>
 *     </filter>
 *   </defs>
 *   <g>
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDistantLight_az0_el45)"/>
 *     <text x="60" y="120" text-anchor="middle">az=0 el=45</text>
 *   </g>
 *   <g transform="translate(120,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDistantLight_az90_el45)"/>
 *     <text x="60" y="120" text-anchor="middle">az=90 el=45</text>
 *   </g>
 *   <g transform="translate(240,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDistantLight_az180_el45)"/>
 *     <text x="60" y="120" text-anchor="middle">az=180 el=45</text>
 *   </g>
 *   <g transform="translate(360,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDistantLight_az0_el15)"/>
 *     <text x="60" y="120" text-anchor="middle">az=0 el=15</text>
 *   </g>
 *   <g transform="translate(480,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDistantLight_az0_el80)"/>
 *     <text x="60" y="120" text-anchor="middle">az=0 el=80</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur" />
 *   <feDiffuseLighting in="blur" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *     <feDistantLight azimuth="0" elevation="45" />
 *   </feDiffuseLighting>
 * </filter>
 * ```
 *
 * At `elevation=15` the light grazes across the bump and picks out a thin crescent; at
 * `elevation=80` the light comes almost straight down and the whole bump lights up roughly
 * uniformly. Swinging `azimuth` from 0° to 180° walks the bright spot across the bump from left to
 * right.
 *
 * \par Usage
 *
 * `<feDistantLight>` can be used as the child of either \ref xml_feDiffuseLighting (matte shading)
 * or \ref xml_feSpecularLighting (glossy highlights). See those pages for the full lighting
 * pipelines. For positional lights, see \ref xml_fePointLight and \ref xml_feSpotLight.
 *
 * \par Attributes
 *
 * | Attribute   | Default | Description |
 * | ----------: | :-----: | :---------- |
 * | `azimuth`   | `0`     | Direction angle in degrees, measured clockwise in the XY plane from the +X axis (0 = east, 90 = south). |
 * | `elevation` | `0`     | Angle in degrees above the XY plane (0 = grazing the surface, 90 = straight down). |
 *
 * `<feDistantLight>` does not take the standard filter primitive attributes (`in`, `result`, `x`,
 * `y`, `width`, `height`) — it is purely a child element of a lighting primitive.
 */

/**
 * DOM object for a \ref xml_feDistantLight element.
 *
 * Represents a light source at infinite distance, producing parallel light rays. The direction
 * is specified by azimuth (angle in the XY plane) and elevation (angle above the XY plane).
 */
class SVGFEDistantLightElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEDistantLightElement wrapper from an entity.
  explicit SVGFEDistantLightElement(EntityHandle handle) : SVGElement(handle) {}

  /// Internal constructor to create the element on an existing \ref donner::Entity.
  static SVGFEDistantLightElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeDistantLight;
  /// XML tag name, \ref xml_feDistantLight.
  static constexpr std::string_view Tag{"feDistantLight"};
};

}  // namespace donner::svg
