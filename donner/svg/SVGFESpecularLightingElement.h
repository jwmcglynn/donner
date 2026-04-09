#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feSpecularLighting "<feSpecularLighting>"
 *
 * `<feSpecularLighting>` is the shiny-highlight companion of \ref xml_feDiffuseLighting.
 * Where diffuse lighting gives you the *matte* component of the Phong reflection model — the
 * soft, bright-toward-the-light look of a pool ball — specular lighting gives you the *glossy*
 * component: the small, bright hotspot you see on a wet or metallic surface when a light
 * reflects off it toward your eye. Use it to add shiny highlights to shapes; it's how you get
 * the classic glossy-button look.
 *
 * Like `<feDiffuseLighting>`, it reads the input's alpha channel as a **height map** (more
 * opaque = taller surface at that spot) and uses the resulting surface normals to compute
 * shading. The surface normal is the imaginary arrow sticking out of the terrain at each
 * point; specular lighting is brightest where that arrow points exactly halfway between the
 * light and the viewer. It must contain exactly one child light source — \ref
 * xml_feDistantLight, \ref xml_fePointLight, or \ref xml_feSpotLight — and because the
 * specular output is non-opaque (black everywhere except the highlight), you almost always
 * composite it on top of a diffuse pass or the source graphic with \ref xml_feComposite using
 * `operator="arithmetic"`. As with diffuse lighting, feeding a solid alpha shape to this
 * primitive produces a flat bump, so every example below first blurs `SourceAlpha` to get a
 * smooth, rounded height map.
 *
 * - DOM object: SVGFESpecularLightingElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feSpecularLightingElement
 *
 * \par Example 1: basic specular highlight with a distant light
 *
 * \htmlonly
 * <svg viewBox="0 0 140 140" width="140" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feSpecularLighting_ex1" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur"/>
 *       <feSpecularLighting in="blur" surfaceScale="6" specularConstant="1.2"
 *                           specularExponent="20" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feSpecularLighting>
 *     </filter>
 *   </defs>
 *   <circle cx="70" cy="70" r="50" fill="black" filter="url(#xml_feSpecularLighting_ex1)"/>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="f" x="-20%" y="-20%" width="140%" height="140%">
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur" />
 *   <feSpecularLighting in="blur" surfaceScale="6" specularConstant="1.2"
 *                       specularExponent="20" lighting-color="white">
 *     <feDistantLight azimuth="45" elevation="60" />
 *   </feSpecularLighting>
 * </filter>
 * ```
 *
 * Everything outside the small bright hotspot is black — that's the empty (RGBA=0) background
 * `feSpecularLighting` emits. In real use you'd composite this over something.
 *
 * \par Example 2: same bump, point light
 *
 * Using a \ref xml_fePointLight instead of a distant light — the hotspot sits under wherever
 * the point light is, rather than from a fixed direction.
 *
 * \htmlonly
 * <svg viewBox="0 0 140 140" width="140" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feSpecularLighting_ex2" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur"/>
 *       <feSpecularLighting in="blur" surfaceScale="6" specularConstant="1.2"
 *                           specularExponent="20" lighting-color="white">
 *         <fePointLight x="40" y="40" z="40"/>
 *       </feSpecularLighting>
 *     </filter>
 *   </defs>
 *   <circle cx="70" cy="70" r="50" fill="black" filter="url(#xml_feSpecularLighting_ex2)"/>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <feSpecularLighting in="blur" surfaceScale="6" specularConstant="1.2"
 *                     specularExponent="20" lighting-color="white">
 *   <fePointLight x="40" y="40" z="40" />
 * </feSpecularLighting>
 * ```
 *
 * Moving the point's `(x, y, z)` drags the highlight around the surface — this is the primary
 * knob for making highlights feel interactive. (See \ref xml_feSpotLight for a cone-shaped
 * alternative that clips everything outside a narrow beam.)
 *
 * \par Example 3: specularExponent
 *
 * `specularExponent` is the "shininess" exponent from the Phong model. Small values (1–5)
 * produce a broad, soft sheen; large values (50+) produce a tight, sharp mirror-like highlight.
 *
 * \htmlonly
 * <svg viewBox="0 0 420 140" width="420" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feSpecularLighting_exp1" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feSpecularLighting in="b" surfaceScale="6" specularConstant="1.2"
 *                           specularExponent="1" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feSpecularLighting>
 *     </filter>
 *     <filter id="xml_feSpecularLighting_exp20" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feSpecularLighting in="b" surfaceScale="6" specularConstant="1.2"
 *                           specularExponent="20" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feSpecularLighting>
 *     </filter>
 *     <filter id="xml_feSpecularLighting_exp80" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feSpecularLighting in="b" surfaceScale="6" specularConstant="1.2"
 *                           specularExponent="80" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feSpecularLighting>
 *     </filter>
 *   </defs>
 *   <g>
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feSpecularLighting_exp1)"/>
 *     <text x="60" y="120" text-anchor="middle">specularExponent=1</text>
 *   </g>
 *   <g transform="translate(140,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feSpecularLighting_exp20)"/>
 *     <text x="60" y="120" text-anchor="middle">20</text>
 *   </g>
 *   <g transform="translate(280,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feSpecularLighting_exp80)"/>
 *     <text x="60" y="120" text-anchor="middle">80</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * \par Example 4: specularConstant
 *
 * `specularConstant` is the overall brightness multiplier (the "ks" in Phong). At 0.3 the
 * highlight is a faint sheen; at 2.0 it clips to fully white over a much larger area.
 *
 * \htmlonly
 * <svg viewBox="0 0 420 140" width="420" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feSpecularLighting_ks03" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feSpecularLighting in="b" surfaceScale="6" specularConstant="0.3"
 *                           specularExponent="20" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feSpecularLighting>
 *     </filter>
 *     <filter id="xml_feSpecularLighting_ks10" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feSpecularLighting in="b" surfaceScale="6" specularConstant="1.0"
 *                           specularExponent="20" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feSpecularLighting>
 *     </filter>
 *     <filter id="xml_feSpecularLighting_ks20" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feSpecularLighting in="b" surfaceScale="6" specularConstant="2.0"
 *                           specularExponent="20" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feSpecularLighting>
 *     </filter>
 *   </defs>
 *   <g>
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feSpecularLighting_ks03)"/>
 *     <text x="60" y="120" text-anchor="middle">specularConstant=0.3</text>
 *   </g>
 *   <g transform="translate(140,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feSpecularLighting_ks10)"/>
 *     <text x="60" y="120" text-anchor="middle">1.0</text>
 *   </g>
 *   <g transform="translate(280,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feSpecularLighting_ks20)"/>
 *     <text x="60" y="120" text-anchor="middle">2.0</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * \par Example 5: putting it together — glossy button
 *
 * This is the "why do I care" example. We take a colored circle, run it through \ref
 * xml_feDiffuseLighting to give it matte shading, then add a \ref xml_feSpecularLighting pass
 * for the glossy highlight, and composite both onto the original colored shape. The final
 * result is a classic glossy-button look.
 *
 * \htmlonly
 * <svg viewBox="0 0 140 140" width="140" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feSpecularLighting_button" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur"/>
 *       <feDiffuseLighting in="blur" surfaceScale="6" diffuseConstant="1.2"
 *                          lighting-color="white" result="diffuse">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feDiffuseLighting>
 *       <feComposite in="diffuse" in2="SourceGraphic" operator="arithmetic"
 *                    k1="1" k2="0" k3="0" k4="0" result="litColor"/>
 *       <feSpecularLighting in="blur" surfaceScale="6" specularConstant="1.4"
 *                           specularExponent="30" lighting-color="white" result="spec">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feSpecularLighting>
 *       <feComposite in="spec" in2="SourceAlpha" operator="in" result="specClipped"/>
 *       <feComposite in="specClipped" in2="litColor" operator="arithmetic"
 *                    k1="0" k2="1" k3="1" k4="0"/>
 *     </filter>
 *   </defs>
 *   <circle cx="70" cy="70" r="50" fill="#3377cc" filter="url(#xml_feSpecularLighting_button)"/>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="glossy" x="-20%" y="-20%" width="140%" height="140%">
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur" />
 *
 *   <!-- Matte shading, multiplied into the source color -->
 *   <feDiffuseLighting in="blur" surfaceScale="6" diffuseConstant="1.2"
 *                      lighting-color="white" result="diffuse">
 *     <feDistantLight azimuth="45" elevation="60" />
 *   </feDiffuseLighting>
 *   <feComposite in="diffuse" in2="SourceGraphic" operator="arithmetic"
 *                k1="1" k2="0" k3="0" k4="0" result="litColor" />
 *
 *   <!-- Glossy highlight, added on top -->
 *   <feSpecularLighting in="blur" surfaceScale="6" specularConstant="1.4"
 *                       specularExponent="30" lighting-color="white" result="spec">
 *     <feDistantLight azimuth="45" elevation="60" />
 *   </feSpecularLighting>
 *   <feComposite in="spec" in2="SourceAlpha" operator="in" result="specClipped" />
 *   <feComposite in="specClipped" in2="litColor" operator="arithmetic"
 *                k1="0" k2="1" k3="1" k4="0" />
 * </filter>
 *
 * <circle cx="70" cy="70" r="50" fill="#3377cc" filter="url(#glossy)" />
 * ```
 *
 * Notice that both lighting primitives share the same blurred-alpha input and the same light
 * source parameters — that's what makes the highlight sit in the right place relative to the
 * matte shading. Varying the light's azimuth/elevation rotates both effects together.
 *
 * \par Attributes
 *
 * | Attribute           | Default | Description |
 * | ------------------: | :-----: | :---------- |
 * | `surfaceScale`      | `1`     | Height multiplier applied to the alpha channel. |
 * | `specularConstant`  | `1`     | Specular reflectance (ks). Overall highlight brightness. |
 * | `specularExponent`  | `1`     | Phong shininess exponent. Larger values give a tighter, sharper highlight. |
 * | `kernelUnitLength`  | auto    | The step size used when computing surface normals. Two numbers, in filter coordinates. Defaults to one device pixel. |
 * | `lighting-color`    | `white` | Color of the light. Presentation attribute; can be set via CSS. |
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
 */

/**
 * DOM object for a \ref xml_feSpecularLighting element.
 *
 * Computes specular lighting using the input's alpha channel as a height map. Unlike
 * feDiffuseLighting, the output is non-opaque and intended to be composited with the
 * original image using feComposite with operator="arithmetic".
 */
class SVGFESpecularLightingElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFESpecularLightingElement wrapper from an entity.
  explicit SVGFESpecularLightingElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFESpecularLightingElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeSpecularLighting;
  /// XML tag name, \ref xml_feSpecularLighting.
  static constexpr std::string_view Tag{"feSpecularLighting"};
};

}  // namespace donner::svg
