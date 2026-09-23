#pragma once
/// @file
/// Shared Slug fill layouts and frozen shader interfaces.
#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Per-draw transform, paint, geometry and clip parameters.
struct alignas(16) SlugFillParams {
  float mvp[16];                 //!< Column-major clip-from-target matrix.
  float patternFromPath[16];     //!< Column-major pattern-from-path matrix.
  float viewport[2];             //!< Target dimensions in pixels.
  float tileSize[2];             //!< Pattern tile dimensions.
  uint32_t hasClipPolygon;       //!< Enables the four clip half-planes.
  uint32_t hasClipMask;          //!< Enables sampled clip coverage.
  uint32_t antialias;            //!< Zero selects binary winding coverage.
  uint32_t _pad1;                //!< Reserved layout padding.
  float clipPolygonPlanes[16];   //!< Four inward pixel-space half-planes.
  float color[4];                //!< Premultiplied fill color.
  uint32_t fillRule;             //!< Zero nonzero, one even-odd.
  uint32_t paintMode;            //!< Solid, pattern, linear or radial paint selector.
  float patternOpacity;          //!< Pattern alpha multiplier.
  uint32_t _pad2;                //!< Reserved layout padding.
  float gridYBase;               //!< Horizontal-band grid origin.
  float gridHStride;             //!< Horizontal-band grid step.
  uint32_t gridHBandCount;       //!< Horizontal-band count.
  float gridXBase;               //!< Vertical-band grid origin.
  float gridVStride;             //!< Vertical-band grid step.
  uint32_t gridVBandCount;       //!< Vertical-band count.
  uint32_t boundingVertexCount;  //!< Number of convex enclosure vertices.
  uint32_t _gridPad0;            //!< Reserved layout padding.
  uint32_t bandBase;             //!< Base element in horizontal bands.
  uint32_t curveBase;            //!< Base element in horizontal curve scalars.
  uint32_t vBandBase;            //!< Base element in vertical bands.
  uint32_t vCurveBase;           //!< Base element in vertical curve scalars.
  uint32_t hGridBase;            //!< Base element in horizontal grid.
  uint32_t vGridBase;            //!< Base element in vertical grid.
  uint32_t hRefsBase;            //!< Base element in horizontal curve references.
  uint32_t vRefsBase;            //!< Base element in vertical curve references.
  float boundingVertices[16];    //!< Two path-space corners per vec4.
  float clipRect[4];             //!< Half-open pixel-space clip rectangle.
  uint32_t clipRectActive;       //!< Enables the instance clip rectangle.
  uint32_t paintBase;            //!< Base element in gradient paint rows.
  uint32_t gradientSpread;       //!< Pad, reflect or repeat spread selector.
  uint32_t gradientStopCount;    //!< Number of active gradient stops.
  float pathFromPixel[4];        //!< Inverse linear transform, two columns, pixel to path.
  float pixelOrigin[2];          //!< Integer pixel the mapping is taken relative to.
  float pathOffset[2];           //!< Path position of `pixelOrigin`'s fractional remainder.
};
static_assert(sizeof(SlugFillParams) == 448);
/// One painter-ordered instance, including its nested transform and bounding polygon.
struct alignas(16) SlugFillInstance {
  float transformRow0[4];         //!< First affine row: a, c, e, padding.
  float transformRow1[4];         //!< Second affine row: b, d, f, padding.
  float color[4];                 //!< Premultiplied fill color.
  uint32_t fillRule;              //!< Zero nonzero, one even-odd.
  uint32_t paintMode;             //!< Solid, pattern, linear or radial paint selector.
  float patternOpacity;           //!< Pattern alpha multiplier.
  uint32_t _pad0;                 //!< Reserved layout padding.
  float gridYBase;                //!< Horizontal-band grid origin.
  float gridHStride;              //!< Horizontal-band grid step.
  uint32_t gridHBandCount;        //!< Horizontal-band count.
  float gridXBase;                //!< Vertical-band grid origin.
  float gridVStride;              //!< Vertical-band grid step.
  uint32_t gridVBandCount;        //!< Vertical-band count.
  float pixelOrigin[2];           //!< Integer pixel this instance's mapping is relative to.
  uint32_t boundingVertexCount;   //!< Number of convex enclosure vertices.
  uint32_t pixelMappingSource;    //!< Nonzero when this record carries its own pixel mapping.
  float pathOffset[2];            //!< Path position of `pixelOrigin`'s fractional remainder.
  float boundingVertices[4 * 4];  //!< Two path-space corners per vec4.
  uint32_t bandBase;              //!< Base element in horizontal bands.
  uint32_t curveBase;             //!< Base element in horizontal curve scalars.
  uint32_t vBandBase;             //!< Base element in vertical bands.
  uint32_t vCurveBase;            //!< Base element in vertical curve scalars.
  uint32_t hGridBase;             //!< Base element in horizontal grid.
  uint32_t vGridBase;             //!< Base element in vertical grid.
  uint32_t hRefsBase;             //!< Base element in horizontal curve references.
  uint32_t vRefsBase;             //!< Base element in vertical curve references.
  float clipRect[4];              //!< Half-open pixel-space clip rectangle.
  uint32_t clipRectActive;        //!< Enables the instance clip rectangle.
  uint32_t paintBase;             //!< Base element in gradient paint rows.
  uint32_t gradientSpread;        //!< Pad, reflect or repeat spread selector.
  uint32_t gradientStopCount;     //!< Number of active gradient stops.
  float pathFromPixel[4];         //!< Inverse linear transform, two columns, pixel to path.
};
static_assert(sizeof(SlugFillInstance) == 256);
/// A band indexes the corresponding curve-reference range.
struct SlugFillBand {
  uint32_t curveStart;  //!< First curve-reference element.
  uint32_t curveCount;  //!< Number of curve references.
};
/**
 * Pixel-to-path mapping a Slug fragment applies to its own pixel center `p`:
 * `pathFromPixel * (p - pixelOrigin) + pathOffset`, with `pathFromPixel` holding the inverse
 * linear transform's two columns. The fill, gradient and mask shaders all take it in this form.
 */
struct SlugPixelMapping {
  float pathFromPixel[4] = {};  //!< Inverse linear transform, two columns, pixel to path.
  float pixelOrigin[2] = {};    //!< Integer pixel the mapping is taken relative to.
  float pathOffset[2] = {};     //!< Path position of the translation's fractional remainder.
};

/// Largest translation, in pixels, split off as an integer pixel origin. A pixel center is a
/// multiple of one half inside a target at most tens of thousands of pixels wide, so it minus an
/// integer origin below 2^22 is a multiple of one half below 2^23, which float32 represents
/// exactly.
inline constexpr double kSlugMaxExactPixelOrigin = 4194304.0;

/**
 * Computes the mapping for the path-to-target-pixel affine `x' = a x + c y + e`,
 * `y' = b x + d y + f`, in double precision.
 *
 * The translation splits into an integer pixel origin and a fractional remainder. The shader
 * subtracts the origin from the pixel center exactly, and every other input depends only on the
 * linear part and that remainder. Draws whose translations differ by an integer, with the sum
 * exact in double as it is for every atlas or tile placement of an ordinary draw, therefore map
 * corresponding pixels to the same path positions whatever their target. A translation too large
 * to split stays whole in the offset, as precise as the float path data it is compared with.
 *
 * A transform without an inverse, or one whose inverse float cannot hold, gets an all-zero
 * mapping, and the shaders cover no pixel of a draw that carries it. The draw's enclosure is built
 * on the GPU from float axes that rounding can leave slightly invertible, so it can still
 * rasterize a sliver of pixels, and only that rule keeps them empty.
 */
inline SlugPixelMapping ComputeSlugPixelMapping(double a, double b, double c, double d, double e,
                                                double f) {
  SlugPixelMapping mapping;
  const double determinant = a * d - b * c;
  if (!std::isfinite(determinant) || determinant == 0.0) {
    return mapping;
  }
  const double pathXFromPixelX = d / determinant;
  const double pathYFromPixelX = -b / determinant;
  const double pathXFromPixelY = -c / determinant;
  const double pathYFromPixelY = a / determinant;
  const auto integerOrigin = [](double translation) {
    return std::isfinite(translation) && std::abs(translation) < kSlugMaxExactPixelOrigin
               ? std::floor(translation)
               : 0.0;
  };
  const double originX = integerOrigin(e);
  const double originY = integerOrigin(f);
  const double remainderX = e - originX;
  const double remainderY = f - originY;
  mapping.pathFromPixel[0] = static_cast<float>(pathXFromPixelX);
  mapping.pathFromPixel[1] = static_cast<float>(pathYFromPixelX);
  mapping.pathFromPixel[2] = static_cast<float>(pathXFromPixelY);
  mapping.pathFromPixel[3] = static_cast<float>(pathYFromPixelY);
  mapping.pixelOrigin[0] = static_cast<float>(originX);
  mapping.pixelOrigin[1] = static_cast<float>(originY);
  mapping.pathOffset[0] =
      static_cast<float>(-(pathXFromPixelX * remainderX + pathXFromPixelY * remainderY));
  mapping.pathOffset[1] =
      static_cast<float>(-(pathYFromPixelX * remainderX + pathYFromPixelY * remainderY));
  for (const float value :
       {mapping.pathFromPixel[0], mapping.pathFromPixel[1], mapping.pathFromPixel[2],
        mapping.pathFromPixel[3], mapping.pathOffset[0], mapping.pathOffset[1]}) {
    if (!std::isfinite(value)) {
      return SlugPixelMapping();
    }
  }
  return mapping;
}

/// Writes `mapping` into any parameter block carrying the three mapping fields.
template <typename ParamsT>
void WriteSlugPixelMapping(ParamsT& params, const SlugPixelMapping& mapping) {
  for (int i = 0; i < 4; ++i) {
    params.pathFromPixel[i] = mapping.pathFromPixel[i];
  }
  for (int i = 0; i < 2; ++i) {
    params.pixelOrigin[i] = mapping.pixelOrigin[i];
    params.pathOffset[i] = mapping.pathOffset[i];
  }
}

/// Returns a stable view of the authored WGSL and reflected interface.
const CompiledShaderView& SlugFillShader();
/// Returns a stable view containing only the platform-native projection.
const CompiledShaderView& SlugFillNativeShader();
}  // namespace donner::gpu::shader::programs
