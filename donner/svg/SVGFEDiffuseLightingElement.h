#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feDiffuseLighting "<feDiffuseLighting>"
 *
 * `<feDiffuseLighting>` uses the input image's alpha channel as a **height map** — the more
 * opaque a pixel, the taller the imaginary "surface" at that spot — and then computes how a
 * light bouncing off that surface would look using the Lambertian (diffuse) reflection model.
 * The output is a grayscale-tinted image that looks like a matte lit surface: bright where the
 * surface tilts toward the light, dark where it tilts away. (A "height map" is just a
 * grayscale image interpreted as a terrain; a "surface normal" is the imaginary arrow sticking
 * straight out of that terrain at each point, and shading is computed by comparing each
 * normal's direction to the direction of the light.)
 *
 * It must contain exactly one child light source — \ref xml_feDistantLight, \ref
 * xml_fePointLight, or \ref xml_feSpotLight — which tells the primitive where the light is
 * coming from. `<feDiffuseLighting>` on its own just produces a lit grayscale image; to apply
 * the lighting to your actual shape's colors, chain it with \ref xml_feComposite (usually
 * `operator="in"` to mask it against the source, or `operator="arithmetic"` to multiply it
 * onto the source colors). For glossy highlights instead of matte shading, see the companion
 * primitive \ref xml_feSpecularLighting.
 *
 * - DOM object: SVGFEDiffuseLightingElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feDiffuseLightingElement
 *
 * \par A common pitfall: flat alpha = flat lighting
 *
 * Because `<feDiffuseLighting>` reads the *alpha channel* as its height map, feeding it a
 * solid-filled shape (which has alpha = 1.0 across the whole interior) produces a bump with
 * zero height variation — i.e. a perfectly flat plateau — and the result looks uniformly lit,
 * often indistinguishable from an outlined shape. The classic fix is to blur `SourceAlpha`
 * first so the alpha fades smoothly near the edges, producing a rounded bump. Every example on
 * this page uses that `feGaussianBlur` → `feDiffuseLighting` pattern.
 *
 * \par Example 1: basic setup with a distant light
 *
 * A soft sphere-like bump lit from the upper-left with \ref xml_feDistantLight.
 *
 * \htmlonly
 * <svg viewBox="0 0 140 140" width="140" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feDiffuseLighting_ex1" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur"/>
 *       <feDiffuseLighting in="blur" surfaceScale="6" diffuseConstant="1.2" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feDiffuseLighting>
 *     </filter>
 *   </defs>
 *   <circle cx="70" cy="70" r="50" fill="black" filter="url(#xml_feDiffuseLighting_ex1)"/>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <filter id="f" x="-20%" y="-20%" width="140%" height="140%">
 *   <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur" />
 *   <feDiffuseLighting in="blur" surfaceScale="6" diffuseConstant="1.2" lighting-color="white">
 *     <feDistantLight azimuth="45" elevation="60" />
 *   </feDiffuseLighting>
 * </filter>
 * <circle cx="70" cy="70" r="50" fill="black" filter="url(#f)" />
 * ```
 *
 * The black circle is invisible on its own — what you see is the lighting output. The blurred
 * alpha gives `feDiffuseLighting` a smooth dome to shade, so the result looks like a ball lit
 * from the upper-left.
 *
 * \par Example 2: same bump, point light
 *
 * Swapping the distant light for a \ref xml_fePointLight positioned above the bump.
 *
 * \htmlonly
 * <svg viewBox="0 0 140 140" width="140" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feDiffuseLighting_ex2" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur"/>
 *       <feDiffuseLighting in="blur" surfaceScale="6" diffuseConstant="1.2" lighting-color="white">
 *         <fePointLight x="40" y="40" z="40"/>
 *       </feDiffuseLighting>
 *     </filter>
 *   </defs>
 *   <circle cx="70" cy="70" r="50" fill="black" filter="url(#xml_feDiffuseLighting_ex2)"/>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <feDiffuseLighting in="blur" surfaceScale="6" diffuseConstant="1.2" lighting-color="white">
 *   <fePointLight x="40" y="40" z="40" />
 * </feDiffuseLighting>
 * ```
 *
 * Because the point light sits at a specific `(x, y, z)` rather than infinitely far away, the
 * brightest spot is pulled toward wherever the light is, rather than being a pure direction.
 *
 * \par Example 3: same bump, spot light
 *
 * A tight \ref xml_feSpotLight aimed at the centre of the bump — only the middle is lit.
 *
 * \htmlonly
 * <svg viewBox="0 0 140 140" width="140" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feDiffuseLighting_ex3" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="blur"/>
 *       <feDiffuseLighting in="blur" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *         <feSpotLight x="40" y="40" z="80"
 *                      pointsAtX="70" pointsAtY="70" pointsAtZ="0"
 *                      specularExponent="10" limitingConeAngle="15"/>
 *       </feDiffuseLighting>
 *     </filter>
 *   </defs>
 *   <circle cx="70" cy="70" r="50" fill="black" filter="url(#xml_feDiffuseLighting_ex3)"/>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <feDiffuseLighting in="blur" surfaceScale="6" diffuseConstant="1.4" lighting-color="white">
 *   <feSpotLight x="40" y="40" z="80"
 *                pointsAtX="70" pointsAtY="70" pointsAtZ="0"
 *                specularExponent="10" limitingConeAngle="15" />
 * </feDiffuseLighting>
 * ```
 *
 * Outside the 15° cone, pixels are completely unlit (black). This is how you'd produce a
 * theatrical-spotlight effect over only part of a shape.
 *
 * \par Example 4: surfaceScale
 *
 * `surfaceScale` is the multiplier applied to the alpha height map — it controls how tall the
 * bump actually is when the surface normals are computed. Larger values exaggerate the bump
 * and produce more dramatic shading; smaller values give a subtle, nearly-flat look.
 *
 * \htmlonly
 * <svg viewBox="0 0 420 140" width="420" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feDiffuseLighting_scale2" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="2" diffuseConstant="1.2" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feDiffuseLighting_scale6" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.2" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feDiffuseLighting_scale12" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="12" diffuseConstant="1.2" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feDiffuseLighting>
 *     </filter>
 *   </defs>
 *   <g>
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDiffuseLighting_scale2)"/>
 *     <text x="60" y="120" text-anchor="middle">surfaceScale=2</text>
 *   </g>
 *   <g transform="translate(140,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDiffuseLighting_scale6)"/>
 *     <text x="60" y="120" text-anchor="middle">surfaceScale=6</text>
 *   </g>
 *   <g transform="translate(280,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDiffuseLighting_scale12)"/>
 *     <text x="60" y="120" text-anchor="middle">surfaceScale=12</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * \par Example 5: diffuseConstant
 *
 * `diffuseConstant` is a simple overall brightness multiplier (the "kd" in the Lambertian
 * equation). Values below 1.0 dim the output; values above 1.0 brighten it (and may clip).
 *
 * \htmlonly
 * <svg viewBox="0 0 420 140" width="420" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feDiffuseLighting_kd05" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="0.5" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feDiffuseLighting_kd10" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.0" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feDiffuseLighting_kd20" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="2.0" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feDiffuseLighting>
 *     </filter>
 *   </defs>
 *   <g>
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDiffuseLighting_kd05)"/>
 *     <text x="60" y="120" text-anchor="middle">diffuseConstant=0.5</text>
 *   </g>
 *   <g transform="translate(140,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDiffuseLighting_kd10)"/>
 *     <text x="60" y="120" text-anchor="middle">1.0</text>
 *   </g>
 *   <g transform="translate(280,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDiffuseLighting_kd20)"/>
 *     <text x="60" y="120" text-anchor="middle">2.0</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * \par Example 6: lighting-color
 *
 * `lighting-color` tints the output by the color of the light itself. The surface's own
 * colors are not involved here — `<feDiffuseLighting>` always produces the pure lit look;
 * you'd composite it onto your shape separately.
 *
 * \htmlonly
 * <svg viewBox="0 0 420 140" width="420" height="140"
 *      style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feDiffuseLighting_white" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.2" lighting-color="white">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feDiffuseLighting_warm" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.2" lighting-color="#ffcc66">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feDiffuseLighting>
 *     </filter>
 *     <filter id="xml_feDiffuseLighting_blue" x="-20%" y="-20%" width="140%" height="140%">
 *       <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="b"/>
 *       <feDiffuseLighting in="b" surfaceScale="6" diffuseConstant="1.2" lighting-color="#6688ff">
 *         <feDistantLight azimuth="45" elevation="60"/>
 *       </feDiffuseLighting>
 *     </filter>
 *   </defs>
 *   <g>
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDiffuseLighting_white)"/>
 *     <text x="60" y="120" text-anchor="middle">white</text>
 *   </g>
 *   <g transform="translate(140,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDiffuseLighting_warm)"/>
 *     <text x="60" y="120" text-anchor="middle">#ffcc66 (warm)</text>
 *   </g>
 *   <g transform="translate(280,0)">
 *     <circle cx="60" cy="60" r="40" fill="black" filter="url(#xml_feDiffuseLighting_blue)"/>
 *     <text x="60" y="120" text-anchor="middle">#6688ff (blue)</text>
 *   </g>
 * </svg>
 * \endhtmlonly
 *
 * \par What you'd actually use this for
 *
 * `<feDiffuseLighting>` on its own just produces a lit grayscale image. In practice it's
 * chained with \ref xml_feComposite — `<feComposite in2="SourceGraphic" operator="in">` to
 * clip the lighting to the silhouette of your shape, or `<feComposite operator="arithmetic"
 * k1="1" k2="0" k3="0" k4="0">` to multiply the lighting onto your source colors. For glossy
 * highlights added on top, use the companion \ref xml_feSpecularLighting primitive.
 *
 * \par Attributes
 *
 * | Attribute           | Default | Description |
 * | ------------------: | :-----: | :---------- |
 * | `surfaceScale`      | `1`     | Height multiplier applied to the alpha channel. Larger values make the bump appear taller. |
 * | `diffuseConstant`   | `1`     | Lambertian reflectance (kd). Overall brightness multiplier. |
 * | `kernelUnitLength`  | auto    | The step size used when computing surface normals. Two numbers, in filter coordinates. Defaults to one device pixel. |
 * | `lighting-color`    | `white` | Color of the light. Presentation attribute; can be set via CSS. |
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
 */

/**
 * DOM object for a \ref xml_feDiffuseLighting element.
 *
 * Computes diffuse lighting using the input's alpha channel as a height map to generate
 * surface normals, then applies the Phong diffuse lighting equation with the specified
 * light source.
 */
class SVGFEDiffuseLightingElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEDiffuseLightingElement wrapper from an entity.
  explicit SVGFEDiffuseLightingElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEDiffuseLightingElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeDiffuseLighting;
  /// XML tag name, \ref xml_feDiffuseLighting.
  static constexpr std::string_view Tag{"feDiffuseLighting"};
};

}  // namespace donner::svg
