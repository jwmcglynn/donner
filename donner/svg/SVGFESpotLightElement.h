#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_feSpotLight "<feSpotLight>"
 *
 * `<feSpotLight>` is a directional, cone-shaped light — think of a flashlight or a theatre
 * spotlight. It sits at a position `(x, y, z)` in filter coordinates, points at a target
 * `(pointsAtX, pointsAtY, pointsAtZ)`, and only illuminates surfaces that fall inside a cone
 * around that pointing direction. Pixels outside the cone are unlit.
 *
 * Two attributes control the shape of the cone. `limitingConeAngle` is the half-angle of the
 * cone in degrees — light outside this angle is clipped to black, giving you a hard edge.
 * `specularExponent` (used for both diffuse and specular spotlights, despite its name) controls
 * how quickly the light fades from the bright centre of the cone to its edge: small values
 * (1–5) give a smooth, soft-edged pool of light, and large values (50+) concentrate the light
 * into a tight hotspot. `<feSpotLight>` must appear as a child of \ref xml_feDiffuseLighting or
 * \ref xml_feSpecularLighting — on its own it does nothing. It takes no standard filter
 * primitive attributes.
 *
 * - DOM object: SVGFESpotLightElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feSpotLightElement
 *
 * \par Cone geometry
 *
 * The spotlight sits at `(x, y, z)` and its axis runs through `(pointsAtX, pointsAtY,
 * pointsAtZ)`. `limitingConeAngle` is measured from that axis outward.
 *
 * \htmlonly
 * <svg viewBox="0 0 260 180" width="390" height="270"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <marker id="xml_feSpotLight_explainer_arrow" viewBox="0 0 10 10" refX="9" refY="5"
 *             markerWidth="6" markerHeight="6" orient="auto">
 *       <path d="M0,0 L10,5 L0,10 z" fill="#333"/>
 *     </marker>
 *   </defs>
 *   <!-- surface -->
 *   <line x1="20" y1="150" x2="240" y2="150" stroke="#88a" stroke-width="2"/>
 *   <text x="200" y="168" fill="#555">surface</text>
 *   <!-- cone fill -->
 *   <path d="M 80,40 L 180,150 L 60,150 Z" fill="#ffd" stroke="#c80" stroke-dasharray="3,3"/>
 *   <!-- axis -->
 *   <line x1="80" y1="40" x2="120" y2="150" stroke="#c33" stroke-width="2"
 *         marker-end="url(#xml_feSpotLight_explainer_arrow)"/>
 *   <!-- light -->
 *   <circle cx="80" cy="40" r="6" fill="#fc0" stroke="#a80"/>
 *   <text x="10" y="36" fill="#a80">(x,y,z)</text>
 *   <!-- target -->
 *   <circle cx="120" cy="150" r="4" fill="#c33"/>
 *   <text x="126" y="166" fill="#c33">(pointsAtX, pointsAtY, pointsAtZ)</text>
 *   <!-- angle arc -->
 *   <path d="M 96,75 A 20,20 0 0,0 110,73" fill="none" stroke="#c80"/>
 *   <text x="112" y="72" fill="#c80">limitingConeAngle</text>
 *   <text x="10" y="18" font-weight="bold">spotlight cone</text>
 * </svg>
 * \endhtmlonly
 *
 * \par Rendered examples
 *
 * All examples below light the same blurred-alpha "sphere" bump (a 100 × 100 region with the
 * bump centred at (50, 50)) using \ref xml_feDiffuseLighting. Only the spotlight parameters
 * change.
 *
 * \htmlonly
 * <svg viewBox="0 0 600 140" width="600" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feSpotLight_basic" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <feSpotLight x="10" y="10" z="60"
 *                      pointsAtX="50" pointsAtY="50" pointsAtZ="0"
 *                      specularExponent="5" limitingConeAngle="40"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feSpotLight_narrow" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <feSpotLight x="10" y="10" z="60"
 *                      pointsAtX="50" pointsAtY="50" pointsAtZ="0"
 *                      specularExponent="5" limitingConeAngle="15"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feSpotLight_wide" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <feSpotLight x="10" y="10" z="60"
 *                      pointsAtX="50" pointsAtY="50" pointsAtZ="0"
 *                      specularExponent="5" limitingConeAngle="60"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feSpotLight_sharp" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <feSpotLight x="10" y="10" z="60"
 *                      pointsAtX="50" pointsAtY="50" pointsAtZ="0"
 *                      specularExponent="60" limitingConeAngle="40"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feSpotLight_retarget" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <feSpotLight x="10" y="10" z="60"
 *                      pointsAtX="80" pointsAtY="80" pointsAtZ="0"
 *                      specularExponent="5" limitingConeAngle="30"/>
 *       </feDiffuseLighting>
 *     </filter>
 *   </defs>
 *   <g>
 *     <circle cx="50" cy="50" r="40" fill="black" filter="url(#xml_feSpotLight_basic)"/>
 *     <text x="50" y="120" text-anchor="middle">basic (40°)</text>
 *   </g>
 *   <g transform="translate(120,0)">
 *     <circle cx="50" cy="50" r="40" fill="black" filter="url(#xml_feSpotLight_narrow)"/>
 *     <text x="50" y="120" text-anchor="middle">narrow 15°</text>
 *   </g>
 *   <g transform="translate(240,0)">
 *     <circle cx="50" cy="50" r="40" fill="black" filter="url(#xml_feSpotLight_wide)"/>
 *     <text x="50" y="120" text-anchor="middle">wide 60°</text>
 *   </g>
 *   <g transform="translate(360,0)">
 *     <circle cx="50" cy="50" r="40" fill="black" filter="url(#xml_feSpotLight_sharp)"/>
 *     <text x="50" y="120" text-anchor="middle">sharp exp=60</text>
 *   </g>
 *   <g transform="translate(480,0)">
 *     <circle cx="50" cy="50" r="40" fill="black" filter="url(#xml_feSpotLight_retarget)"/>
 *     <text x="50" y="120" text-anchor="middle">aimed at (80,80)</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur" />
 *   <feDiffuseLighting in="blur" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *     <feSpotLight x="10" y="10" z="60"
 *                  pointsAtX="50" pointsAtY="50" pointsAtZ="0"
 *                  specularExponent="5" limitingConeAngle="40" />
 *   </feDiffuseLighting>
 * </filter>
 * ```
 *
 * The first three examples keep the same light position and target but sweep
 * `limitingConeAngle` from 15° (a tight pencil beam that only grazes a small spot) to 60° (a
 * wide pool that covers most of the bump). The fourth keeps the 40° cone but cranks
 * `specularExponent` from 5 to 60, concentrating the light into a small bright core even though
 * the cone is still wide. The last example keeps the cone narrow but re-aims the spotlight at
 * `(80, 80, 0)` — the hotspot moves to the lower-right.
 *
 * \par Usage
 *
 * `<feSpotLight>` can be used as the child of either \ref xml_feDiffuseLighting (matte shading)
 * or \ref xml_feSpecularLighting (glossy highlights). See those pages for the full lighting
 * pipelines. For a simpler omnidirectional light see \ref xml_fePointLight, and for parallel
 * (directional) light see \ref xml_feDistantLight.
 *
 * \par Attributes
 *
 * | Attribute          | Default | Description |
 * | -----------------: | :-----: | :---------- |
 * | `x`                | `0`     | X coordinate of the light in the filter's coordinate system. |
 * | `y`                | `0`     | Y coordinate of the light. |
 * | `z`                | `0`     | Z coordinate (height above the surface). |
 * | `pointsAtX`        | `0`     | X coordinate of the point the light is aimed at. |
 * | `pointsAtY`        | `0`     | Y coordinate of the aim point. |
 * | `pointsAtZ`        | `0`     | Z coordinate of the aim point. |
 * | `specularExponent` | `1`     | Falloff exponent from the centre of the cone to its edge. Larger values give a tighter, sharper hotspot. |
 * | `limitingConeAngle`| none    | Half-angle of the cone in degrees. Light outside the cone is clipped to black. If omitted, the cone is unbounded. |
 *
 * `<feSpotLight>` does not take the standard filter primitive attributes (`in`, `result`, `x`,
 * `y`, `width`, `height`) — it is purely a child element of a lighting primitive.
 */

/**
 * DOM object for a \ref xml_feSpotLight element.
 *
 * Represents a spotlight at a specific position, pointing toward a target point. The light
 * intensity falls off based on the angle from the light's direction axis, controlled by
 * specularExponent and limitingConeAngle.
 */
class SVGFESpotLightElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFESpotLightElement(EntityHandle handle) : SVGElement(handle) {}

  static SVGFESpotLightElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeSpotLight;
  /// XML tag name, \ref xml_feSpotLight.
  static constexpr std::string_view Tag{"feSpotLight"};
};

}  // namespace donner::svg
